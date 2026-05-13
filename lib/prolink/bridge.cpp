#include "bridge.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace prolink {

namespace {

uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr uint32_t RECV_TIMEOUT_MS = 5;
constexpr size_t   RECV_BUF_LEN    = 1024;

}  // namespace

Bridge::Bridge(IUdpSocket& beat_sock,
               IUdpSocket& status_sock,
               IUdpSocket& keepalive_sock,
               IClockSink& clock,
               BridgeConfig cfg,
               BridgeCallbacks cb)
    : beat_sock_(beat_sock)
    , status_sock_(status_sock)
    , keepalive_sock_(keepalive_sock)
    , clock_(clock)
    , cfg_(std::move(cfg))
    , cb_(std::move(cb)) {
    last_known_bpm_.store(cfg_.fallback_bpm);
}

void Bridge::set_clock_offset_ms(float ms) {
    clock_offset_ms_.store(ms);
    push_offset_to_clock_();
}

void Bridge::set_grid_offset_ms(float ms) {
    grid_offset_ms_.store(ms);
    push_offset_to_clock_();
}

void Bridge::adjust_clock_offset_ms(float delta_ms) {
    set_clock_offset_ms(clock_offset_ms_.load() + delta_ms);
}

void Bridge::adjust_grid_offset_ms(float delta_ms) {
    set_grid_offset_ms(grid_offset_ms_.load() + delta_ms);
}

void Bridge::reset_grid_offset() {
    set_grid_offset_ms(0.0f);
}

void Bridge::push_offset_to_clock_() {
    float total_ms = clock_offset_ms_.load() + grid_offset_ms_.load();
    clock_.set_offset_us(static_cast<int32_t>(total_ms * 1000.0f));
}

void Bridge::run() {
    running_.store(true);
    last_keepalive_ms_ = 0;
    last_packet_ms_    = now_ms();

    uint8_t buf[RECV_BUF_LEN];

    while (running_.load()) {
        // Round-robin both listener sockets with short timeouts.
        int n = beat_sock_.recv(buf, sizeof(buf), RECV_TIMEOUT_MS);
        if (n > 0) handle_beat_packet(buf, static_cast<size_t>(n));

        n = status_sock_.recv(buf, sizeof(buf), RECV_TIMEOUT_MS);
        if (n > 0) handle_status_packet(buf, static_cast<size_t>(n));

        uint64_t t = now_ms();
        maybe_send_keepalive(t);
        maybe_stop_on_silence(t);
    }

    // Make sure the clock isn't left running after run() returns.
    if (playing_.exchange(false)) {
        clock_.stop();
    }
}

void Bridge::stop() {
    running_.store(false);
}

void Bridge::handle_beat_packet(const uint8_t* buf, size_t len) {
    auto parsed = parse_beat_packet(buf, len);
    if (!parsed) return;
    last_packet_ms_ = now_ms();
    beat_count_.fetch_add(1, std::memory_order_relaxed);

    // Initial master claim: with only one device on the link, the first beat
    // we see *is* the master. Status packets will overwrite this once they
    // start flowing.
    if (current_master_.load() == 0) {
        current_master_.store(parsed->device_num);
        if (cfg_.verbose) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "claiming device %u (%s) as master (beat-only)",
                          parsed->device_num, parsed->device_name);
            log(msg);
        }
    }

    if (parsed->device_num != current_master_.load()) return;

    // Master-filtered. Fire callback so the GUI only sees the active deck —
    // not e.g. deck 2's idle status from a combined unit like the XDJ-XZ.
    if (cb_.on_beat) cb_.on_beat(*parsed);

    // Always update tempo from beat packets — effective_bpm() encodes the
    // pitch slider, so this tracks tempo changes even when status packets
    // aren't flowing (no virtual CDJ, or pcapng replay). Status packets
    // at ~5 Hz take over as the primary source once they start flowing;
    // both encode the same value so double-updating is harmless.
    {
        float bpm = parsed->effective_bpm();
        if (bpm > 0.0f) {
            last_known_bpm_.store(bpm);
            clock_.update_tempo_bpm(bpm);
        }
    }

    // Hold MIDI Start until the next downbeat so the slave's bar 1 lines up
    // with the master's bar 1.
    if (!playing_.load()) {
        if (parsed->beat_in_bar == 1) {
            playing_.store(true);
            waiting_for_downbeat_ = false;
            clock_.update_tempo_bpm(parsed->effective_bpm());
            clock_.start();
            expected_beat_in_bar_ = 1;
            bar_slip_pending_realign_ = false;
            if (cfg_.verbose) log("start (downbeat)");
        } else if (!waiting_for_downbeat_) {
            waiting_for_downbeat_ = true;
            if (cfg_.verbose) {
                char msg[64];
                std::snprintf(msg, sizeof(msg),
                              "waiting for downbeat (got beat %u/4)",
                              parsed->beat_in_bar);
                log(msg);
            }
        }
        return;
    }

    // Bar-slip detection: the XZ advances beat_in_bar 1→2→3→4→1. If we miss
    // a packet, our next observed beat_in_bar disagrees with what we expect.
    // We can't fix bar alignment with a soft phase nudge (the slave doesn't
    // know what bar it's in via MIDI clock alone); instead, queue a Stop+Start
    // on the next downbeat so bar 1 of the slave realigns with bar 1 of the
    // master.
    if (expected_beat_in_bar_ != 0) {
        uint8_t expected_next = (expected_beat_in_bar_ % 4) + 1;
        if (parsed->beat_in_bar != expected_next && !bar_slip_pending_realign_) {
            bar_slip_pending_realign_ = true;
            if (cfg_.verbose) {
                char msg[80];
                std::snprintf(msg, sizeof(msg),
                              "bar slip: expected beat %u, got %u — realign on next downbeat",
                              expected_next, parsed->beat_in_bar);
                log(msg);
            }
        }
    }
    expected_beat_in_bar_ = parsed->beat_in_bar;

    if (bar_slip_pending_realign_ && parsed->beat_in_bar == 1) {
        clock_.stop();
        clock_.update_tempo_bpm(parsed->effective_bpm());
        clock_.start();
        bar_slip_pending_realign_ = false;
        if (cfg_.verbose) log("realigned on downbeat (Stop+Start)");
        return;  // skip the soft phase correction; we just hard-reset
    }

    clock_.correct_phase(parsed->beat_in_bar);
}

void Bridge::handle_status_packet(const uint8_t* buf, size_t len) {
    auto parsed = parse_status_packet(buf, len);
    if (!parsed) return;
    last_packet_ms_ = now_ms();
    status_count_.fetch_add(1, std::memory_order_relaxed);

    // Master tracking: whoever currently has master+playing flags wins.
    if (parsed->is_master() && parsed->is_playing()) {
        if (current_master_.load() != parsed->device_num) {
            current_master_.store(parsed->device_num);
            if (cfg_.verbose) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "master is now device %u (%s)",
                              parsed->device_num, parsed->device_name);
                log(msg);
            }
        }
    }

    if (parsed->device_num != current_master_.load()) return;

    // Master-filtered. Fire callback so the GUI only sees the active deck.
    // The XDJ-XZ broadcasts status for both internal decks; without this
    // filter the GUI alternates between deck 1's real numbers and deck 2's
    // idle/loading state (causing BPM to oscillate, Mv-valid to flicker).
    if (cb_.on_status) cb_.on_status(*parsed);

    // Tempo update — only when the BPM is trustworthy.
    if (parsed->bpm_valid() && parsed->effective_bpm() > 0.0f) {
        float bpm = parsed->effective_bpm();
        last_known_bpm_.store(bpm);
        clock_.update_tempo_bpm(bpm);
    }

    // Play-state debounce — the XZ flickers the play flag during cue scrubs.
    bool master_playing = parsed->is_playing();
    uint64_t t = now_ms();
    if (master_playing != pending_play_state_) {
        pending_play_state_      = master_playing;
        pending_play_change_ms_  = t;
    }
    if (last_master_playing_ != pending_play_state_ &&
        (t - pending_play_change_ms_) >= cfg_.play_debounce_ms) {
        last_master_playing_ = pending_play_state_;
        if (!last_master_playing_ && playing_.load()) {
            // Playing → stopped: kill the clock immediately.
            playing_.store(false);
            waiting_for_downbeat_ = false;
            expected_beat_in_bar_ = 0;
            bar_slip_pending_realign_ = false;
            clock_.stop();
            if (cfg_.verbose) log("stop (master paused)");
        }
        // Stopped → playing transitions are handled by the next beat packet
        // (we hold Start until beat_in_bar == 1).
    }
}

void Bridge::maybe_send_keepalive(uint64_t t) {
    if (!cfg_.send_vcdj_announce) return;
    if (t - last_keepalive_ms_ < cfg_.keepalive_period_ms) return;
    last_keepalive_ms_ = t;

    uint8_t pkt[64];
    size_t n = build_keepalive_packet(pkt, sizeof(pkt),
                                      cfg_.device_name,
                                      cfg_.device_num,
                                      cfg_.mac,
                                      cfg_.local_ip);
    if (n == 0) return;
    bool ok = keepalive_sock_.send(pkt, n, cfg_.broadcast_ip, PORT_KEEPALIVE);
    if (cfg_.verbose) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "keepalive → %u.%u.%u.%u:%u  device=%u  %s",
                      (cfg_.broadcast_ip >> 24) & 0xFF,
                      (cfg_.broadcast_ip >> 16) & 0xFF,
                      (cfg_.broadcast_ip >>  8) & 0xFF,
                       cfg_.broadcast_ip        & 0xFF,
                      PORT_KEEPALIVE,
                      cfg_.device_num,
                      ok ? "ok" : "FAILED");
        log(msg);
    }
}

void Bridge::maybe_stop_on_silence(uint64_t t) {
    if (!playing_.load()) return;
    if (t - last_packet_ms_ < cfg_.silence_timeout_ms) return;
    playing_.store(false);
    waiting_for_downbeat_ = false;
    last_master_playing_ = false;
    pending_play_state_  = false;
    expected_beat_in_bar_ = 0;
    bar_slip_pending_realign_ = false;
    clock_.stop();
    if (cfg_.verbose) log("stop (silence timeout)");
}

void Bridge::log(const char* msg) const {
    if (cb_.on_log) {
        cb_.on_log(msg);
    } else {
        std::fprintf(stderr, "[bridge] %s\n", msg);
    }
}

}  // namespace prolink
