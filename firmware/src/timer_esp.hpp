#pragma once

// Firmware ITimer backed by ESP-IDF's esp_timer (microsecond-resolution
// hardware timer). The desktop equivalent is desktop/timer_posix.hpp,
// which uses std::thread + sleep_until — same callback contract, different
// platform plumbing.
//
// The callback returns the desired interval to the *next* tick in µs.
// Returning 0 stops the timer cleanly. esp_timer's one-shot mode is
// re-armed at the end of each callback with the returned interval, so
// the tick cadence is whatever the Clock decides on a per-tick basis.

#include "clock.hpp"

#include <atomic>
#include <functional>

// Forward decl so the header doesn't pull esp_timer.h into everything.
struct esp_timer;
using esp_timer_handle_t = struct esp_timer*;

namespace firmware {

class TimerEsp : public prolink::ITimer {
public:
    TimerEsp() = default;
    ~TimerEsp() override;
    TimerEsp(const TimerEsp&) = delete;
    TimerEsp& operator=(const TimerEsp&) = delete;

    void start(std::function<uint32_t()> on_tick) override;
    void stop() override;

private:
    static void timer_cb(void* arg);

    esp_timer_handle_t handle_ = nullptr;
    std::function<uint32_t()> on_tick_;
    std::atomic<bool> running_{false};
};

}  // namespace firmware
