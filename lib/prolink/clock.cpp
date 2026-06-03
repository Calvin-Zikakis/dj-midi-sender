#include "clock.hpp"

#include <algorithm>
#include <cmath>

namespace prolink {

namespace {
// Saner bounds for an out-of-range tempo update; corresponds to ~20–300 BPM.
constexpr uint32_t MIN_TICK_PERIOD_US = 60'000'000u / 300u / TICKS_PER_BEAT;
constexpr uint32_t MAX_TICK_PERIOD_US = 60'000'000u / 20u  / TICKS_PER_BEAT;

uint32_t bpm_to_tick_period_us(float bpm) {
    if (!std::isfinite(bpm) || bpm <= 0.0f) return 0;
    double period = 60'000'000.0 / static_cast<double>(bpm) / TICKS_PER_BEAT;
    if (period < MIN_TICK_PERIOD_US) period = MIN_TICK_PERIOD_US;
    if (period > MAX_TICK_PERIOD_US) period = MAX_TICK_PERIOD_US;
    return static_cast<uint32_t>(period);
}
}  // namespace

Clock::Clock(IMidiOut& midi, ITimer& timer, uint32_t gain_divisor)
    : midi_(midi)
    , timer_(timer)
    , gain_(gain_divisor == 0 ? 1 : gain_divisor)
    , tick_period_us_(bpm_to_tick_period_us(120.0f))
    , phase_error_us_(0)
    , offset_us_(0)
    , last_beat_anchor_us_(0)
    , tick_in_beat_(0)
    , beat_in_bar_(0)
    , current_bpm_(120.0f)
    , running_(false)
    , ticks_emitted_(0) {}

void Clock::update_tempo_bpm(float bpm) {
    uint32_t period = bpm_to_tick_period_us(bpm);
    if (period == 0) return;  // implausible — ignore
    tick_period_us_.store(period);
    current_bpm_.store(bpm);
}

void Clock::correct_phase(uint8_t beat_in_bar) {
    beat_in_bar_.store(beat_in_bar);

    // Where should the next tick fire? Ideally tick 0 happens `offset_us`
    // before now (positive offset == lead-time compensation). If
    // `tick_in_beat` is `t`, we are about to emit tick `t` and want it
    // at `now - offset + t * period`. The signed phase error wrapped to
    // the shortest direction around the 24-tick circle:
    uint32_t period    = tick_period_us_.load();
    uint8_t  t         = tick_in_beat_.load();
    int32_t  off       = offset_us_.load();
    int32_t  beat_span = static_cast<int32_t>(period) * TICKS_PER_BEAT;
    // Positive err → next tick fires later (slow down); negative → fires sooner (speed up).
    // Beat arrived; tick_in_beat_ t means we've emitted t ticks past the beat boundary →
    // we're t * period μs ahead → need to slow down → positive err.
    // Steady state: t * period == off (tick 0 leads the beat by offset_us).
    int32_t  err       = static_cast<int32_t>(t) * static_cast<int32_t>(period) - off;
    if (err < -beat_span / 2) err += beat_span;
    if (err >  beat_span / 2) err -= beat_span;
    phase_error_us_.store(err);
}

void Clock::feed_beat(uint32_t ms_to_next_beat, uint8_t beat_in_bar) {
    // ms_to_next_beat is reserved for a future master-beat predictor (to
    // reject UDP arrival jitter); on a wired link that jitter (~1–2 ms) is
    // dwarfed by what we fix here, so we don't steer on it yet.
    (void)ms_to_next_beat;
    beat_in_bar_.store(beat_in_bar);
    if (!running_.load()) return;

    const uint32_t period    = tick_period_us_.load();
    const int64_t  beat_span = static_cast<int64_t>(period) * TICKS_PER_BEAT;
    if (beat_span <= 0) return;

    // Same semantics as correct_phase() — drive our tick-0 lead over the
    // master beat toward `offset` — but measured in continuous microseconds
    // (time since our last tick-0) instead of the integer tick index, which
    // resolved phase only to the nearest ~20 ms tick. `offset` stays a
    // constant time lead, so it does NOT change with tempo.
    //   lead = now − our last tick-0  ≈ how far our beat boundary preceded
    //          this master beat (the packet arrives on the beat).
    //   err > 0 → leading too much → slow down (longer next interval).
    const int64_t now_us = static_cast<int64_t>(timer_.now_us());
    int64_t lead = now_us - last_beat_anchor_us_.load();
    int64_t err  = lead - static_cast<int64_t>(offset_us_.load());
    while (err >  beat_span / 2) err -= beat_span;
    while (err < -beat_span / 2) err += beat_span;
    phase_error_us_.store(static_cast<int32_t>(err));
}

void Clock::set_offset_us(int32_t new_offset) {
    int32_t old = offset_us_.exchange(new_offset);
    int32_t delta = new_offset - old;
    if (delta == 0) return;

    // Inject the delta into phase_error scaled by gain, so the *next* tick
    // applies most of the nudge in one shot — important for live calibration
    // where the operator wants to hear the change immediately, not over
    // ~200 ms. The residual is small and bleeds off the next few ticks.
    // Sign: positive offset means lead → tick stream fires sooner → negative
    // err contribution.
    phase_error_us_.fetch_sub(delta * static_cast<int32_t>(gain_));
}

void Clock::start() {
    if (running_.exchange(true)) return;
    tick_in_beat_.store(0);
    phase_error_us_.store(0);
    last_beat_anchor_us_.store(static_cast<int64_t>(timer_.now_us()));
    midi_.send_byte(MIDI_START);
    timer_.start([this]() { return this->on_tick_(); });
}

void Clock::stop() {
    if (!running_.exchange(false)) return;
    timer_.stop();
    midi_.send_byte(MIDI_STOP);
}

uint32_t Clock::on_tick_() {
    if (!running_.load()) return 0;

    // Emitting tick 0 is our beat boundary — timestamp it so feed_beat() has a
    // continuous-time reference for phase locking.
    if (tick_in_beat_.load() == 0) {
        last_beat_anchor_us_.store(static_cast<int64_t>(timer_.now_us()));
    }

    midi_.send_byte(MIDI_CLOCK);
    ticks_emitted_.fetch_add(1, std::memory_order_relaxed);

    // Bleed off a fraction of the phase error into the next interval.
    int32_t err        = phase_error_us_.load();
    int32_t correction = err / static_cast<int32_t>(gain_);
    int32_t period     = static_cast<int32_t>(tick_period_us_.load());
    int32_t next_us    = period + correction;
    phase_error_us_.store(err - correction);

    // Advance position.
    uint8_t t = tick_in_beat_.load();
    t = static_cast<uint8_t>((t + 1) % TICKS_PER_BEAT);
    tick_in_beat_.store(t);

    // Sanity floor so a corrupt phase error can't ask for a 0 µs interval.
    if (next_us < static_cast<int32_t>(MIN_TICK_PERIOD_US / 2)) {
        next_us = static_cast<int32_t>(MIN_TICK_PERIOD_US / 2);
    }
    return static_cast<uint32_t>(next_us);
}

}  // namespace prolink
