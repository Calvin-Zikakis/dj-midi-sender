// State-machine tests for prolink::Bridge, driven through fake sockets and a
// fake clock sink. Every behaviour here was either hard-won on real hardware or
// is a bug we actually shipped and fixed — master handoff in both directions,
// tempo authority, bar alignment, re-sync, start/stop policy.
//
// Bridge::run() is a blocking poll loop, so each test runs it on a thread and
// interacts through the fakes. The bridge reads real wall-clock time, so tests
// use short configured timeouts and wait_for() rather than fixed sleeps.

#include "bridge.hpp"
#include "packets.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

void check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s\n", file, line, expr);
    }
}
#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

void check_eq(long long got, long long want, const char* what,
              const char* file, int line) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s — got %lld, want %lld\n",
                    file, line, what, got, want);
    }
}
#define CHECK_EQ(got, want) check_eq((long long)(got), (long long)(want), #got, __FILE__, __LINE__)

void section(const char* name) { std::printf("%s\n", name); std::fflush(stdout); }

// Spin until `pred` holds or the deadline passes. Returns whether it held —
// so a failure reports as a normal CHECK rather than hanging the suite.
bool wait_for(std::function<bool()> pred, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// ── Fakes ──────────────────────────────────────────────────────────────────

struct SentPacket {
    std::vector<uint8_t> bytes;
    uint32_t ip;
    uint16_t port;
    uint8_t type() const { return bytes.size() > 0x0A ? bytes[0x0A] : 0; }
};

class FakeSocket : public prolink::IUdpSocket {
public:
    int recv(uint8_t* buf, size_t len, uint32_t timeout_ms,
             uint32_t* src_ip = nullptr) override {
        std::unique_lock<std::mutex> lock(mu_);
        if (inbox_.empty()) {
            // Behave like a real socket: block for the timeout, so the bridge's
            // poll loop doesn't spin the CPU during a test.
            cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return !inbox_.empty(); });
        }
        if (inbox_.empty()) return 0;
        auto pkt = inbox_.front();
        inbox_.pop_front();
        const size_t n = std::min(len, pkt.first.size());
        std::memcpy(buf, pkt.first.data(), n);
        if (src_ip) *src_ip = pkt.second;
        return static_cast<int>(n);
    }

    bool send(const uint8_t* buf, size_t len, uint32_t ip, uint16_t port) override {
        std::lock_guard<std::mutex> lock(mu_);
        sent_.push_back({std::vector<uint8_t>(buf, buf + len), ip, port});
        return true;
    }

    void deliver(const uint8_t* buf, size_t len, uint32_t src_ip = 0x0A000001) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            inbox_.emplace_back(std::vector<uint8_t>(buf, buf + len), src_ip);
        }
        cv_.notify_one();
    }

    std::vector<SentPacket> sent() {
        std::lock_guard<std::mutex> lock(mu_);
        return sent_;
    }
    size_t count_sent_of_type(uint8_t type) {
        std::lock_guard<std::mutex> lock(mu_);
        size_t n = 0;
        for (auto& p : sent_) if (p.type() == type) ++n;
        return n;
    }
    void clear_sent() {
        std::lock_guard<std::mutex> lock(mu_);
        sent_.clear();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<std::vector<uint8_t>, uint32_t>> inbox_;
    std::vector<SentPacket> sent_;
};

class FakeClock : public prolink::IClockSink {
public:
    void update_tempo_bpm(float bpm) override { bpm_.store(bpm); ++tempo_updates_; }
    void correct_phase(uint8_t beat) override { (void)beat; ++phase_corrections_; }
    void feed_beat(uint32_t ms, uint8_t beat) override {
        (void)ms; (void)beat; ++beats_fed_;
    }
    void start() override { running_.store(true); ++starts_; }
    void stop() override  { running_.store(false); ++stops_; }
    void set_offset_us(int32_t us) override { offset_us_.store(us); }
    int32_t get_offset_us() const override { return offset_us_.load(); }

    std::atomic<float>   bpm_{0.0f};
    std::atomic<bool>    running_{false};
    std::atomic<int>     starts_{0}, stops_{0}, tempo_updates_{0};
    std::atomic<int>     beats_fed_{0}, phase_corrections_{0};
    std::atomic<int32_t> offset_us_{0};
};

// A bridge running on its own thread, with fakes wired in and sensible test
// defaults (short timeouts; announce off so the ~4 s claim handshake doesn't
// slow every test — the claim has its own test).
struct Rig {
    FakeSocket beat, status, keepalive;
    FakeClock  clock;
    std::unique_ptr<prolink::Bridge> bridge;
    std::thread thread;

    explicit Rig(std::function<void(prolink::BridgeConfig&)> tweak = nullptr) {
        prolink::BridgeConfig cfg;
        cfg.device_num          = 7;
        cfg.master_device_num   = 4;
        cfg.send_vcdj_announce  = false;
        cfg.silence_timeout_ms  = 300;
        cfg.play_debounce_ms    = 20;
        cfg.bpm_smoothing_alpha = 1.0f;   // no EMA lag in assertions
        cfg.broadcast_ip        = 0xFFFFFFFFu;
        std::strncpy(cfg.device_name, "test-box", sizeof(cfg.device_name) - 1);
        if (tweak) tweak(cfg);
        bridge = std::make_unique<prolink::Bridge>(beat, status, keepalive,
                                                   clock, cfg);
        thread = std::thread([this] { bridge->run(); });
        // run() sets running_ on entry, so a stop() that beats the thread there
        // would be swallowed and the loop would never exit. Wait for it.
        while (!bridge->running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ~Rig() {
        bridge->stop();
        // Nudge the poll loop so it notices the stop promptly.
        uint8_t z = 0;
        beat.deliver(&z, 1);
        if (thread.joinable()) thread.join();
    }
};

// ── Packet helpers ─────────────────────────────────────────────────────────

std::vector<uint8_t> beat_packet(uint8_t dev, float bpm, uint8_t beat_in_bar) {
    std::vector<uint8_t> b(0x60);
    prolink::build_beat_packet(b.data(), b.size(), "DECK", dev, bpm, beat_in_bar);
    return b;
}

std::vector<uint8_t> status_packet(uint8_t dev, float bpm, uint8_t beat_in_bar,
                                   bool is_master, uint32_t syncn = 5,
                                   uint8_t mh = 0xFF, bool playing = true) {
    std::vector<uint8_t> b(284);
    prolink::build_status_packet(b.data(), b.size(), "DECK", dev, bpm,
                                 beat_in_bar, is_master, syncn, mh);
    if (!playing) b[0x89] &= static_cast<uint8_t>(~0x40);  // clear the play bit
    return b;
}

// Enter master mode and wait until the run loop has actually applied it — the
// takeover request going out is the observable signal. Returns false if it
// never did, so callers can report it as a normal failure.
bool enter_master_mode(Rig& r) {
    r.bridge->set_master_mode(true);
    return wait_for([&] {
        return r.beat.count_sent_of_type(prolink::PKT_TYPE_MASTER_HANDOFF_REQ) > 0;
    });
}

// Enter master mode and drive the deck's yield through to confirmation.
bool become_master(Rig& r, uint8_t deck, float bpm, uint32_t deck_ip,
                   uint32_t syncn = 9) {
    if (!enter_master_mode(r)) return false;
    auto yield = status_packet(deck, bpm, 1, /*is_master*/ true, syncn,
                               /*mh*/ 4);   // deck yields to our master slot
    r.status.deliver(yield.data(), yield.size(), deck_ip);
    return wait_for([&] { return r.bridge->is_tempo_master(); });
}

// Release master and wait until the run loop has applied it (the appointment
// command going out is the signal).
bool release_master(Rig& r) {
    r.bridge->set_master_mode(false);
    return wait_for([&] {
        return r.beat.count_sent_of_type(prolink::PKT_TYPE_SYNC_CONTROL) > 0;
    });
}

// ── Master tracking and tempo ──────────────────────────────────────────────

void test_tracks_master_and_takes_its_tempo() {
    section("follows the deck that holds the master flag");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, /*is_master*/ true);
    r.status.deliver(s.data(), s.size());
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));
    CHECK(wait_for([&] { return std::abs(r.clock.bpm_.load() - 128.0f) < 0.01f; }));
}

void test_master_flag_moves_between_decks() {
    section("master handoff between two decks is followed");
    Rig r;
    auto a = status_packet(1, 120.0f, 1, true);
    r.status.deliver(a.data(), a.size());
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 1; }));

    auto b = status_packet(2, 150.0f, 1, true);
    r.status.deliver(b.data(), b.size());
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 2; }));
    CHECK(wait_for([&] { return std::abs(r.clock.bpm_.load() - 150.0f) < 0.01f; }));
}

void test_ignores_non_master_decks() {
    section("a non-master deck's tempo is ignored");
    Rig r;
    auto m = status_packet(1, 120.0f, 1, true);
    r.status.deliver(m.data(), m.size());
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 1; }));

    auto other = status_packet(2, 175.0f, 1, /*is_master*/ false);
    r.status.deliver(other.data(), other.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(std::abs(r.clock.bpm_.load() - 120.0f) < 0.01f);   // still deck 1's
}

void test_invalid_bpm_is_not_applied() {
    section("tempo is only taken when Mv marks it valid");
    Rig r;
    auto s = status_packet(1, 130.0f, 1, true);
    s[0x90] = 0x7F; s[0x91] = 0xFF;    // Mv = 0x7FFF (no rekordbox track)
    r.status.deliver(s.data(), s.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK_EQ(r.clock.tempo_updates_.load(), 0);
}

// ── Transport ──────────────────────────────────────────────────────────────

void test_start_is_held_until_a_downbeat() {
    section("MIDI Start waits for beat 1 of the bar");
    Rig r;
    for (uint8_t beat : {2, 3, 4}) {
        auto b = beat_packet(1, 120.0f, beat);
        r.beat.deliver(b.data(), b.size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK_EQ(r.clock.starts_.load(), 0);        // no downbeat yet

    auto down = beat_packet(1, 120.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));
    CHECK(r.bridge->is_playing());
}

void test_stops_when_the_master_pauses() {
    section("Stop when the master's play flag drops");
    Rig r;
    auto down = beat_packet(1, 120.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));

    // Two paused status packets: one to arm the debounce, one after it expires.
    for (int i = 0; i < 2; ++i) {
        auto s = status_packet(1, 120.0f, 1, true, 5, 0xFF, /*playing*/ false);
        r.status.deliver(s.data(), s.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    CHECK(wait_for([&] { return r.clock.stops_.load() >= 1; }));
    CHECK(!r.bridge->is_playing());
}

void test_stops_on_silence() {
    section("Stop when the link goes quiet");
    Rig r;   // silence_timeout_ms = 300
    auto down = beat_packet(1, 120.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));
    CHECK(wait_for([&] { return r.clock.stops_.load() >= 1; }, 1500));
}

// ── Bar slip and re-sync ───────────────────────────────────────────────────

void test_a_link_blip_does_not_stop_the_clock() {
    section("a brief link drop rides through instead of stopping");
    Rig r([](prolink::BridgeConfig& c) { c.link_down_grace_ms = 2000; });
    auto down = beat_packet(1, 120.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));

    // The link drops. Packets stop with it, so silence detection would fire at
    // silence_timeout_ms (300) — but the box knows this is a network fault,
    // not the DJ stopping, and keeps clocking on the latched tempo.
    r.bridge->set_link_up(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    CHECK(r.clock.stops_.load() == 0);
    CHECK(r.clock.running_.load());

    // Link back before the grace expires: no Stop, no Start, no waiting for a
    // downbeat — the clock simply carries on and re-locks.
    r.bridge->set_link_up(true);
    r.beat.deliver(down.data(), down.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(r.clock.stops_.load() == 0);
    CHECK(r.clock.starts_.load() == 1);
}

void test_a_long_link_outage_still_stops() {
    section("but a link that stays down does stop the clock");
    Rig r([](prolink::BridgeConfig& c) { c.link_down_grace_ms = 400; });
    auto down = beat_packet(1, 120.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));

    r.bridge->set_link_up(false);
    CHECK(wait_for([&] { return r.clock.stops_.load() >= 1; }, 2000));
}

void test_keep_playing_holds_when_the_deck_stops() {
    section("keep playing: a stopped deck holds the clock instead of stopping it");
    Rig r;
    auto play = status_packet(1, 128.0f, 1, true);
    r.status.deliver(play.data(), play.size());
    auto down = beat_packet(1, 128.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));
    r.bridge->set_keep_playing(true);

    // The deck stops. Without the setting this sends MIDI Stop; with it the
    // clock carries on at the latched tempo so downstream gear keeps going.
    auto paused = status_packet(1, 128.0f, 1, true, 5, 0xFF, /*playing*/ false);
    r.status.deliver(paused.data(), paused.size());
    CHECK(wait_for([&] { return r.bridge->holding(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(r.clock.stops_.load() == 0);
    CHECK(r.clock.running_.load());

    // A deck driving us again ends the hold, with no Stop/Start in between.
    // Real decks stream status at ~5 Hz and the play flag is debounced, so a
    // single packet cannot satisfy it — deliver a stream like the hardware.
    for (int i = 0; i < 6; ++i) {
        r.status.deliver(play.data(), play.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(wait_for([&] { return !r.bridge->holding(); }));
    CHECK(r.clock.stops_.load() == 0);
    CHECK(r.clock.starts_.load() == 1);
}

void test_keep_playing_holds_through_silence() {
    section("keep playing: a link gone quiet holds the clock too");
    Rig r;   // silence_timeout_ms = 300
    auto down = beat_packet(1, 120.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));
    r.bridge->set_keep_playing(true);

    // A deck powered off mid-set, not just paused.
    CHECK(wait_for([&] { return r.bridge->holding(); }, 1500));
    CHECK(r.clock.stops_.load() == 0);
    CHECK(r.clock.running_.load());
}

void test_keep_playing_off_still_stops() {
    section("keep playing off leaves the stop behaviour alone");
    Rig r;
    auto play = status_packet(1, 128.0f, 1, true);
    r.status.deliver(play.data(), play.size());
    auto down = beat_packet(1, 128.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));

    auto paused = status_packet(1, 128.0f, 1, true, 5, 0xFF, /*playing*/ false);
    r.status.deliver(paused.data(), paused.size());
    CHECK(wait_for([&] { return r.clock.stops_.load() >= 1; }));
    CHECK(!r.bridge->holding());
}

void test_taking_the_master_role_clears_the_hold() {
    section("holding ends when the box stops following decks");
    Rig r;
    auto play = status_packet(1, 128.0f, 1, true);
    r.status.deliver(play.data(), play.size());
    auto down = beat_packet(1, 128.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));
    r.bridge->set_keep_playing(true);
    auto paused = status_packet(1, 128.0f, 1, true, 5, 0xFF, /*playing*/ false);
    r.status.deliver(paused.data(), paused.size());
    CHECK(wait_for([&] { return r.bridge->holding(); }));

    // Going standalone (and, by the same path, taking the master role) is a
    // different state from following stopped decks — the panel must not keep
    // showing HOLD once the box owns the tempo outright.
    r.bridge->set_ignore_master(true);
    CHECK(!r.bridge->holding());
}

void test_single_dropped_beat_does_not_realign() {
    section("one dropped beat packet must not trigger a Stop+Start");
    Rig r;
    // Establish playback on a clean bar.
    for (uint8_t beat : {1, 2, 3, 4}) {
        auto b = beat_packet(1, 120.0f, beat);
        r.beat.deliver(b.data(), b.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));
    const int starts_before = r.clock.starts_.load();

    // A single skipped beat (1 -> 3) is a dropped packet, not a real slip.
    auto skip = beat_packet(1, 120.0f, 3);
    r.beat.deliver(skip.data(), skip.size());
    auto next = beat_packet(1, 120.0f, 4);
    r.beat.deliver(next.data(), next.size());
    auto down = beat_packet(1, 120.0f, 1);
    r.beat.deliver(down.data(), down.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(r.clock.starts_.load(), starts_before);
}

void test_sustained_bar_slip_realigns() {
    section("a sustained bar slip does realign on the next downbeat");
    Rig r;
    auto d0 = beat_packet(1, 120.0f, 1);
    r.beat.deliver(d0.data(), d0.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));

    // Repeatedly report the same beat number: the expectation never matches,
    // so the confidence counter climbs past the threshold.
    for (int i = 0; i < 5; ++i) {
        auto b = beat_packet(1, 120.0f, 3);
        r.beat.deliver(b.data(), b.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto down = beat_packet(1, 120.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() >= 2; }));
}

void test_manual_resync_realigns_on_downbeat() {
    section("a re-sync request realigns on the master's next downbeat");
    Rig r;
    auto d0 = beat_packet(1, 120.0f, 1);
    r.beat.deliver(d0.data(), d0.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));

    r.bridge->request_resync(/*immediate*/ false);
    CHECK(r.bridge->resync_pending());

    // Non-downbeats must not trigger it.
    auto b2 = beat_packet(1, 120.0f, 2);
    r.beat.deliver(b2.data(), b2.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(r.clock.starts_.load(), 1);

    auto down = beat_packet(1, 120.0f, 1);
    r.beat.deliver(down.data(), down.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 2; }));
    CHECK(wait_for([&] { return !r.bridge->resync_pending(); }));
}

void test_immediate_resync() {
    section("an immediate re-sync does not wait for a downbeat");
    Rig r;
    auto d0 = beat_packet(1, 120.0f, 1);
    r.beat.deliver(d0.data(), d0.size());
    CHECK(wait_for([&] { return r.clock.starts_.load() == 1; }));

    r.bridge->request_resync(/*immediate*/ true);
    CHECK(wait_for([&] { return r.clock.starts_.load() == 2; }));
}

// ── Offsets ────────────────────────────────────────────────────────────────

void test_offsets_sum_into_the_clock() {
    section("clock + grid offsets are summed and pushed as microseconds");
    Rig r;
    r.bridge->set_clock_offset_ms(30.0f);
    CHECK_EQ(r.clock.offset_us_.load(), 30000);
    r.bridge->set_grid_offset_ms(-5.0f);
    CHECK_EQ(r.clock.offset_us_.load(), 25000);
    r.bridge->adjust_clock_offset_ms(1.5f);
    CHECK(std::abs(r.bridge->clock_offset_ms() - 31.5f) < 0.001f);
    CHECK_EQ(r.clock.offset_us_.load(), 26500);
    r.bridge->reset_grid_offset();
    CHECK_EQ(r.clock.offset_us_.load(), 31500);
    CHECK(std::abs(r.bridge->total_offset_ms() - 31.5f) < 0.001f);
}

// ── Standalone / tempo authority ───────────────────────────────────────────

void test_standalone_ignores_decks() {
    section("standalone: the box owns the tempo and ignores deck packets");
    Rig r;
    r.bridge->set_manual_bpm(100.0f);
    r.bridge->set_ignore_master(true);
    CHECK(wait_for([&] { return r.clock.running_.load(); }));   // cold-starts

    const int fed_before = r.clock.beats_fed_.load();
    auto b = beat_packet(1, 174.0f, 1);
    r.beat.deliver(b.data(), b.size());
    auto s = status_packet(1, 174.0f, 1, true);
    r.status.deliver(s.data(), s.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Neither phase nor tempo may come from a deck while we are the authority:
    // that feedback loop made a synced deck lurch around the beat grid.
    CHECK_EQ(r.clock.beats_fed_.load(), fed_before);
    CHECK(std::abs(r.clock.bpm_.load() - 100.0f) < 0.01f);
}

void test_standalone_survives_silence() {
    section("standalone keeps clocking when the link goes quiet");
    Rig r;
    r.bridge->set_manual_bpm(120.0f);
    r.bridge->set_ignore_master(true);
    CHECK(wait_for([&] { return r.clock.running_.load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // > timeout
    CHECK(r.clock.running_.load());
    CHECK(r.bridge->is_playing());
}

// ── Tempo master: taking the role ──────────────────────────────────────────

void test_master_mode_requests_handoff_then_asserts() {
    section("tempo master: request 0x26, wait for the yield, then assert");
    Rig r;
    // Learn the current master and its IP.
    auto s = status_packet(3, 128.0f, 1, /*is_master*/ true, /*syncn*/ 9);
    r.status.deliver(s.data(), s.size(), /*src_ip*/ 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));

    r.beat.clear_sent();
    r.bridge->set_master_mode(true);
    CHECK(r.bridge->master_mode());

    // A takeover request must go to the master, unicast, on the beat port.
    CHECK(wait_for([&] {
        return r.beat.count_sent_of_type(prolink::PKT_TYPE_MASTER_HANDOFF_REQ) > 0;
    }));
    bool addressed_correctly = false;
    for (auto& p : r.beat.sent()) {
        if (p.type() == prolink::PKT_TYPE_MASTER_HANDOFF_REQ) {
            addressed_correctly = (p.ip == 0xC0A80105 && p.port == prolink::PORT_BEAT);
            break;
        }
    }
    CHECK(addressed_correctly);

    // Until the master yields we must NOT claim the role.
    CHECK(!r.bridge->is_tempo_master());

    // The master yields by naming us in Mh (we claim device 4 while master).
    auto yield = status_packet(3, 128.0f, 1, true, /*syncn*/ 9, /*mh*/ 4);
    r.status.deliver(yield.data(), yield.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->is_tempo_master(); }));
}

void test_master_broadcasts_beats_and_status() {
    section("as master, the box broadcasts its own grid and status");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));

    CHECK(become_master(r, 3, 128.0f, 0xC0A80105));

    // Status packets: broadcast, master flag set, our device number, Syncn
    // above the peer's.
    const bool good = wait_for([&] {
        for (auto& p : r.status.sent()) {
            if (p.type() != prolink::PKT_TYPE_STATUS) continue;
            auto parsed = prolink::parse_status_packet(p.bytes.data(), p.bytes.size());
            if (parsed && parsed->is_master() && parsed->device_num == 4 &&
                parsed->syncn > 9 && p.port == prolink::PORT_STATUS) {
                return true;
            }
        }
        return false;
    });
    CHECK(good);

    // Beat packets are emitted from the clock's per-beat callback.
    r.beat.clear_sent();
    for (int i = 0; i < 4; ++i) r.bridge->on_master_beat();
    CHECK_EQ(r.beat.count_sent_of_type(prolink::PKT_TYPE_BEAT), 4);
    auto bp = prolink::parse_beat_packet(r.beat.sent()[0].bytes.data(),
                                         r.beat.sent()[0].bytes.size());
    CHECK(bp.has_value());
    if (bp) CHECK_EQ(bp->device_num, 4);
}

void test_master_takeover_keeps_bar_alignment() {
    section("taking master continues the deck's bar rather than restarting it");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));

    // Deck is on beat 2 of the bar when we take over.
    auto b2 = beat_packet(3, 128.0f, 2);
    r.beat.deliver(b2.data(), b2.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(enter_master_mode(r));
    r.beat.clear_sent();
    r.bridge->on_master_beat();          // our first broadcast beat

    auto sent = r.beat.sent();
    CHECK(!sent.empty());
    if (!sent.empty()) {
        auto bp = prolink::parse_beat_packet(sent[0].bytes.data(), sent[0].bytes.size());
        CHECK(bp.has_value());
        // Continues the bar: after the deck's beat 2 comes beat 3, not beat 1.
        if (bp) CHECK_EQ(bp->beat_in_bar, 3);
    }
}

void test_master_takes_over_at_the_following_tempo() {
    section("taking master does not lurch the tempo");
    Rig r;
    auto s = status_packet(3, 137.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return std::abs(r.clock.bpm_.load() - 137.0f) < 0.01f; }));

    CHECK(enter_master_mode(r));
    // We keep clocking at the tempo we were following, not a default.
    CHECK(std::abs(r.clock.bpm_.load() - 137.0f) < 0.01f);
}

void test_master_ignores_deck_tempo() {
    section("as master, a deck's tempo cannot hijack ours");
    Rig r;
    auto s = status_packet(3, 120.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));

    CHECK(become_master(r, 3, 120.0f, 0xC0A80105));

    r.bridge->set_manual_bpm(100.0f);
    // A deck reporting a wildly different tempo must not move us. (This is the
    // status-path half of the feedback-loop bug.)
    auto fast = status_packet(3, 175.0f, 1, false, 9);
    r.status.deliver(fast.data(), fast.size(), 0xC0A80105);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(std::abs(r.clock.bpm_.load() - 100.0f) < 0.01f);
}

// ── Tempo master: giving the role back ─────────────────────────────────────

void test_master_yields_when_a_deck_asks() {
    section("a deck can always reclaim master from the box");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));
    CHECK(become_master(r, 3, 128.0f, 0xC0A80105));

    // Deck 3 asks for it back with a 0x26 on the beat port.
    r.status.clear_sent();
    uint8_t req[64];
    const size_t rn = prolink::build_master_handoff_request(req, sizeof req, "DECK", 3);
    r.beat.deliver(req, rn, 0xC0A80105);

    // We must acknowledge with 0x27, unicast back on the STATUS port.
    CHECK(wait_for([&] {
        return r.status.count_sent_of_type(prolink::PKT_TYPE_MASTER_HANDOFF_RESP) > 0;
    }));
    bool ack_ok = false;
    for (auto& p : r.status.sent()) {
        if (p.type() == prolink::PKT_TYPE_MASTER_HANDOFF_RESP) {
            ack_ok = (p.ip == 0xC0A80105 && p.port == prolink::PORT_STATUS);
            break;
        }
    }
    CHECK(ack_ok);

    // And advertise the requester in our own Mh while the handoff is pending.
    CHECK(wait_for([&] {
        for (auto& p : r.status.sent()) {
            if (p.type() != prolink::PKT_TYPE_STATUS) continue;
            auto parsed = prolink::parse_status_packet(p.bytes.data(), p.bytes.size());
            if (parsed && parsed->master_handoff == 3) return true;
        }
        return false;
    }));

    // Once the deck asserts master, we step down.
    auto reclaimed = status_packet(3, 128.0f, 1, /*is_master*/ true, 12);
    r.status.deliver(reclaimed.data(), reclaimed.size(), 0xC0A80105);
    CHECK(wait_for([&] { return !r.bridge->is_tempo_master(); }));
    CHECK(wait_for([&] { return !r.bridge->master_mode(); }));
}

void test_release_appoints_a_deck() {
    section("releasing master appoints a deck so the link keeps one");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));
    CHECK(become_master(r, 3, 128.0f, 0xC0A80105));

    r.beat.clear_sent();
    // A SYNC_CONTROL "become master" must go to the deck we are handing to.
    CHECK(release_master(r));

    // And once that deck claims the role, we are fully out of master mode.
    auto reclaimed = status_packet(3, 128.0f, 1, true, 12);
    r.status.deliver(reclaimed.data(), reclaimed.size(), 0xC0A80105);
    CHECK(wait_for([&] { return !r.bridge->master_mode(); }));
}

void test_release_restores_standalone() {
    section("releasing master restores standalone rather than forcing follow");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));

    r.bridge->set_ignore_master(true);       // standalone first
    CHECK(become_master(r, 3, 128.0f, 0xC0A80105));   // then take master

    CHECK(release_master(r));
    auto reclaimed = status_packet(3, 128.0f, 1, true, 12);
    r.status.deliver(reclaimed.data(), reclaimed.size(), 0xC0A80105);
    CHECK(wait_for([&] { return !r.bridge->master_mode(); }));
    // We were standalone before taking master, so we must still be standalone.
    CHECK(r.bridge->ignore_master());
}

void test_reclaiming_master_clears_a_pending_release() {
    section("re-selecting master cancels a release still in flight");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));
    CHECK(become_master(r, 3, 128.0f, 0xC0A80105));

    r.bridge->set_master_mode(false);   // release starts, Mh advertises deck 3
    r.bridge->set_master_mode(true);    // ...and we immediately want it back
    r.status.clear_sent();

    // The bug this guards: the stale release left us advertising Mh, so we
    // claimed the role and instantly gave it away again.
    CHECK(wait_for([&] {
        for (auto& p : r.status.sent()) {
            if (p.type() != prolink::PKT_TYPE_STATUS) continue;
            auto parsed = prolink::parse_status_packet(p.bytes.data(), p.bytes.size());
            if (parsed && parsed->master_handoff != 0xFF) return false;
        }
        return r.status.count_sent_of_type(prolink::PKT_TYPE_STATUS) > 0;
    }));
    CHECK(r.bridge->master_mode());
}

// ── Device-number claim ────────────────────────────────────────────────────

void test_device_claim_runs_before_keepalives() {
    section("the device-number claim handshake precedes keep-alives");
    Rig r([](prolink::BridgeConfig& cfg) {
        cfg.send_vcdj_announce  = true;    // enable the claim for this test
        cfg.keepalive_period_ms = 50;
    });

    // All four stages, in order, then keep-alives.
    CHECK(wait_for([&] {
        return r.keepalive.count_sent_of_type(prolink::PKT_TYPE_KEEPALIVE) > 0;
    }, 8000));

    const auto sent = r.keepalive.sent();
    std::vector<uint8_t> order;
    for (auto& p : sent) {
        if (order.empty() || order.back() != p.type()) order.push_back(p.type());
    }
    // hello (0x0a) -> stage1 (0x00) -> stage2 (0x02) -> stage3 (0x04) -> keepalive (0x06)
    const std::vector<uint8_t> want = {0x0A, 0x00, 0x02, 0x04, 0x06};
    CHECK_EQ(order.size(), want.size());
    for (size_t i = 0; i < want.size() && i < order.size(); ++i)
        CHECK_EQ(order[i], want[i]);

    // The claim must name the device number we intend to use.
    for (auto& p : sent) {
        if (p.type() == 0x02) { CHECK_EQ(p.bytes[0x2E], 7); break; }
    }
}


// ── Regressions from the code audit ────────────────────────────────────────

void test_own_beat_packets_are_ignored() {
    section("we never track ourselves as master from our own echoed beats");
    Rig r;
    // Broadcasts loop back, so as master our own beats arrive on our own
    // socket. Latching ourselves as current_master_ used to inflate the
    // packet counters and leave no deck to appoint on release.
    auto mine = beat_packet(/*dev*/ 7, 120.0f, 1);   // 7 == our device number
    r.beat.deliver(mine.data(), mine.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK_EQ(r.bridge->current_master_num(), 0);
    CHECK_EQ(r.bridge->beat_packet_count(), 0);
    CHECK_EQ(r.clock.starts_.load(), 0);
}

void test_garbage_tempo_never_reaches_the_clock() {
    section("an absurd tempo from a deck cannot poison our own grid");
    Rig r;
    // 128 BPM track with a 2.0x pitch reads as 256; push it far higher by
    // claiming a huge pitch. Nothing outside the sane range may be applied.
    auto s = status_packet(3, 600.0f, 1, true);
    // Pitch1 = 8.0x -> 4800 BPM effective.
    s[0x8C] = 0x00; s[0x8D] = 0x80; s[0x8E] = 0x00; s[0x8F] = 0x00;
    r.status.deliver(s.data(), s.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const float bpm = r.clock.bpm_.load();
    CHECK(bpm == 0.0f || (bpm >= 1.0f && bpm <= 655.0f));
}

void test_absurd_syncn_cannot_disable_takeover() {
    section("a bogus Syncn cannot permanently block master takeover");
    Rig r;
    // One packet claiming Syncn = 0xFFFFFFFF used to raise the ceiling
    // forever, so max+1 wrapped to 0 and no deck would ever yield to us.
    auto poison = status_packet(3, 128.0f, 1, true, 0xFFFFFFFFu);
    r.status.deliver(poison.data(), poison.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));
    auto normal = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(normal.data(), normal.size(), 0xC0A80105);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(become_master(r, 3, 128.0f, 0xC0A80105));
    // Our broadcast Syncn must be a plausible, non-zero generation.
    CHECK(wait_for([&] {
        for (auto& p : r.status.sent()) {
            if (p.type() != prolink::PKT_TYPE_STATUS) continue;
            auto parsed = prolink::parse_status_packet(p.bytes.data(), p.bytes.size());
            if (parsed && parsed->is_master() && parsed->syncn > 0 &&
                parsed->syncn < 0x10000000u) return true;
        }
        return false;
    }));
}

void test_handoff_ack_requires_magic_and_the_right_sender() {
    section("a stray 0x27 cannot make us assert master");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));
    CHECK(enter_master_mode(r));

    // Right type byte, no magic, wrong source: must be ignored on both counts.
    uint8_t junk[64] = {0};
    junk[0x0A] = prolink::PKT_TYPE_MASTER_HANDOFF_RESP;
    r.status.deliver(junk, sizeof junk, 0xC0A80105);      // no magic
    uint8_t ack[64];
    const size_t an = prolink::build_master_handoff_response(ack, sizeof ack, "X", 3);
    r.status.deliver(ack, an, 0x0A0B0C0D);                // not the master's IP
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(!r.bridge->is_tempo_master());

    // The real thing, from the master, is accepted.
    r.status.deliver(ack, an, 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->is_tempo_master(); }));
}

void test_release_without_handoff_restores_the_source() {
    section("releasing master we never held still restores the source");
    Rig r;
    // No master is ever learned, so the handoff cannot complete — the path
    // that used to leave the box silently standalone while the panel said
    // otherwise.
    r.bridge->set_master_mode(true);
    CHECK(wait_for([&] { return r.bridge->ignore_master(); }));
    r.bridge->set_master_mode(false);
    CHECK(wait_for([&] { return !r.bridge->master_mode(); }));
    // Back to following decks, not stuck ignoring them.
    CHECK(wait_for([&] { return !r.bridge->ignore_master(); }));

    auto s = status_packet(3, 132.0f, 1, true);
    r.status.deliver(s.data(), s.size());
    CHECK(wait_for([&] { return std::abs(r.clock.bpm_.load() - 132.0f) < 0.01f; }));
}

void test_source_chosen_during_release_wins() {
    section("a source picked during a pending release is not overwritten");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));
    CHECK(become_master(r, 3, 128.0f, 0xC0A80105));

    // Release, then immediately choose standalone. When the deck finally
    // claims master, the stale pre-master value must not revert that choice.
    CHECK(release_master(r));
    r.bridge->set_ignore_master(true);
    auto reclaimed = status_packet(3, 128.0f, 1, true, 12);
    r.status.deliver(reclaimed.data(), reclaimed.size(), 0xC0A80105);
    CHECK(wait_for([&] { return !r.bridge->master_mode(); }));
    CHECK(r.bridge->ignore_master());
}

void test_auto_source_keeps_tracking_the_master() {
    section("switching back to auto does not forget who holds master");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));

    // The panel falls back to `follower master` when a deck reclaims the role.
    // That must not reset tracking to "unknown": a re-bootstrap latches onto
    // whichever deck's status lands first — often an idle one with no track
    // loaded, so no valid tempo — and only corrects when the real master's
    // flag next comes round, seconds later on a real link.
    r.bridge->set_force_master_device(0);
    CHECK(r.bridge->current_master_num() == 3);

    // Pinning still takes effect at once, and releasing the pin holds that
    // deck until the master flag moves on its own.
    r.bridge->set_force_master_device(1);
    CHECK(r.bridge->current_master_num() == 1);
    r.bridge->set_force_master_device(0);
    CHECK(r.bridge->current_master_num() == 1);

    auto moved = status_packet(3, 128.0f, 1, true, 10);
    r.status.deliver(moved.data(), moved.size(), 0xC0A80105);
    CHECK(wait_for([&] { return r.bridge->current_master_num() == 3; }));
}

void test_tempo_snaps_back_after_a_release() {
    section("the first deck tempo after a release lands whole, not smoothed");
    Rig r;
    auto s = status_packet(3, 128.0f, 1, true, 9);
    r.status.deliver(s.data(), s.size(), 0xC0A80105);
    CHECK(wait_for([&] { return std::abs(r.clock.bpm_.load() - 128.0f) < 0.01f; }));
    CHECK(become_master(r, 3, 128.0f, 0xC0A80105));

    // Deck 3 asks for the role back and asserts it.
    uint8_t req[64];
    const size_t rn = prolink::build_master_handoff_request(req, sizeof req, "DECK", 3);
    r.beat.deliver(req, rn, 0xC0A80105);
    auto reclaimed = status_packet(3, 128.0f, 1, /*is_master*/ true, 12);
    r.status.deliver(reclaimed.data(), reclaimed.size(), 0xC0A80105);
    CHECK(wait_for([&] { return !r.bridge->master_mode(); }));

    // The tempo smoother still held the pre-master value, so a deck now at a
    // very different tempo used to be approached over several packets — the
    // box audibly lagging the deck for seconds. One packet must be enough.
    auto fast = status_packet(3, 140.0f, 1, true, 12);
    r.status.deliver(fast.data(), fast.size(), 0xC0A80105);
    CHECK(wait_for([&] { return std::abs(r.clock.bpm_.load() - 140.0f) < 0.01f; }));
}
}  // namespace

int bridge_tests_main() {
    test_tracks_master_and_takes_its_tempo();
    test_master_flag_moves_between_decks();
    test_ignores_non_master_decks();
    test_invalid_bpm_is_not_applied();

    test_start_is_held_until_a_downbeat();
    test_stops_when_the_master_pauses();
    test_stops_on_silence();
    test_a_link_blip_does_not_stop_the_clock();
    test_a_long_link_outage_still_stops();
    test_keep_playing_holds_when_the_deck_stops();
    test_keep_playing_holds_through_silence();
    test_keep_playing_off_still_stops();
    test_taking_the_master_role_clears_the_hold();

    test_single_dropped_beat_does_not_realign();
    test_sustained_bar_slip_realigns();
    test_manual_resync_realigns_on_downbeat();
    test_immediate_resync();

    test_offsets_sum_into_the_clock();

    test_standalone_ignores_decks();
    test_standalone_survives_silence();

    test_master_mode_requests_handoff_then_asserts();
    test_master_broadcasts_beats_and_status();
    test_master_takeover_keeps_bar_alignment();
    test_master_takes_over_at_the_following_tempo();
    test_master_ignores_deck_tempo();

    test_master_yields_when_a_deck_asks();
    test_release_appoints_a_deck();
    test_release_restores_standalone();
    test_reclaiming_master_clears_a_pending_release();

    test_device_claim_runs_before_keepalives();

    test_own_beat_packets_are_ignored();
    test_garbage_tempo_never_reaches_the_clock();
    test_absurd_syncn_cannot_disable_takeover();
    test_handoff_ack_requires_magic_and_the_right_sender();
    test_release_without_handoff_restores_the_source();
    test_source_chosen_during_release_wins();
    test_auto_source_keeps_tracking_the_master();
    test_tempo_snaps_back_after_a_release();

    std::printf("\nbridge: %d checks, %d failure%s\n", g_checks, g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures;
}
