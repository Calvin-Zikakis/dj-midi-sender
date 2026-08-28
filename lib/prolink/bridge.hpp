#pragma once

#include "clock.hpp"
#include "packets.hpp"
#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

namespace prolink {

class IUdpSocket {
public:
    // Blocking recv with timeout in milliseconds. Returns bytes read, 0 on
    // timeout, or -1 on socket error. If `src_ip` is non-null, the sender's
    // IPv4 address (host byte order) is written to it — used to unicast the
    // tempo-master handoff request back to the current master.
    virtual int recv(uint8_t* buf, size_t len, uint32_t timeout_ms,
                     uint32_t* src_ip = nullptr) = 0;

    // Send `buf` to (ip, port). Returns true on success. ip is the IPv4
    // address as a host-order uint32_t (e.g. 192.168.1.1 == 0xC0A80101 on
    // any host). Pass 0xFFFFFFFF to broadcast.
    virtual bool send(const uint8_t* buf, size_t len,
                      uint32_t ip, uint16_t port) = 0;
    virtual ~IUdpSocket() = default;
};

struct BridgeConfig {
    uint8_t  device_num         = 5;            // virtual CDJ # — matches python-prodj-link's working default
    char     device_name[21]    = "xdj-bridge";
    uint8_t  mac[6]             = {0};
    uint32_t local_ip        = 0;            // own IP, network byte order
    uint32_t broadcast_ip    = 0xFFFFFFFFu;  // network broadcast (e.g. 169.254.255.255)
    bool     send_vcdj_announce = true;
    bool     verbose            = false;
    uint32_t silence_timeout_ms = 2000;
    uint32_t keepalive_period_ms = 1500;
    uint32_t play_debounce_ms   = 100;
    float    fallback_bpm       = 120.0f;

    // If non-zero (1..6), pin master tracking to this device number and
    // ignore the is_master flag. Useful when you want the drum machine to
    // follow a specific deck regardless of who has the master button lit.
    // 0 = auto (track whichever device has is_master, bootstrap from first
    // packet otherwise).
    uint8_t  force_master_device = 0;

    // Device number claimed only while acting as tempo master. Only player
    // slots (1..4) can hold the master role — mixer slots get the handoff
    // offered but never completed. But a 4-channel unit like the XDJ-XZ treats
    // all four player slots as its own: sitting on one permanently breaks its
    // deck-to-deck master handoff. So the box lives on `device_num` (a harmless
    // out-of-the-way number) and only claims this player slot for as long as it
    // is actually master, re-running the claim handshake on each switch.
    uint8_t  master_device_num = 4;

    // EMA smoothing coefficient applied to incoming BPM values before
    // pushing to the clock. 0 < alpha <= 1, where 1 = no smoothing (each
    // update applied directly), smaller = heavier smoothing. 0.3 means
    // ~6 status packets (~1.2 s) to reach 90% of a step change — smooth
    // enough to damp sample noise without lagging slider movements.
    float    bpm_smoothing_alpha = 0.3f;

    // Hysteresis window for falling back from status-driven tempo to
    // beat-packet-driven tempo. With status packets arriving at ~5 Hz,
    // 500 ms means roughly two consecutive misses before the bridge
    // accepts a beat-packet tempo update. Status packets are the lower-
    // jitter source — we only want one driver at a time.
    uint32_t status_silence_fallback_ms = 500;
};

// Optional hooks — useful for visualizers, logging, tests.
struct BridgeCallbacks {
    std::function<void(const BeatPacket&)>   on_beat;
    std::function<void(const StatusPacket&)> on_status;
    std::function<void(const char*)>         on_log;
    // Raw-bytes hooks for protocol reverse-engineering / diagnostics. Called
    // for EVERY parsed packet of that type, before master filtering, with the
    // raw wire bytes. Null by default (zero cost).
    std::function<void(const uint8_t*, size_t)> on_status_raw;
    std::function<void(const uint8_t*, size_t)> on_beat_raw;
    // Fired for EVERY received datagram (port, bytes) before parsing — catches
    // unknown packet types (e.g. sync/master-request commands) that the beat/
    // status parsers drop. Null by default.
    std::function<void(uint16_t, const uint8_t*, size_t)> on_raw_datagram;
};

// Orchestrates the dual-source clock pipeline. Owns master tracking,
// play-state debouncing, hold-Start-until-downbeat, keep-alive emission,
// and silence detection. Pure logic — all I/O is injected.
class Bridge {
public:
    Bridge(IUdpSocket& beat_sock,
           IUdpSocket& status_sock,
           IUdpSocket& keepalive_sock,
           IClockSink& clock,
           BridgeConfig cfg,
           BridgeCallbacks cb = {});

    // Blocking — spawns listener threads internally on platforms that need it;
    // returns when stop() is called.
    void run();

    // Asynchronously requests the run loop to exit.
    void stop();

    // Two-axis lead-time compensation. The scheduler sees only the sum.
    //
    //   clock_offset_ms — physical chain latency (USB → slave). Per output
    //     port; the desktop wrapper persists it to ~/.config/dj-midi-sender.json.
    //   grid_offset_ms  — per-track beat-grid skew (rekordbox's analyzed grid
    //     vs where the kick actually sits). Session-only.
    void  set_clock_offset_ms(float ms);
    void  set_grid_offset_ms(float ms);
    void  adjust_clock_offset_ms(float delta_ms);
    void  adjust_grid_offset_ms(float delta_ms);
    void  reset_grid_offset();
    float clock_offset_ms() const { return clock_offset_ms_.load(); }
    float grid_offset_ms()  const { return grid_offset_ms_.load(); }
    float total_offset_ms() const { return clock_offset_ms_.load() + grid_offset_ms_.load(); }

    // Runtime clock-source selection (front-panel encoder). 0 = auto-track
    // whoever holds the master flag; 1..6 = pin to that device number.
    // Mirrors cfg.force_master_device but settable live.
    void    set_force_master_device(uint8_t device_num);
    uint8_t force_master_device() const { return cfg_.force_master_device; }

    // Free-run mode. When enabled, the bridge does NOT stop the clock when the
    // master pauses or the link goes silent — it latches the last tempo and
    // keeps clocking ("full hardware mode"). It only suppresses stops; the
    // clock must already have been started by a real master playing.
    void set_free_run(bool on) { free_run_.store(on); if (!on) manual_active_.store(false); }
    bool free_run() const { return free_run_.load(); }

    // Manual tempo (front-panel: hold-tap + spin in Free mode). Adjusts the
    // running clock by `delta` BPM and latches it so packet tempo stops
    // overriding — until a master beat arrives (it's playing again → re-sync).
    // Only meaningful while the clock is already running (free-run latched).
    void nudge_manual_bpm(float delta);
    bool manual_tempo_active() const { return manual_active_.load(); }

    // Ignore-players / standalone mode ("Off" source). The box clocks on its
    // own manual tempo and ignores every deck — no sync, no auto-stop, and a
    // master beat no longer pulls it back. The clock cold-starts when enabled.
    void set_ignore_master(bool on);
    bool ignore_master() const { return ignore_master_.load(); }

    // Set the manual tempo absolutely (classic tap-tempo). nudge_manual_bpm()
    // is the relative version (spin fine-tune).
    void set_manual_bpm(float bpm);

    // Tempo-master mode (experimental). When enabled, the box broadcasts Pro
    // DJ Link beat packets (port 50001, aligned to our clock via on_master_beat)
    // and status packets with the master flag (port 50002, ~5 Hz), so CDJs in
    // sync mode can follow the box's tempo/grid. Implies standalone (we ignore
    // other decks and run our own tempo). Requires the beat/status sockets to
    // have broadcast enabled by the platform layer.
    void set_master_mode(bool on);
    bool master_mode() const { return master_mode_.load(); }
    // True only once the handoff completed and we actually hold the role;
    // master_mode() is true from the moment it is requested.
    bool is_tempo_master() const { return master_confirmed_.load(); }
    // Our own bar position (1..4) while acting as master. The Clock's
    // beat_in_bar only tracks incoming deck beats, which we deliberately ignore
    // when we are the authority — so the UI reads this instead.
    uint8_t master_beat_in_bar() const { return master_beat_pos_.load(); }
    // Called once per beat from the clock (Clock::set_on_beat) to emit a beat
    // packet advertising our grid. No-op unless master mode is on.
    void on_master_beat();

    // Manual beat re-sync (front-panel tap). MIDI clock carries tempo but not
    // bar position, so if a slave's transport is stopped/started locally it
    // keeps the tempo but loses bar alignment with the master. This re-emits
    // MIDI Stop+Start to snap the slave back to bar 1.
    //   immediate == false (sync): defer the Stop+Start to the master's next
    //     downbeat, so the slave's bar 1 realigns with the master's bar 1.
    //   immediate == true (free/standalone): restart now (there's no master
    //     downbeat to wait for).
    // No-op unless the clock is already running.
    void request_resync(bool immediate = false);
    bool resync_pending() const { return resync_request_.load() != 0; }

    // Per-packet counts since startup.
    uint64_t beat_packet_count()   const { return beat_count_.load(); }
    uint64_t status_packet_count() const { return status_count_.load(); }

    // Diagnostics.
    bool     is_playing()         const { return playing_.load(); }
    uint8_t  current_master_num() const { return current_master_.load(); }
    float    last_known_bpm()     const { return last_known_bpm_.load(); }

private:
    void handle_beat_packet(const uint8_t* buf, size_t len);
    void handle_status_packet(const uint8_t* buf, size_t len);
    // Device-number claim handshake, stepped from the run loop at ~300 ms
    // intervals. Keep-alives are held off until it completes.
    void step_device_claim(uint64_t now_ms);
    void maybe_send_keepalive(uint64_t now_ms);
    void maybe_stop_on_silence(uint64_t now_ms);
    void maybe_resync(uint64_t now_ms);
    void maybe_broadcast_master_status(uint64_t now_ms);
    // A deck (device `requester`, at `requester_ip`) asked us to yield master.
    void handle_master_yield_request(uint8_t requester, uint32_t requester_ip);
    void log(const char* msg) const;

    IUdpSocket& beat_sock_;
    IUdpSocket& status_sock_;
    IUdpSocket& keepalive_sock_;
    IClockSink& clock_;
    BridgeConfig cfg_;
    BridgeCallbacks cb_;

    std::atomic<bool>     running_{false};
    std::atomic<bool>     playing_{false};
    std::atomic<bool>     free_run_{false};
    std::atomic<bool>     ignore_master_{false};
    std::atomic<bool>     manual_active_{false};
    std::atomic<float>    manual_bpm_{120.0f};
    // Manual re-sync request: 0 = none, 1 = deferred (on next master downbeat),
    // 2 = immediate. Set from the UI task, consumed on the bridge thread.
    std::atomic<uint8_t>  resync_request_{0};
    // Tempo-master mode broadcasting.
    std::atomic<bool>     master_mode_{false};
    uint8_t               master_beat_in_bar_ = 0;   // 0 = not started; 1..4
    std::atomic<uint8_t>  master_beat_pos_{0};       // same, readable by the UI
    uint64_t              last_master_status_ms_ = 0;
    std::atomic<uint32_t> max_syncn_seen_{0};        // highest Syncn from peers
    int                   master_request_countdown_ = 0;  // takeover requests left to send
    std::atomic<uint32_t> master_ip_{0};             // current master's source IP (host order)
    uint32_t              last_status_src_ip_ = 0;   // src IP of the status packet in flight
    // True once the current master has yielded to us (its Mh names our device);
    // only then do we assert mm=1. Until then we broadcast mm=0 and keep asking.
    std::atomic<bool>     master_confirmed_{false};
    // Reverse handoff: a deck asked us (the master) to yield. We advertise its
    // number in our Mh until it asserts master, then step down. 0 = not yielding.
    std::atomic<uint8_t>  yielding_to_{0};
    // Deadline for a UI-requested release to complete; if the appointed deck
    // never claims master we step down anyway. 0 = no release pending.
    uint64_t              release_deadline_ms_ = 0;
    std::atomic<uint8_t>  current_master_{0};
    std::atomic<float>    last_known_bpm_{120.0f};
    std::atomic<float>    clock_offset_ms_{0.0f};
    std::atomic<float>    grid_offset_ms_{0.0f};
    std::atomic<uint64_t> beat_count_{0};
    std::atomic<uint64_t> status_count_{0};

    void push_offset_to_clock_();
    void apply_tempo_(float bpm);

    // Held entirely on the main thread; no concurrent access.
    bool     waiting_for_downbeat_ = false;
    bool     last_master_playing_  = false;
    uint64_t pending_play_change_ms_ = 0;
    bool     pending_play_state_ = false;
    uint64_t last_packet_ms_ = 0;
    uint64_t last_status_ms_ = 0;       // last successfully parsed status packet
    uint64_t last_keepalive_ms_ = 0;
    // Device-number claim progress: 12 steps (hello x3, then claim stages
    // 1/2/3 x3 each) at ~300 ms, then done. Keep-alives wait for it.
    uint8_t  claim_step_ = 0;
    uint64_t last_claim_ms_ = 0;
    bool     claim_done_ = false;
    uint8_t  idle_device_num_ = 0;   // number to return to when not master
    uint8_t  claim_repeats_ = 3;     // packets per stage (3 at boot, 1 when fast)
    uint16_t claim_interval_ms_ = 300;
    // Re-run the claim handshake for a new device number. `fast` sends one
    // packet per stage at a tighter interval (~0.6 s instead of ~3.6 s) — used
    // for mid-set master switches, where a multi-second stall is unusable.
    void restart_device_claim(uint8_t device_num, bool fast = true);
    float    smoothed_bpm_ = 0.0f;      // EMA-filtered tempo (0 = bootstrap)

    // Bar-alignment tracking. If a beat packet is dropped, the master's
    // beat_in_bar advances out of step with our internal expectation; we
    // catch that and force a Stop+Start on the next downbeat to re-align
    // the slave's bar 1 with the master's.
    uint8_t  expected_beat_in_bar_ = 0;   // 0 = "not yet observed"
    bool     bar_slip_pending_realign_ = false;
    uint8_t  bar_slip_count_ = 0;         // consecutive unexpected beat numbers
};

}  // namespace prolink
