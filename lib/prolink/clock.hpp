#pragma once

#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

namespace prolink {

// Implemented by platform layers. The callback returns the desired interval
// (microseconds) until the next call. Return 0 to stop the timer cleanly.
class ITimer {
public:
    virtual void start(std::function<uint32_t()> on_tick) = 0;
    virtual void stop() = 0;
    // Monotonic microsecond clock in the timer's own timebase. Used by Clock
    // to timestamp tick emission and beat-packet arrivals for feed-forward
    // phase locking, so it must be consistent with start()'s scheduling.
    virtual uint64_t now_us() const = 0;
    virtual ~ITimer() = default;
};

class IMidiOut {
public:
    virtual void send_byte(uint8_t b) = 0;
    virtual ~IMidiOut() = default;
};

// The Bridge talks to Clock through this interface so the firmware can
// implement it as a uClock adapter without changing bridge.cpp.
class IClockSink {
public:
    virtual void update_tempo_bpm(float bpm) = 0;
    virtual void correct_phase(uint8_t beat_in_bar) = 0;
    // Feed-forward phase lock from a Pro DJ Link beat packet. ms_to_next_beat
    // is the master's predicted time until its next beat; beat_in_bar is 1..4.
    // Continuous-time, so it supersedes correct_phase() (which resolves phase
    // only to the nearest whole tick, ~20 ms at 120 BPM).
    virtual void feed_beat(uint32_t ms_to_next_beat, uint8_t beat_in_bar) = 0;
    virtual void start() = 0;
    virtual void stop()  = 0;

    // Lead-time compensation. Positive offset means tick 0 of every beat
    // fires `offset_us` µs *before* the master's beat boundary, to absorb
    // physical chain latency (USB transit + slave processing). Changes
    // take effect within a few ticks.
    virtual void set_offset_us(int32_t offset_us) = 0;
    virtual int32_t get_offset_us() const = 0;

    virtual ~IClockSink() = default;
};

// 24 PPQN MIDI clock driver. Tempo and phase are tracked separately:
//   - update_tempo_bpm() sets the steady tick interval, applied on the
//     very next tick — no smoothing, the ~200 ms status-packet cadence
//     is already smooth enough.
//   - correct_phase() records the signed phase error in microseconds.
//     The error is bled off across many ticks (default gain divisor = 16,
//     i.e. ~6% of the residual error applied each tick), so beat-packet
//     network jitter (~±2 ms) is averaged out instead of being passed
//     straight through to the tick stream.
class Clock : public IClockSink {
public:
    Clock(IMidiOut& midi, ITimer& timer, uint32_t gain_divisor = 16);

    void update_tempo_bpm(float bpm) override;
    void correct_phase(uint8_t beat_in_bar) override;
    void feed_beat(uint32_t ms_to_next_beat, uint8_t beat_in_bar) override;
    void start() override;
    void stop()  override;
    void set_offset_us(int32_t offset_us) override;
    int32_t get_offset_us() const override { return offset_us_.load(); }

    // Optional hook fired once per beat, at the tick-0 boundary (after the MIDI
    // clock byte for that tick). Used by tempo-master mode to emit a Pro DJ Link
    // beat packet aligned to our own grid. Runs in the timer callback context.
    void set_on_beat(std::function<void()> on_beat) { on_beat_ = std::move(on_beat); }

    // Status readouts (lock-free).
    bool     is_running()             const { return running_.load(); }
    float    current_bpm()            const { return current_bpm_.load(); }
    uint8_t  current_beat_in_bar()    const { return beat_in_bar_.load(); }
    uint8_t  current_tick_in_beat()   const { return tick_in_beat_.load(); }
    int32_t  current_phase_error_us() const { return phase_error_us_.load(); }
    uint32_t current_tick_period_us() const { return tick_period_us_.load(); }
    uint64_t ticks_emitted_total()    const { return ticks_emitted_.load(); }

private:
    uint32_t on_tick_();  // wrapped into the ITimer callback

    IMidiOut& midi_;
    ITimer&   timer_;
    uint32_t  gain_;

    std::atomic<uint32_t> tick_period_us_;
    std::atomic<int32_t>  phase_error_us_;
    std::atomic<int32_t>  offset_us_;        // lead-time compensation, signed
    std::atomic<int64_t>  last_beat_anchor_us_;  // abs time (timer µs) of our last tick-0
    std::atomic<int64_t>  last_feed_us_;      // abs time of the last master beat fed in
    std::atomic<uint8_t>  tick_in_beat_;     // index of *next* tick to emit
    std::atomic<uint8_t>  beat_in_bar_;
    std::atomic<float>    current_bpm_;
    std::atomic<bool>     running_;
    std::atomic<uint64_t> ticks_emitted_;    // monotonic counter for rate display
    std::function<void()> on_beat_;          // fired at each tick-0 boundary
};

}  // namespace prolink
