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

// A single dropped beat packet (UDP) makes the master's beat_in_bar appear to
// skip a number, but our free-running clock never actually lost bar alignment
// — so reacting to one mismatch with a hard Stop+Start just glitches the slave
// (the OP-XY "randomly stops"). Only realign after the bar position disagrees
// this many beats in a row, which a transient drop can't reach.
constexpr uint8_t  BAR_SLIP_REALIGN_THRESHOLD = 3;

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

void Bridge::set_manual_bpm(float bpm) {
    // Thread-safe (called from the UI task): touches only atomics + the
    // clock's atomic tempo. The bridge thread skips packet tempo while
    // manual_active_ is set, and (unless ignoring) clears it on a master beat.
    if (bpm < 20.0f)  bpm = 20.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    manual_bpm_.store(bpm);
    manual_active_.store(true);
    last_known_bpm_.store(bpm);   // so the display reflects the manual tempo
    clock_.update_tempo_bpm(bpm);
}

void Bridge::nudge_manual_bpm(float delta) {
    float base = manual_active_.load() ? manual_bpm_.load() : last_known_bpm_.load();
    set_manual_bpm(base + delta);
}

void Bridge::request_resync(bool immediate) {
    // Thread-safe: just latch the request. The bridge thread performs the
    // Stop+Start — deferred requests on the next master downbeat (in
    // handle_beat_packet), immediate ones in the run loop (maybe_resync).
    resync_request_.store(immediate ? 2 : 1);
}

void Bridge::set_master_mode(bool on) {
    master_mode_.store(on);
    if (on) {
        master_beat_in_bar_    = 0;
        last_master_status_ms_ = 0;
        master_confirmed_.store(false);
        // Keep requesting the handoff for a while (5 Hz cadence) until the
        // master yields; ~6 s ceiling so we don't spam forever if it never does.
        master_request_countdown_ = 30;
        // Master mode is standalone: we run our own tempo and ignore other
        // decks. set_ignore_master() latches the manual tempo and the run loop
        // cold-starts the clock, whose per-beat callback drives beat emission.
        set_ignore_master(true);
    }
}

void Bridge::on_master_beat() {
    if (!master_mode_.load()) return;
    master_beat_in_bar_ = static_cast<uint8_t>((master_beat_in_bar_ % 4) + 1);
    const float bpm = last_known_bpm_.load();
    if (!(bpm > 0.0f)) return;
    uint8_t pkt[128];
    size_t n = build_beat_packet(pkt, sizeof(pkt), cfg_.device_name,
                                 cfg_.device_num, bpm, master_beat_in_bar_);
    if (n) beat_sock_.send(pkt, n, cfg_.broadcast_ip, PORT_BEAT);
}

void Bridge::maybe_broadcast_master_status(uint64_t t) {
    if (!master_mode_.load()) return;
    if (t - last_master_status_ms_ < 200) return;   // ~5 Hz, like real players
    last_master_status_ms_ = t;
    const float bpm = last_known_bpm_.load();
    if (!(bpm > 0.0f)) return;

    // Takeover request burst: unicast a MASTER_HANDOFF_REQUEST (0x26) to the
    // current master on port 50001, asking it to yield to us (it replies 0x27
    // and sets its Mh to our device number). Keep asking until it yields
    // (master_confirmed_); needs the master's IP, learned from its status.
    if (!master_confirmed_.load() && master_request_countdown_ > 0) {
        const uint32_t mip = master_ip_.load();
        if (mip != 0) {
            --master_request_countdown_;
            uint8_t req[64];
            size_t rn = build_master_handoff_request(req, sizeof(req),
                                                     cfg_.device_name, cfg_.device_num);
            if (rn) {
                bool ok = beat_sock_.send(req, rn, mip, PORT_BEAT);
                char m[96];
                std::snprintf(m, sizeof(m),
                    "handoff 0x26 -> %u.%u.%u.%u:%u dev=%u %s (%d left)",
                    (mip >> 24) & 0xFF, (mip >> 16) & 0xFF, (mip >> 8) & 0xFF, mip & 0xFF,
                    PORT_BEAT, cfg_.device_num, ok ? "ok" : "FAIL", master_request_countdown_);
                log(m);
            }
        } else {
            log("master mode: waiting to learn the current master's IP");
        }
    }
    const uint8_t  beat  = master_beat_in_bar_ ? master_beat_in_bar_ : 1;
    const uint32_t syncn = max_syncn_seen_.load() + 1;   // outrank the peers
    // Only claim master (mm=1) once the current master has yielded; until then
    // we broadcast as a normal synced follower and keep sending 0x26 requests.
    const bool assert_master = master_confirmed_.load();
    uint8_t pkt[320];
    size_t n = build_status_packet(pkt, sizeof(pkt), cfg_.device_name,
                                   cfg_.device_num, bpm, beat, assert_master,
                                   syncn);
    if (n) status_sock_.send(pkt, n, cfg_.broadcast_ip, PORT_STATUS);
}

void Bridge::set_ignore_master(bool on) {
    ignore_master_.store(on);
    // Enabling forces the manual-tempo source (the run loop then cold-starts
    // the clock); disabling resumes normal sync.
    manual_active_.store(on);
}

void Bridge::push_offset_to_clock_() {
    float total_ms = clock_offset_ms_.load() + grid_offset_ms_.load();
    clock_.set_offset_us(static_cast<int32_t>(total_ms * 1000.0f));
}

void Bridge::apply_tempo_(float bpm) {
    if (bpm <= 0.0f) return;
    if (manual_active_.load()) return;  // manual tempo latched — don't override
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
        if (n > 0) {
            if (cb_.on_raw_datagram) cb_.on_raw_datagram(PORT_BEAT, buf, static_cast<size_t>(n));
            handle_beat_packet(buf, static_cast<size_t>(n));
        }

        uint32_t status_src = 0;
        n = status_sock_.recv(buf, sizeof(buf), RECV_TIMEOUT_MS, &status_src);
        if (n > 0) {
            last_status_src_ip_ = status_src;
            if (cb_.on_raw_datagram) cb_.on_raw_datagram(PORT_STATUS, buf, static_cast<size_t>(n));
            // The master's yield acknowledgement (0x27) arrives here, on the
            // status port — not on 50001 where we sent the request.
            if (n > 0x0A && buf[0x0A] == PKT_TYPE_MASTER_HANDOFF_RESP &&
                master_mode_.load() && !master_confirmed_.load()) {
                master_confirmed_.store(true);
                log("master yield ACK (0x27) received — asserting mm=1");
            }
            handle_status_packet(buf, static_cast<size_t>(n));
        }

        uint64_t t = now_ms();
        maybe_send_keepalive(t);
        maybe_stop_on_silence(t);
        maybe_resync(t);
        maybe_broadcast_master_status(t);

        // Standalone / free-run cold-start: if the manual tempo is the source
        // and nothing is running yet, start clocking (emits MIDI Start). Lets
        // "Off" mode (and manual-BPM in free-run) drive a synth with no deck.
        if ((free_run_.load() || ignore_master_.load()) &&
            manual_active_.load() && !playing_.load()) {
            playing_.store(true);
            smoothed_bpm_ = manual_bpm_.load();
            clock_.update_tempo_bpm(manual_bpm_.load());
            clock_.start();
        }
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
    if (cb_.on_beat_raw) cb_.on_beat_raw(buf, len);
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

    // A master beat means it's playing again — drop any manual-tempo latch so
    // we re-sync to the deck. (Not in ignore-players mode: there we never sync.)
    if (!ignore_master_.load()) manual_active_.store(false);

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
            bar_slip_count_ = 0;
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
        if (parsed->beat_in_bar != expected_next) {
            // Only a *sustained* disagreement is a real slip; a one-off is
            // almost always a dropped beat packet and must not realign.
            if (++bar_slip_count_ >= BAR_SLIP_REALIGN_THRESHOLD &&
                !bar_slip_pending_realign_) {
                bar_slip_pending_realign_ = true;
                if (cfg_.verbose) {
                    char msg[96];
                    std::snprintf(msg, sizeof(msg),
                                  "bar slip x%u (expected %u, got %u) — realign on next downbeat",
                                  bar_slip_count_, expected_next, parsed->beat_in_bar);
                    log(msg);
                }
            }
        } else {
            bar_slip_count_ = 0;  // clean sequence — clear the slip confidence
        }
    }
    expected_beat_in_bar_ = parsed->beat_in_bar;

    // A downbeat realign fires for either an auto-detected bar slip or a manual
    // deferred re-sync request (front-panel tap). Both snap the slave's bar 1
    // onto the master's bar 1 with a Stop+Start.
    const bool resync_deferred = (resync_request_.load() == 1);
    if ((bar_slip_pending_realign_ || resync_deferred) && parsed->beat_in_bar == 1) {
        clock_.stop();
        smoothed_bpm_ = parsed->effective_bpm();
        last_known_bpm_.store(smoothed_bpm_);
        clock_.update_tempo_bpm(smoothed_bpm_);
        clock_.start();
        bar_slip_pending_realign_ = false;
        bar_slip_count_ = 0;
        resync_request_.store(0);
        if (cfg_.verbose) log(resync_deferred ? "resync realign on downbeat (Stop+Start)"
                                              : "realigned on downbeat (Stop+Start)");
        return;  // skip the soft phase correction; we just hard-reset
    }

    clock_.feed_beat(parsed->ms_next_beat, parsed->beat_in_bar);
}

void Bridge::handle_status_packet(const uint8_t* buf, size_t len) {
    auto parsed = parse_status_packet(buf, len);
    if (!parsed) return;
    if (cb_.on_status_raw) cb_.on_status_raw(buf, len);
    // Track the highest master-generation counter any peer reports, so a master
    // takeover can announce Syncn = max+1 (required for CDJs to yield master).
    if (parsed->device_num != cfg_.device_num &&
        parsed->syncn > max_syncn_seen_.load()) {
        max_syncn_seen_.store(parsed->syncn);
    }
    // Never process our own broadcast status (we may receive our own broadcast):
    // it must not make us track ourselves as master or as the handoff target.
    if (parsed->device_num == cfg_.device_num) return;
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
    master_ip_.store(last_status_src_ip_);  // remember the master's IP for a handoff request

    // Handoff acknowledgement: the current master sets its Mh to our device
    // number when it yields to us. Only then may we assert mm=1.
    if (master_mode_.load() && !master_confirmed_.load() &&
        parsed->master_handoff == cfg_.device_num) {
        master_confirmed_.store(true);
        max_syncn_seen_.store(parsed->syncn);   // our Syncn will be this + 1
        if (cfg_.verbose) log("master yielded to us — asserting mm=1");
    }

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
        if (!last_master_playing_ && playing_.load() &&
            !free_run_.load() && !ignore_master_.load()) {
            // Playing → stopped: kill the clock immediately. (Skipped in
            // free-run / ignore: the clock keeps going on its latched tempo.)
            playing_.store(false);
            waiting_for_downbeat_ = false;
            expected_beat_in_bar_ = 0;
            bar_slip_pending_realign_ = false;
            bar_slip_count_ = 0;
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
    if (free_run_.load() || ignore_master_.load()) return;  // keep clocking standalone
    if (t - last_packet_ms_ < cfg_.silence_timeout_ms) return;
    playing_.store(false);
    waiting_for_downbeat_ = false;
    last_master_playing_ = false;
    pending_play_state_  = false;
    expected_beat_in_bar_ = 0;
    bar_slip_pending_realign_ = false;
    bar_slip_count_ = 0;
    smoothed_bpm_ = 0.0f;   // reseed on next start
    clock_.stop();
    if (cfg_.verbose) log("stop (silence timeout)");
}

void Bridge::maybe_resync(uint64_t t) {
    const uint8_t req = resync_request_.load();
    if (req == 0) return;
    if (!playing_.load()) {          // nothing running to realign
        resync_request_.store(0);
        return;
    }

    // Deferred (==1) requests are handled on the master's next downbeat in
    // handle_beat_packet — but only while master beats are actually arriving.
    // If they've gone stale (standalone, or the master paused), fall through
    // and restart now so the tap never feels dead.
    const bool beats_live =
        (last_packet_ms_ != 0) && (t - last_packet_ms_ < 750);
    if (req == 1 && beats_live && !ignore_master_.load()) return;

    // Immediate (==2), or a deferred request with no live master beats.
    clock_.stop();
    float bpm = last_known_bpm_.load();
    if (bpm > 0.0f) clock_.update_tempo_bpm(bpm);
    clock_.start();
    resync_request_.store(0);
    if (cfg_.verbose) log("resync (immediate Stop+Start)");
}

void Bridge::log(const char* msg) const {
    if (cb_.on_log) {
        cb_.on_log(msg);
    } else {
        std::fprintf(stderr, "[bridge] %s\n", msg);
    }
}

}  // namespace prolink
