#pragma once

#include "bridge.hpp"
#include "clock.hpp"
#include "packets.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace desktop {

// Owns the GLFW window + ImGui context. Run from the main thread (GLFW
// requires this on macOS). The Bridge + Clock run on worker threads and
// publish state into here via on_beat / on_status (called from the network
// thread). The Clock is sampled directly each frame.
//
// All side effects from user input go back through std::function callbacks
// so the GUI doesn't depend on Bridge concrete types beyond reading state.
class GuiVisualizer {
public:
    struct OffsetActions {
        std::function<void(float)> adjust_clock_ms;   // delta
        std::function<void(float)> adjust_grid_ms;    // delta
        std::function<void()>      reset_grid;
        std::function<float()>     get_clock_ms;
        std::function<float()>     get_grid_ms;
    };

    GuiVisualizer(const prolink::Bridge& bridge,
                  const prolink::Clock&  clock,
                  OffsetActions          actions,
                  std::string            midi_port_name);
    ~GuiVisualizer();

    // Returns true if the window opened. Call run() if true; otherwise the
    // caller can fall back to headless mode.
    bool init(int width = 700, int height = 720);

    // Blocks on the GLFW event loop until the window is closed or
    // request_stop() is called. Must run on the thread that called init().
    void run();

    // Thread-safe: can be called from any thread to ask the window to close.
    void request_stop();

    // Called from the network thread on each packet — keeps a snapshot.
    void on_beat(const prolink::BeatPacket& pkt);
    void on_status(const prolink::StatusPacket& pkt);

private:
    void render_frame();
    void handle_keys();

    const prolink::Bridge& bridge_;
    const prolink::Clock&  clock_;
    OffsetActions          actions_;
    std::string            midi_port_;

    // GLFW window opaque pointer; void* to avoid leaking GLFW into the header.
    void* window_ = nullptr;

    // --- Snapshots updated from the network thread ---
    std::mutex          snap_mu_;
    prolink::BeatPacket   last_beat_{};
    prolink::StatusPacket last_status_{};
    bool                  have_beat_   = false;
    bool                  have_status_ = false;
    std::chrono::steady_clock::time_point last_beat_at_{};

    // Tick-rate windowing: sample clock_.ticks_emitted_total() once per frame
    // and divide by elapsed wall time.
    uint64_t                                 last_tick_count_ = 0;
    std::chrono::steady_clock::time_point    last_tick_sample_at_{};
    float                                    tick_rate_hz_ = 0.0f;

    // 0..1 beat-flash intensity, decays over BEAT_FLASH_MS.
    float beat_flash_ = 0.0f;
    // Cached last-emitted tick counter so we can show a tick-pulse.
    uint64_t prev_tick_total_ = 0;
    float    tick_flash_ = 0.0f;
};

}  // namespace desktop
