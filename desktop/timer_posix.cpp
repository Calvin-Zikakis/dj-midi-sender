#include "timer_posix.hpp"

#include <chrono>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>

namespace {

// Conversion factor: nanoseconds → Mach absolute time units.
// Computed once at startup. On Apple Silicon, numer==denom==1 (1 unit = 1 ns).
// On Intel Macs the ratio is typically 1000000000/1000000000 (same) or
// occasionally 1000000000/N.
struct MachClock {
    mach_timebase_info_data_t tb{};
    MachClock() { mach_timebase_info(&tb); }
    uint64_t us_to_mach(uint64_t us) const {
        // us → ns: multiply by 1000
        // ns → mach units: multiply by denom / numer
        return us * 1000ull * tb.denom / tb.numer;
    }
} g_mach;

// Promote the calling thread to a real-time scheduling class so macOS won't
// preempt it for multi-millisecond stretches. Period/computation/constraint
// are calibrated for a ~20 ms tick interval (24 PPQN @ ~120 BPM).
void set_realtime_priority() {
    const double period_s   = 0.020;   // 20 ms nominal tick interval
    const double compute_s  = 0.002;   // 2 ms worst-case in-callback work
    const double constraint_s = 0.004; // 4 ms hard deadline

    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    auto ns_to_mach = [&](double ns) -> uint32_t {
        return static_cast<uint32_t>(ns * tb.denom / tb.numer);
    };

    thread_time_constraint_policy policy{};
    policy.period      = ns_to_mach(period_s     * 1e9);
    policy.computation = ns_to_mach(compute_s    * 1e9);
    policy.constraint  = ns_to_mach(constraint_s * 1e9);
    policy.preemptible = 1;
    thread_policy_set(mach_thread_self(),
                      THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

}  // namespace
#endif  // __APPLE__

namespace desktop {

TimerPosix::~TimerPosix() {
    stop();
}

void TimerPosix::start(std::function<uint32_t()> on_tick) {
    if (running_.exchange(true)) return;
    on_tick_ = std::move(on_tick);
    thread_ = std::thread([this]() {
#ifdef __APPLE__
        set_realtime_priority();
        uint64_t next_mach = mach_absolute_time();
        while (running_.load()) {
            uint32_t next_us = on_tick_ ? on_tick_() : 0;
            if (next_us == 0) { running_.store(false); break; }

            next_mach += g_mach.us_to_mach(next_us);

            // mach_wait_until can return early; spin-check for the last ~50 µs
            // to eat the scheduler quantum boundary.
            uint64_t spin_threshold = g_mach.us_to_mach(50);
            mach_wait_until(next_mach > spin_threshold
                                ? next_mach - spin_threshold
                                : next_mach);
            while (mach_absolute_time() < next_mach) { /* busy-wait ~50 µs */ }

            // Snap forward if we drifted more than one beat behind (debugger,
            // sleep, etc.) to avoid a tick burst.
            uint64_t now_mach = mach_absolute_time();
            uint64_t beat_mach = g_mach.us_to_mach(100'000);  // 100 ms
            if (now_mach > next_mach + beat_mach) {
                next_mach = now_mach;
            }
        }
#else
        using namespace std::chrono;
        auto next = steady_clock::now();
        while (running_.load()) {
            uint32_t next_us = on_tick_ ? on_tick_() : 0;
            if (next_us == 0) { running_.store(false); break; }
            next += microseconds(next_us);
            std::this_thread::sleep_until(next);
            auto now = steady_clock::now();
            if (now > next + milliseconds(100)) next = now;
        }
#endif
    });
}

void TimerPosix::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (thread_.joinable()) thread_.join();
}

}  // namespace desktop
