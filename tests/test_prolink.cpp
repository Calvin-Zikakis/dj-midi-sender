// Unit tests for lib/prolink — dependency-free (no gtest), so they build and
// run anywhere the core does.
//
// The emphasis is deliberate: almost everything here pins down *wire bytes we
// reverse-engineered from hardware*. Those offsets have no compiler to protect
// them — shift one and the box silently stops taking tempo master, or a CDJ
// quietly ignores our beat grid. The reference data comes from:
//   - captures/xdj-xz-export-mode.pcapng (a real XDJ-XZ beat packet)
//   - a live XDJ-700 master status packet
//   - Deep Symmetry's beat-link payload templates (handoff / sync / claim)
//
// Build + run:  cmake --build build --target xdj_tests && ./build/tests/xdj_tests

#include "clock.hpp"
#include "packets.hpp"
#include "types.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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

void check_eq_u32(uint32_t got, uint32_t want, const char* what,
                  const char* file, int line) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s — got %u (0x%x), want %u (0x%x)\n",
                    file, line, what, got, got, want, want);
    }
}
#define CHECK_EQ(got, want) check_eq_u32((uint32_t)(got), (uint32_t)(want), #got, __FILE__, __LINE__)

std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

void section(const char* name) { std::printf("%s\n", name); std::fflush(stdout); }

// A real XDJ-XZ beat packet (beat 1 of the bar, 132.00 BPM, unity pitch),
// captured in captures/xdj-xz-export-mode.pcapng.
const char* kRealBeatPacketHex =
    "5173707431576d4a4f4c2858444a2d585a0000000000000000000000000000010001003c"
    "000001c60000038d0000071a0000071a00000e3400000e34"
    "ffffffffffffffffffffffffffffffffffffffffffffffff"
    "001000000000339001000001";

// ── Beat packets ───────────────────────────────────────────────────────────

void test_beat_packet_matches_real_hardware() {
    section("beat packet reproduces a real XDJ-XZ packet");
    const auto real = from_hex(kRealBeatPacketHex);
    CHECK_EQ(real.size(), 0x60);

    uint8_t built[128];
    const size_t n = prolink::build_beat_packet(built, sizeof built, "XDJ-XZ",
                                                /*device*/ 1, 132.0f, /*beat*/ 1);
    CHECK_EQ(n, 0x60);

    // Byte-for-byte, except the "ms to next beat" field at 0x24, where the XZ's
    // own value jitters by ~1 ms between packets (454 vs 455 at 132 BPM).
    for (size_t i = 0; i < n; ++i) {
        if (i >= 0x24 && i < 0x28) continue;
        if (built[i] != real[i]) {
            std::printf("  FAIL byte 0x%02zx: built %02x, real %02x\n",
                        i, built[i], real[i]);
            ++g_failures;
        }
        ++g_checks;
    }
    // And that field must still be within a millisecond of the captured value.
    const uint32_t built_next = (built[0x24] << 24) | (built[0x25] << 16) |
                                (built[0x26] << 8) | built[0x27];
    CHECK(built_next >= 454 && built_next <= 455);
}

void test_beat_packet_roundtrip_and_timing_formula() {
    section("beat packet round-trips and carries the right bar timings");
    uint8_t buf[128];
    for (uint8_t beat = 1; beat <= 4; ++beat) {
        const float bpm = 128.0f;
        CHECK_EQ(prolink::build_beat_packet(buf, sizeof buf, "box", 4, bpm, beat), 0x60);
        auto p = prolink::parse_beat_packet(buf, 0x60);
        CHECK(p.has_value());
        if (!p) continue;
        CHECK_EQ(p->device_num, 4);
        CHECK_EQ(p->beat_in_bar, beat);
        CHECK(std::fabs(p->track_bpm() - bpm) < 0.01f);
        CHECK(std::fabs(p->pitch_multiplier() - 1.0f) < 1e-6f);

        // Derived empirically from the captures: as multiples of the beat
        // interval, next beat = 1 and next bar = (5 - beat).
        const double interval = 60000.0 / bpm;
        CHECK_EQ(p->ms_next_beat, (uint32_t)(interval + 0.5));
        CHECK_EQ(p->ms_next_bar,  (uint32_t)(interval * (5 - beat) + 0.5));
    }
}

// ── Status packets ─────────────────────────────────────────────────────────

void test_status_packet_master_fields() {
    section("status packet carries the master designation");
    uint8_t buf[320];
    const size_t n = prolink::build_status_packet(buf, sizeof buf, "xdj-bridge",
                                                  /*device*/ 4, 128.0f, /*beat*/ 3,
                                                  /*is_master*/ true, /*syncn*/ 41,
                                                  /*master_handoff*/ 0xFF);
    CHECK_EQ(n, 284);
    auto p = prolink::parse_status_packet(buf, n);
    CHECK(p.has_value());
    if (!p) return;

    CHECK_EQ(p->device_num, 4);
    CHECK(p->is_master());               // flags 0x89 bit 5
    CHECK(p->is_playing());
    CHECK(p->bpm_valid());               // Mv == 0x8000
    CHECK_EQ(p->beat_in_bar, 3);
    CHECK_EQ(p->syncn, 41);
    CHECK_EQ(p->master_handoff, 0xFF);
    CHECK(std::fabs(p->track_bpm() - 128.0f) < 0.01f);
    CHECK(std::fabs(p->pitch_multiplier() - 1.0f) < 1e-6f);
    // The two fields that actually designate the master, at their exact offsets.
    CHECK_EQ(buf[0x9E], 0x01);           // Mm
    CHECK_EQ((buf[0x89] >> 5) & 1, 1);   // flags bit 5
}

void test_status_packet_follower_and_handoff() {
    section("status packet as follower, and while yielding master");
    uint8_t buf[320];
    prolink::build_status_packet(buf, sizeof buf, "box", 4, 120.0f, 1,
                                 /*is_master*/ false, 7, 0xFF);
    auto p = prolink::parse_status_packet(buf, 284);
    CHECK(p.has_value());
    if (p) CHECK(!p->is_master());
    CHECK_EQ(buf[0x9E], 0x00);

    // Yielding master to device 3 must advertise that in Mh.
    prolink::build_status_packet(buf, sizeof buf, "box", 4, 120.0f, 1,
                                 /*is_master*/ true, 7, /*master_handoff*/ 3);
    auto q = prolink::parse_status_packet(buf, 284);
    CHECK(q.has_value());
    if (q) CHECK_EQ(q->master_handoff, 3);
    CHECK_EQ(buf[0x9F], 3);
}

// ── Tempo-master handshake ─────────────────────────────────────────────────

void test_master_handoff_request_bytes() {
    section("0x26 takeover request matches beat-link's payload");
    uint8_t buf[64];
    const size_t n = prolink::build_master_handoff_request(buf, sizeof buf,
                                                           "xdj-bridge", 4);
    CHECK_EQ(n, 40);                                   // 0x1f header + 9 payload
    CHECK_EQ(buf[0x0A], prolink::PKT_TYPE_MASTER_HANDOFF_REQ);
    CHECK(std::memcmp(buf, prolink::MAGIC, sizeof prolink::MAGIC) == 0);
    CHECK(std::memcmp(buf + 0x0B, "xdj-bridge", 10) == 0);

    // Payload at 0x1f: 01 00 <dev> 00 04 00 00 00 <dev>
    const uint8_t want[9] = {0x01, 0x00, 4, 0x00, 0x04, 0x00, 0x00, 0x00, 4};
    for (size_t i = 0; i < sizeof want; ++i) CHECK_EQ(buf[0x1F + i], want[i]);
}

void test_master_handoff_response_bytes() {
    section("0x27 yield ACK matches beat-link's payload");
    uint8_t buf[64];
    const size_t n = prolink::build_master_handoff_response(buf, sizeof buf,
                                                            "xdj-bridge", 4);
    CHECK_EQ(n, 44);                                   // 0x1f header + 13 payload
    CHECK_EQ(buf[0x0A], prolink::PKT_TYPE_MASTER_HANDOFF_RESP);
    const uint8_t want[13] = {0x01, 0x00, 4, 0x00, 0x08, 0x00, 0x00, 0x00, 4,
                              0x00, 0x00, 0x00, 0x01};
    for (size_t i = 0; i < sizeof want; ++i) CHECK_EQ(buf[0x1F + i], want[i]);
}

void test_sync_control_bytes() {
    section("0x2a sync-control (appoint master) payload");
    uint8_t buf[64];
    const size_t n = prolink::build_sync_control_packet(
        buf, sizeof buf, "box", 4, prolink::SYNC_CMD_BECOME_MASTER);
    CHECK_EQ(n, 44);
    CHECK_EQ(buf[0x0A], prolink::PKT_TYPE_SYNC_CONTROL);
    CHECK_EQ(buf[0x1F + 0x02], 4);     // sender device number
    CHECK_EQ(buf[0x1F + 0x08], 4);
    CHECK_EQ(buf[0x1F + 0x0C], prolink::SYNC_CMD_BECOME_MASTER);
}

// ── Device-number claim handshake ──────────────────────────────────────────

void test_device_claim_packets() {
    section("device-number claim stages");
    uint8_t buf[64];
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0xDB, 0xC3, 0x42};

    CHECK_EQ(prolink::build_announce_hello(buf, sizeof buf, "box"), 0x26);
    CHECK_EQ(buf[0x0A], 0x0A);
    CHECK_EQ(buf[0x23], 0x26);                 // self-declared length
    CHECK(std::memcmp(buf + 0x0C, "box", 3) == 0);   // name at 0x0C here, not 0x0B

    CHECK_EQ(prolink::build_claim_stage1(buf, sizeof buf, "box", mac, 2), 0x2C);
    CHECK_EQ(buf[0x0A], 0x00);
    CHECK_EQ(buf[0x23], 0x2C);
    CHECK_EQ(buf[0x24], 2);                    // packet counter
    CHECK(std::memcmp(buf + 0x26, mac, 6) == 0);

    CHECK_EQ(prolink::build_claim_stage2(buf, sizeof buf, "box", mac,
                                         0xA9FE2A2Au, /*device*/ 4,
                                         /*counter*/ 3, /*auto*/ false), 0x32);
    CHECK_EQ(buf[0x0A], 0x02);
    CHECK_EQ(buf[0x24], 0xA9);                 // our IP, big-endian
    CHECK_EQ(buf[0x27], 0x2A);
    CHECK(std::memcmp(buf + 0x28, mac, 6) == 0);
    CHECK_EQ(buf[0x2E], 4);                    // the number being claimed
    CHECK_EQ(buf[0x2F], 3);                    // counter
    CHECK_EQ(buf[0x31], 0x02);                 // 2 = claiming a specific number

    CHECK_EQ(prolink::build_claim_stage3(buf, sizeof buf, "box", 4, 1), 0x26);
    CHECK_EQ(buf[0x0A], 0x04);
    CHECK_EQ(buf[0x24], 4);
    CHECK_EQ(buf[0x25], 1);
}

// ── Keep-alive ─────────────────────────────────────────────────────────────

void test_keepalive_packet() {
    section("virtual-CDJ keep-alive");
    uint8_t buf[64];
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    CHECK_EQ(prolink::build_keepalive_packet(buf, sizeof buf, "box", 7, mac,
                                             0xA9FE2A2Au), 0x36);
    CHECK_EQ(buf[0x0A], prolink::PKT_TYPE_KEEPALIVE);
    CHECK_EQ(buf[0x24], 7);                    // device number
    CHECK(std::memcmp(buf + 0x26, mac, 6) == 0);
    CHECK_EQ(buf[0x2C], 0xA9);                 // IP, big-endian
    CHECK_EQ(buf[0x2F], 0x2A);
}

// ── Parser hardening ───────────────────────────────────────────────────────

void test_parsers_reject_bad_input() {
    section("parsers reject malformed input");
    uint8_t buf[320];
    prolink::build_beat_packet(buf, sizeof buf, "x", 1, 120.0f, 1);
    CHECK(!prolink::parse_beat_packet(buf, 0x40).has_value());   // truncated
    buf[0] ^= 0xFF;
    CHECK(!prolink::parse_beat_packet(buf, 0x60).has_value());   // bad magic

    prolink::build_status_packet(buf, sizeof buf, "x", 1, 120.0f, 1, true, 1, 0xFF);
    CHECK(!prolink::parse_status_packet(buf, 0x50).has_value()); // truncated
    // A status packet must not parse as a beat packet, or vice versa.
    CHECK(!prolink::parse_beat_packet(buf, 284).has_value());
}

void test_pitch_decoding() {
    section("pitch decodes as a multiplier, not a percentage");
    uint8_t buf[128];
    prolink::build_beat_packet(buf, sizeof buf, "x", 1, 136.0f, 1);
    auto p = prolink::parse_beat_packet(buf, 0x60);
    CHECK(p.has_value());
    if (!p) return;
    // Unity pitch is 0x00100000, and effective BPM is track BPM x multiplier.
    CHECK_EQ(p->pitch_raw, prolink::PITCH_UNITY);
    CHECK(std::fabs(p->pitch_percent()) < 0.01f);
    CHECK(std::fabs(p->effective_bpm() - 136.0f) < 0.01f);
}

// ── Clock ──────────────────────────────────────────────────────────────────

class FakeTimer : public prolink::ITimer {
public:
    void start(std::function<uint32_t()> on_tick) override { on_tick_ = std::move(on_tick); }
    void stop() override { on_tick_ = nullptr; }
    uint64_t now_us() const override { return now_; }
    uint32_t tick() { return on_tick_ ? on_tick_() : 0; }
    bool running() const { return on_tick_ != nullptr; }
    uint64_t now_ = 0;
private:
    std::function<uint32_t()> on_tick_;
};

class FakeMidi : public prolink::IMidiOut {
public:
    void send_byte(uint8_t b) override { bytes.push_back(b); }
    std::vector<uint8_t> bytes;
};

void test_clock_tempo_and_transport() {
    section("clock: tempo maths and transport bytes");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, /*gain*/ 16);

    clock.update_tempo_bpm(120.0f);
    // 24 PPQN at 120 BPM = 500 ms per beat = 20833 us per tick.
    CHECK_EQ(clock.current_tick_period_us(), 20833);
    clock.update_tempo_bpm(140.0f);
    CHECK_EQ(clock.current_tick_period_us(), 60000000u / 140 / 24);

    clock.start();
    CHECK(clock.is_running());
    CHECK(!midi.bytes.empty() && midi.bytes.front() == prolink::MIDI_START);

    for (int i = 0; i < 24; ++i) timer.tick();
    // One beat of ticks: 24 clock bytes after the Start.
    size_t clocks = 0;
    for (uint8_t b : midi.bytes) if (b == prolink::MIDI_CLOCK) ++clocks;
    CHECK_EQ(clocks, 24);

    clock.stop();
    CHECK(!clock.is_running());
    CHECK(midi.bytes.back() == prolink::MIDI_STOP);

    // Implausible tempos must not take effect.
    const uint32_t before = clock.current_tick_period_us();
    clock.update_tempo_bpm(0.0f);
    clock.update_tempo_bpm(-5.0f);
    CHECK_EQ(clock.current_tick_period_us(), before);
}

void test_clock_offset_is_tempo_independent() {
    section("clock: lead-time offset is a constant time, not a fraction of a beat");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, 16);
    clock.set_offset_us(30000);
    CHECK_EQ(clock.get_offset_us(), 30000);
    clock.update_tempo_bpm(120.0f);
    CHECK_EQ(clock.get_offset_us(), 30000);   // unchanged by a tempo change
    clock.update_tempo_bpm(174.0f);
    CHECK_EQ(clock.get_offset_us(), 30000);
}


// ── Clock: PLL behaviour ───────────────────────────────────────────────────
//
// The phase-lock loop is the heart of timing accuracy: it must converge on the
// master's beat, bleed error off gradually (rather than jerking the tick
// stream), and never ask the timer for a nonsensical interval.

void test_clock_phase_error_converges() {
    section("clock: phase error is bled off gradually, not applied at once");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, /*gain*/ 16);
    clock.update_tempo_bpm(120.0f);
    clock.start();

    // Land a beat late enough to look like a large positive phase error.
    timer.now_ = 0;
    timer.tick();                    // tick 0 anchors the beat
    timer.now_ = 30000;              // 30 ms after our beat boundary
    clock.feed_beat(500, 1);
    const int32_t err0 = clock.current_phase_error_us();
    CHECK(err0 > 0);                 // we are leading, so slow down

    // Each tick may only take roughly 1/gain of the remaining error, so a
    // single tick must not swallow it all — that would pass network jitter
    // straight through to the slave.
    const uint32_t next = timer.tick();
    const int32_t err1 = clock.current_phase_error_us();
    CHECK(std::abs(err1) < std::abs(err0));          // converging
    CHECK(std::abs(err1) > std::abs(err0) / 4);      // but not in one jump
    // The interval was stretched, not collapsed.
    CHECK(next > clock.current_tick_period_us());

    // Many ticks later the error has settled. The correction is integer
    // division by the gain, so once the residual drops below the divisor the
    // correction rounds to zero and the last few microseconds stay — 16 us is
    // four orders of magnitude below a MIDI tick, so that floor is harmless.
    for (int i = 0; i < 200; ++i) timer.tick();
    CHECK(std::abs(clock.current_phase_error_us()) < 16);
}

void test_clock_phase_error_wraps_the_short_way() {
    section("clock: phase error takes the shortest way round the beat");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, 16);
    clock.update_tempo_bpm(120.0f);   // 500 ms per beat
    clock.start();

    timer.now_ = 0;
    timer.tick();
    // A beat arriving 490 ms "late" is really 10 ms early: correcting forwards
    // by 490 ms instead of back by 10 ms would audibly lurch the slave.
    timer.now_ = 490000;
    clock.feed_beat(500, 1);
    const int32_t err = clock.current_phase_error_us();
    CHECK(std::abs(err) < 250000);    // within half a beat either way
}

void test_clock_never_requests_a_degenerate_interval() {
    section("clock: a corrupt phase error cannot stall or spin the timer");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, /*gain*/ 1);   // worst case: apply it all
    clock.update_tempo_bpm(120.0f);
    clock.start();

    // Enormous negative offset — the correction wants a hugely negative
    // interval. The floor must keep it positive and sane.
    clock.set_offset_us(2000000);
    for (int i = 0; i < 50; ++i) {
        const uint32_t next = timer.tick();
        CHECK(next > 0);
        CHECK(next < 4000000);        // never longer than a few seconds
    }
}

void test_clock_gain_zero_is_not_a_division_by_zero() {
    section("clock: a zero gain divisor is coerced, not fatal");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, /*gain*/ 0);
    clock.update_tempo_bpm(120.0f);
    clock.start();
    for (int i = 0; i < 10; ++i) CHECK(timer.tick() > 0);
}

void test_clock_tracks_bar_position_from_beats() {
    section("clock: bar position follows the beats it is fed");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, 16);
    clock.update_tempo_bpm(120.0f);
    clock.start();
    for (uint8_t beat = 1; beat <= 4; ++beat) {
        clock.feed_beat(500, beat);
        CHECK_EQ(clock.current_beat_in_bar(), beat);
    }
}

void test_clock_ticks_only_while_running() {
    section("clock: no ticks are emitted while stopped");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, 16);
    clock.update_tempo_bpm(120.0f);

    // Not started: the timer callback must decline to reschedule.
    CHECK_EQ(timer.tick(), 0);

    clock.start();
    const uint64_t before = clock.ticks_emitted_total();
    for (int i = 0; i < 12; ++i) timer.tick();
    CHECK_EQ(clock.ticks_emitted_total() - before, 12);

    clock.stop();
    const uint64_t after_stop = clock.ticks_emitted_total();
    timer.tick();
    CHECK_EQ(clock.ticks_emitted_total(), after_stop);   // no further ticks
}

void test_clock_feed_beat_ignored_while_stopped() {
    section("clock: beats fed while stopped do not accumulate phase error");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, 16);
    clock.update_tempo_bpm(120.0f);
    timer.now_ = 999999;
    clock.feed_beat(500, 1);
    CHECK_EQ(clock.current_phase_error_us(), 0);
}

void test_clock_restart_clears_stale_phase() {
    section("clock: restarting clears stale phase error and bar position");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, 16);
    clock.update_tempo_bpm(120.0f);
    clock.start();
    timer.now_ = 0;
    timer.tick();
    timer.now_ = 40000;
    clock.feed_beat(500, 1);
    CHECK(clock.current_phase_error_us() != 0);

    clock.stop();
    clock.start();
    // A fresh start must not inherit the old error, or the first bar after a
    // re-sync would be dragged by whatever happened before it.
    CHECK_EQ(clock.current_phase_error_us(), 0);
    CHECK_EQ(clock.current_tick_in_beat(), 0);
}

void test_clock_tempo_bounds() {
    section("clock: absurd tempos are clamped into a usable range");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, 16);

    clock.update_tempo_bpm(10000.0f);        // far above any real tempo
    CHECK(clock.current_tick_period_us() >= 60000000u / 300u / 24u);
    clock.update_tempo_bpm(0.5f);            // far below
    CHECK(clock.current_tick_period_us() <= 60000000u / 20u / 24u);

    // NaN and infinity must be refused outright rather than poisoning the timer.
    const uint32_t sane = clock.current_tick_period_us();
    clock.update_tempo_bpm(std::nanf(""));
    CHECK_EQ(clock.current_tick_period_us(), sane);
    clock.update_tempo_bpm(INFINITY);
    CHECK_EQ(clock.current_tick_period_us(), sane);
}

void test_clock_offset_change_is_audible_quickly() {
    section("clock: an offset nudge takes effect within a tick or two");
    FakeMidi midi;
    FakeTimer timer;
    prolink::Clock clock(midi, timer, 16);
    clock.update_tempo_bpm(120.0f);
    clock.start();
    timer.tick();

    const int32_t before = clock.current_phase_error_us();
    clock.set_offset_us(10000);        // operator nudges +10 ms of lead
    const int32_t after = clock.current_phase_error_us();
    // The delta is injected scaled by the gain so the very next tick applies
    // most of it — calibrating by ear needs immediate feedback.
    CHECK(after != before);
    CHECK(std::abs(after - before) >= 10000);
}

// ── Hardening against garbage off the network ──────────────────────────────
//
// Beat and status packets are attacker- or noise-controlled. Anything the
// bridge re-broadcasts as tempo master must survive whatever arrives.

void test_absurd_tempo_is_rejected() {
    section("packets: a tempo outside the wire range is refused, not truncated");
    uint8_t buf[320];
    // bpm*100 must fit a uint16; 1310 BPM would wrap to ~655 while the timing
    // fields were computed from the real value — a self-inconsistent grid.
    CHECK_EQ(prolink::build_beat_packet(buf, sizeof buf, "x", 4, 1310.7f, 1), 0);
    CHECK_EQ(prolink::build_beat_packet(buf, sizeof buf, "x", 4, 0.0001f, 1), 0);
    CHECK_EQ(prolink::build_beat_packet(buf, sizeof buf, "x", 4, -5.0f, 1), 0);
    CHECK_EQ(prolink::build_status_packet(buf, sizeof buf, "x", 4, 1310.7f, 1,
                                          true, 1, 0xFF), 0);
    // Real tempos still build.
    CHECK(prolink::build_beat_packet(buf, sizeof buf, "x", 4, 174.0f, 1) == 0x60);
    CHECK(prolink::bpm_is_sane(128.0f));
    CHECK(!prolink::bpm_is_sane(1310.7f));
}

void test_parsers_reject_short_buffers_safely() {
    section("packets: a bare magic header is rejected without reading past it");
    // Exactly the magic and nothing else. parse_status_packet used to check
    // the type byte at 0x0A before the length, reading one byte off the end.
    uint8_t only_magic[10];
    std::memcpy(only_magic, prolink::MAGIC, sizeof only_magic);
    CHECK(!prolink::parse_status_packet(only_magic, sizeof only_magic).has_value());
    CHECK(!prolink::parse_beat_packet(only_magic, sizeof only_magic).has_value());
    CHECK(!prolink::parse_status_packet(only_magic, 0).has_value());
    CHECK(prolink::has_prolink_magic(only_magic, sizeof only_magic));
    only_magic[3] ^= 0xFF;
    CHECK(!prolink::has_prolink_magic(only_magic, sizeof only_magic));
}
}  // namespace

// Defined in test_bridge.cpp; returns its failure count.
int bridge_tests_main();

int main() {
    test_beat_packet_matches_real_hardware();
    test_beat_packet_roundtrip_and_timing_formula();
    test_status_packet_master_fields();
    test_status_packet_follower_and_handoff();
    test_master_handoff_request_bytes();
    test_master_handoff_response_bytes();
    test_sync_control_bytes();
    test_device_claim_packets();
    test_keepalive_packet();
    test_parsers_reject_bad_input();
    test_pitch_decoding();
    test_clock_tempo_and_transport();
    test_clock_offset_is_tempo_independent();
    test_clock_phase_error_converges();
    test_clock_phase_error_wraps_the_short_way();
    test_clock_never_requests_a_degenerate_interval();
    test_clock_gain_zero_is_not_a_division_by_zero();
    test_clock_tracks_bar_position_from_beats();
    test_clock_ticks_only_while_running();
    test_clock_feed_beat_ignored_while_stopped();
    test_clock_restart_clears_stale_phase();
    test_clock_tempo_bounds();
    test_clock_offset_change_is_audible_quickly();
    test_absurd_tempo_is_rejected();
    test_parsers_reject_short_buffers_safely();

    std::printf("\npackets/clock: %d checks, %d failure%s\n\n", g_checks, g_failures,
                g_failures == 1 ? "" : "s");

    const int bridge_failures = bridge_tests_main();
    const int total = g_failures + bridge_failures;
    std::printf("\n%s\n", total == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return total == 0 ? 0 : 1;
}
