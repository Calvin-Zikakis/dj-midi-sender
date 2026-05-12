#pragma once

#include "clock.hpp"

#include <atomic>
#include <functional>
#include <thread>

namespace desktop {

// ITimer backed by a std::thread that sleep_until's the next deadline.
// The callback returns the desired interval to the next tick (µs). Returning
// 0 stops the timer cleanly.
//
// macOS sleep_until has ~100 µs resolution; the PLL absorbs the residual.
class TimerPosix : public prolink::ITimer {
public:
    TimerPosix() = default;
    ~TimerPosix() override;
    TimerPosix(const TimerPosix&) = delete;
    TimerPosix& operator=(const TimerPosix&) = delete;

    void start(std::function<uint32_t()> on_tick) override;
    void stop() override;

private:
    std::atomic<bool>           running_{false};
    std::thread                 thread_;
    std::function<uint32_t()>   on_tick_;
};

}  // namespace desktop
