#include "timer_esp.hpp"

#include <cstdio>

#include "esp_timer.h"

namespace firmware {

TimerEsp::~TimerEsp() {
    stop();
    if (handle_) {
        esp_timer_delete(handle_);
        handle_ = nullptr;
    }
}

void TimerEsp::timer_cb(void* arg) {
    auto* self = static_cast<TimerEsp*>(arg);
    if (!self->running_.load() || !self->on_tick_) return;

    uint32_t next_us = self->on_tick_();
    if (next_us == 0) {
        self->running_.store(false);
        return;
    }

    // Anchor the next fire to a cumulative deadline so dispatch + callback
    // latency doesn't accumulate as drift (which the PLL would then fight,
    // showing up as audible tempo jitter). If we've fallen behind by more
    // than a tick, resync to "now" rather than bunching ticks to catch up.
    self->next_deadline_us_ += next_us;
    const int64_t now   = esp_timer_get_time();
    int64_t       delay = self->next_deadline_us_ - now;
    if (delay < 1) {
        delay = 1;
        self->next_deadline_us_ = now + 1;
    }
    esp_timer_start_once(self->handle_, static_cast<uint64_t>(delay));
}

void TimerEsp::start(std::function<uint32_t()> on_tick) {
    on_tick_ = std::move(on_tick);

    if (!handle_) {
        esp_timer_create_args_t args = {};
        args.callback        = &TimerEsp::timer_cb;
        args.arg             = this;
        // Dispatch the callback from the esp_timer task, not an ISR — the
        // Clock's on_tick path calls IMidiOut::send_byte and printf, which
        // aren't ISR-safe.
        args.dispatch_method = ESP_TIMER_TASK;
        args.name            = "prolink-clock";
        esp_err_t err = esp_timer_create(&args, &handle_);
        if (err != ESP_OK) {
            printf("[timer] esp_timer_create failed: 0x%x\n", err);
            return;
        }
    }

    running_.store(true);
    // Anchor the schedule to now; the first tick fires effectively
    // immediately and the callback's return value sets every subsequent
    // interval (relative to this cumulative deadline).
    next_deadline_us_ = esp_timer_get_time();
    esp_timer_start_once(handle_, 1);
}

uint64_t TimerEsp::now_us() const {
    return static_cast<uint64_t>(esp_timer_get_time());
}

void TimerEsp::stop() {
    if (running_.exchange(false)) {
        if (handle_) {
            esp_timer_stop(handle_);
        }
    }
}

}  // namespace firmware
