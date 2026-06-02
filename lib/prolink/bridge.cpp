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

void Bridge::set_force_master_device(uint8_t device_num) {
    // Mirrors cfg.force_master_device but settable live (front-panel
    // encoder). Storing current_master_ here makes the change take effect
    // immediately: device_num==0 re-bootstraps auto-tracking from the next
    // packet, non-zero pins to that device. The per-packet master logic
    // re-reads cfg_.force_master_device, so a cross-task byte write is fine.
    cfg_.force_master_device = device_num;
    current_master_.store(device_num);
}

void Bridge::push_offset_to_clock_() {
    float total_ms = clock_offset_ms_.load() + grid_offset_ms_.load();
    clock_.set_offset_us(static_cast<int32_t>(total_ms * 1000.0f));
}

void Bridge::apply_tempo_(float bpm) {
    if (bpm <= 0.0f) return;
    float alpha = cfg_.bpm_smoothing_alpha;
    if (alpha <= 0.0f) alpha = 1.0f;
    if (alpha > 1.0f)  alpha = 1.0f;

    if (smoothed_bpm_ <= 0.0f) {
        smoothed_bpm_ = bpm;        // bootstrap — first update lands directly
    } else {
        smoothed_bpm_ = alpha * bpm + (1.0f - alpha) * smoothed_bpm_;
    }
    last_known_bpm_.store(smoothed_bpm_);
    clock_.update_tempo_bpm(smoothed_bpm_);
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

    // Master claim from beat packets. If --follow-device was passed, pin
    // to that. Otherwise (auto mode): with only one device on the link,
    // the first beat we see is the master. Status packets will overwrite
    // this once they start flowing.
    if (cfg_.force_master_device != 0) {
        if (current_master_.load() != cfg_.force_master_device) {
            current_master_.store(cfg_.force_master_device);
        }
    } else if (current_master_.load() == 0) {
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

    // Beat-packet tempo is a fallback. Status packets are the lower-jitter
    // source (5 Hz, encode the same pitch_multiplier × track_bpm). Driving
    // the clock from BOTH sources at once means slight sample-timing
    // differences between them kick the period back and forth — a real
    // contributor to GUI BPM jitter we observed in live testing. We only
    // accept beat-packet tempo when status packets haven't been seen for
    // status_silence_fallback_ms (default 500 ms — ~2 missed status packets
    // at 5 Hz).
    {
        float bpm = parsed->effective_bpm();
        if (bpm > 0.0f) {
            bool status_recently_seen =
                (last_status_ms_ != 0) &&
                ((now_ms() - last_status_ms_) < cfg_.status_silence_fallback_ms);
            if (!status_recently_seen) {
                apply_tempo_(bpm);
            } else {
                // Keep the "last known BPM" diagnostic up to date even when
                // we don't drive the clock, so the GUI's master panel still
                // shows the current effective BPM from beat packets.
                last_known_bpm_.store(bpm);
            }
        }
    }

    // Hold MIDI Start until the next downbeat so the slave's bar 1 lines up
    // with the master's bar 1.
    if (!playing_.load()) {
        if (parsed->beat_in_bar == 1) {
            playing_.store(true);
            waiting_for_downbeat_ = false;
            // Fresh start — seed the smoother directly with the current
            // value so the first beat fires at the right tempo rather
            // than lagging behind via EMA convergence.
            smoothed_bpm_ = parsed->effective_bpm();
            last_known_bpm_.store(smoothed_bpm_);
            clock_.update_tempo_bpm(smoothed_bpm_);
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
        smoothed_bpm_ = parsed->effective_bpm();
        last_known_bpm_.store(smoothed_bpm_);
        clock_.update_tempo_bpm(smoothed_bpm_);
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
    uint64_t t = now_ms();
    last_packet_ms_ = t;
    last_status_ms_ = t;
    status_count_.fetch_add(1, std::memory_order_relaxed);

    // Master tracking. Three cases:
    //   (0) Pinned — cfg_.force_master_device != 0. Always follow the named
    //       device; ignore is_master entirely. Useful when you want the box
    //       to follow deck 1 specifically regardless of who has the master
    //       button lit.
    //   (1) Auto, steady state — follow whoever has the is_master flag set.
    //       We do NOT also require is_playing, because the master flag is
    //       the Pioneer protocol's "this is the tempo authority"
    //       designation and transfers between decks independent of play
    //       state. A paused master is still the deck we should track.
    //   (2) Auto, bootstrap — if no device has been identified as master yet
    //       (we just started up, or no deck has claimed the flag), latch
    //       onto the first status-sending device. A real master claim will
    //       supersede this on the next packet that has is_master set.
    if (cfg_.force_master_device != 0) {
        if (current_master_.load() != cfg_.force_master_device) {
            current_master_.store(cfg_.force_master_device);
            if (cfg_.verbose) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "master pinned to device %u (--follow-device)",
                              cfg_.force_master_device);
                log(msg);
            }
        }
    } else {
        bool claim_master = parsed->is_master() ||
                            current_master_.load() == 0;
        if (claim_master && current_master_.load() != parsed->device_num) {
            current_master_.store(parsed->device_num);
            if (cfg_.verbose) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "master is now device %u (%s)%s",
                              parsed->device_num, parsed->device_name,
                              parsed->is_master() ? "" : " (bootstrap)");
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

    // Tempo update — only when the BPM is trustworthy. Routed through the
    // EMA-smoothing helper so brief noise in successive status packets
    // doesn't kick the tick period directly.
    if (parsed->bpm_valid() && parsed->effective_bpm() > 0.0f) {
        apply_tempo_(parsed->effective_bpm());
    }

    // Play-state debounce — the XZ flickers the play flag during cue scrubs.
    bool master_playing = parsed->is_playing();
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
            smoothed_bpm_ = 0.0f;   // reseed on next start
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
    smoothed_bpm_ = 0.0f;   // reseed on next start
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
