"""Deadline-based 24-PPQN MIDI clock scheduler.

Two-step model — tempo and phase are tracked separately:

- **Tempo** is taken directly from the master's `ms_next_beat` prediction
  (`tick_period = ms_next_beat / 24`). New tempo applies on the very next
  tick, so pitch-slider changes propagate within one beat with no transient.
- **Phase** is corrected via a fraction of the per-packet error applied to
  `_next_tick_time`. Per-packet network jitter (~±2 ms) is averaged across
  a few beats rather than passed straight through to the tick stream.

Crucially, the period is never computed by dividing a phase span — an
earlier design did `(target_boundary - next_tick) / ticks_remaining`,
which produced a runaway period when ticks_remaining was small (1–2),
exactly the case that occurs around tempo changes. Separating tempo and
phase eliminates that failure mode.
"""
from __future__ import annotations

import threading
import time
from typing import Callable, Optional

TICKS_PER_BEAT = 24
_MIN_PERIOD_S = 60.0 / 300 / TICKS_PER_BEAT  # 300 BPM ceiling
_MAX_PERIOD_S = 60.0 / 20 / TICKS_PER_BEAT   # 20 BPM floor

# Fraction of phase error applied per beat packet. 0.35 ≈ 90% lock within
# 5 beats; attenuates per-packet jitter by ~0.65× per beat.
_PHASE_CORRECTION_GAIN = 0.35


class MidiClockScheduler:
    def __init__(self, send_tick: Callable[[], None], offset_ms: float = 0.0):
        self._send_tick = send_tick
        self._lock = threading.Lock()
        self._tick_period_s = 60.0 / 120.0 / TICKS_PER_BEAT  # arbitrary default
        self._tick_in_beat = 0  # index of the next tick to fire, 0..23
        self._offset_s = offset_ms / 1000.0
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._next_tick_time = 0.0

    def beat_arrived(self, ms_next_beat: int) -> None:
        """Update tempo from the packet, gently re-align phase.

        Tempo comes from `ms_next_beat / 24` — applied immediately.
        Phase is the signed shortest distance between our `_next_tick_time`
        and where it *should* be if the master just announced a beat
        boundary at `now`. A fraction of that error is applied to
        `_next_tick_time`, so jitter averages out.
        """
        if ms_next_beat <= 0 or ms_next_beat > 4000:
            return  # implausible (< 15 BPM); ignore corrupt packets

        with self._lock:
            now = time.perf_counter()

            new_period = ms_next_beat / 1000.0 / TICKS_PER_BEAT
            new_period = max(_MIN_PERIOD_S, min(_MAX_PERIOD_S, new_period))
            self._tick_period_s = new_period

            # If the master just announced a beat boundary at `now`, our tick 0
            # of this beat should fire at (now - offset). The k-th tick fires
            # k periods later. So our `_next_tick_time` ideally equals:
            ideal_next_tick = (now - self._offset_s) + self._tick_in_beat * new_period

            # Phase error wrapped to the shortest direction around the 24-tick
            # circle (so we always converge the short way).
            beat_span = new_period * TICKS_PER_BEAT
            half = beat_span / 2.0
            error = ((ideal_next_tick - self._next_tick_time) + half) % beat_span - half

            self._next_tick_time += error * _PHASE_CORRECTION_GAIN

            # Safety: bound `_next_tick_time` to a reasonable window around now
            # so a pathological packet can't park us minutes in the past/future.
            if self._next_tick_time < now - beat_span:
                self._next_tick_time = now
            elif self._next_tick_time > now + 2.0 * beat_span:
                self._next_tick_time = now + new_period

    def set_offset_ms(self, offset_ms: float) -> None:
        """Set the lead offset (absolute, ms). Slides the tick stream
        immediately so the change is audible within the current beat.
        """
        with self._lock:
            new_s = offset_ms / 1000.0
            self._next_tick_time -= (new_s - self._offset_s)
            self._offset_s = new_s

    def get_offset_ms(self) -> float:
        with self._lock:
            return self._offset_s * 1000.0

    def start(self) -> None:
        if self._running:
            return
        with self._lock:
            self._tick_in_beat = 0
        self._running = True
        self._next_tick_time = time.perf_counter()
        self._thread = threading.Thread(target=self._run, name="midi-clock-scheduler", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None

    def _run(self) -> None:
        while self._running:
            now = time.perf_counter()
            sleep_for = self._next_tick_time - now
            if sleep_for > 0.0005:
                time.sleep(sleep_for - 0.0005)
            # Busy-wait the last sub-ms — time.sleep() on macOS is ~1 ms-precise,
            # too coarse for 24 PPQN at high BPM.
            while time.perf_counter() < self._next_tick_time:
                pass

            self._send_tick()

            with self._lock:
                self._tick_in_beat = (self._tick_in_beat + 1) % TICKS_PER_BEAT
                self._next_tick_time += self._tick_period_s

            # If we somehow fall >100ms behind schedule, skip the burst.
            if time.perf_counter() - self._next_tick_time > 0.1:
                self._next_tick_time = time.perf_counter()
