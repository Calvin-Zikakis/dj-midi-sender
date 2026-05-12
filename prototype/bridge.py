"""XDJ-XZ Pro DJ Link → MIDI clock bridge.

Listens for type-0x28 beat packets on UDP port 50001 and drives a
deadline-scheduled 24-PPQN MIDI clock out a chosen MIDI output port.
Sends MIDI Start on the master deck's downbeat, Stop after silence.

The lead-time compensation is split into two knobs:

- **Clock offset**: physical chain latency (USB transit + slave
  processing). Per-output-port, persisted to config, set once.
- **Grid offset**: per-track beat-grid skew (rekordbox's analyzed
  grid not matching where the kick actually lives). Session state,
  easy to nudge, not persisted.

The scheduler doesn't care about the distinction — it applies the sum.
"""
from __future__ import annotations

import argparse
import socket
import sys
import time
from threading import Event
from typing import Callable, Optional

from beat_packet import parse_beat_packet, BeatPacket

DJ_LINK_BEAT_PORT = 50001
SILENCE_TIMEOUT_S = 2.0
SOCKET_TIMEOUT_S = 0.25
STATUS_INTERVAL_S = 0.5


class Bridge:
    def __init__(
        self,
        midi_port_name: Optional[str],
        bind_addr: str,
        port: int,
        no_midi: bool = False,
        on_beat: Optional[Callable[[BeatPacket], None]] = None,
        clock_offset_ms: float = 0.0,
        grid_offset_ms: float = 0.0,
    ):
        self.bind_addr = bind_addr
        self.port = port
        self.no_midi = no_midi
        self.on_beat = on_beat
        self.playing = False
        self.last_beat_time = 0.0
        self._last_status_time = 0.0

        # Two-axis lead compensation; scheduler sees the sum.
        self._clock_offset_ms = float(clock_offset_ms)
        self._grid_offset_ms = float(grid_offset_ms)
        self._midi_port_name: Optional[str] = None

        if no_midi:
            self.midi_out = None
            self.scheduler = None
            self._clock_msg = self._start_msg = self._stop_msg = None
            print("[midi] disabled (--no-midi)", file=sys.stderr)
            return

        import mido  # lazy so --no-midi works without python-rtmidi installed

        if midi_port_name is None:
            names = mido.get_output_names()
            if not names:
                raise RuntimeError("No MIDI output ports available. Use --list-midi to inspect.")
            midi_port_name = names[0]
            print(f"[midi] using first available output: {midi_port_name}", file=sys.stderr)
        self.midi_out = mido.open_output(midi_port_name)
        self._midi_port_name = midi_port_name

        self._clock_msg = mido.Message("clock")
        self._start_msg = mido.Message("start")
        self._stop_msg = mido.Message("stop")

        from scheduler import MidiClockScheduler
        self.scheduler = MidiClockScheduler(
            send_tick=self._send_clock,
            offset_ms=self._total_offset_ms(),
        )

    # ---- offset controls ----

    def _total_offset_ms(self) -> float:
        return self._clock_offset_ms + self._grid_offset_ms

    def _apply_offsets(self) -> None:
        if self.scheduler is not None:
            self.scheduler.set_offset_ms(self._total_offset_ms())

    def adjust_clock_offset_ms(self, delta_ms: float) -> float:
        self._clock_offset_ms += delta_ms
        self._apply_offsets()
        if self._midi_port_name:
            import config
            config.save_clock_offset_ms(self._midi_port_name, self._clock_offset_ms)
        return self._clock_offset_ms

    def adjust_grid_offset_ms(self, delta_ms: float) -> float:
        self._grid_offset_ms += delta_ms
        self._apply_offsets()
        return self._grid_offset_ms

    def reset_grid_offset(self) -> None:
        self._grid_offset_ms = 0.0
        self._apply_offsets()

    def get_clock_offset_ms(self) -> float:
        return self._clock_offset_ms

    def get_grid_offset_ms(self) -> float:
        return self._grid_offset_ms

    # ---- MIDI / network plumbing ----

    def _send_clock(self) -> None:
        self.midi_out.send(self._clock_msg)

    def run(self, stop_event: Optional[Event] = None) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.bind_addr, self.port))
        sock.settimeout(SOCKET_TIMEOUT_S)
        print(f"[net] listening on {self.bind_addr}:{self.port}", file=sys.stderr)

        def should_stop() -> bool:
            return stop_event is not None and stop_event.is_set()

        try:
            while not should_stop():
                try:
                    data, _addr = sock.recvfrom(2048)
                except socket.timeout:
                    self._check_silence()
                    continue
                pkt = parse_beat_packet(data)
                if pkt is None:
                    continue
                self._handle_beat(pkt)
        except KeyboardInterrupt:
            print("\n[bridge] interrupted", file=sys.stderr)
        finally:
            self._stop_playback()
            if self.midi_out is not None:
                self.midi_out.close()
            sock.close()

    def _handle_beat(self, pkt: BeatPacket) -> None:
        now = time.perf_counter()
        self.last_beat_time = now

        if not self.no_midi:
            if not self.playing:
                # Hold MIDI Start until a downbeat so the slave's bar 1 lines up
                # with the master deck's bar 1 — Sync, not just tempo.
                if pkt.beat_in_bar == 1:
                    self._start_playback(pkt)
                else:
                    self._log_throttled(
                        f"[bridge] waiting for downbeat (got beat {pkt.beat_in_bar}/4)",
                        now,
                    )
            else:
                self.scheduler.beat_arrived(pkt.ms_next_beat)

        if self.on_beat is not None:
            self.on_beat(pkt)

        if now - self._last_status_time >= STATUS_INTERVAL_S:
            self._last_status_time = now
            sys.stdout.write(
                f"\r{pkt.device:>8s}#{pkt.device_num}  "
                f"bpm {pkt.effective_bpm:7.3f} "
                f"(track {pkt.track_bpm:6.2f} × {pkt.pitch_multiplier:.4f}, "
                f"pitch {pkt.pitch_percent:+7.2f}%)  beat {pkt.beat_in_bar}/4   "
            )
            sys.stdout.flush()

    def _start_playback(self, pkt: BeatPacket) -> None:
        print(f"\n[bridge] ▶ start  bpm={pkt.effective_bpm:.3f}", file=sys.stderr)
        self.midi_out.send(self._start_msg)
        self.scheduler.start()
        # Seed the scheduler with this beat's prediction so the very first beat
        # has a correct tick period (rather than the 120-BPM default).
        self.scheduler.beat_arrived(pkt.ms_next_beat)
        self.playing = True

    def _stop_playback(self) -> None:
        if not self.playing:
            return
        print("\n[bridge] ■ stop", file=sys.stderr)
        self.scheduler.stop()
        self.midi_out.send(self._stop_msg)
        self.playing = False

    def _check_silence(self) -> None:
        if self.playing and (time.perf_counter() - self.last_beat_time) > SILENCE_TIMEOUT_S:
            self._stop_playback()

    def _log_throttled(self, msg: str, now: float) -> None:
        if now - self._last_status_time >= STATUS_INTERVAL_S:
            self._last_status_time = now
            print(msg, file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description="XDJ-XZ → MIDI clock bridge")
    ap.add_argument("--midi-port", help="MIDI output port name (default: first available)")
    ap.add_argument("--list-midi", action="store_true", help="List MIDI output ports and exit")
    ap.add_argument("--no-midi", action="store_true",
                    help="Skip MIDI output (useful with --visualize for beat-detection sanity checks)")
    ap.add_argument("--visualize", action="store_true",
                    help="Open a window that flashes on every beat and exposes live offset controls")
    ap.add_argument("--bind", default="0.0.0.0", help="UDP bind address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=DJ_LINK_BEAT_PORT, help=f"UDP port (default {DJ_LINK_BEAT_PORT})")
    ap.add_argument(
        "--clock-offset-ms",
        type=float,
        default=None,
        help="Lead the beat by N ms to compensate physical chain latency (USB → slave). "
             "Persists per-port. If omitted, loads the saved value for the chosen MIDI port. "
             "In --visualize mode, tune live with Shift+←/→ (±1 ms) and Shift+↓/↑ (±10 ms).",
    )
    ap.add_argument(
        "--grid-offset-ms",
        type=float,
        default=0.0,
        help="Initial per-track beat-grid offset in ms (not persisted). "
             "In --visualize mode, ←/→ ±1 ms, ↓/↑ ±10 ms, Space/0 resets.",
    )
    args = ap.parse_args()

    if args.list_midi:
        import mido
        for name in mido.get_output_names():
            print(name)
        return

    # Resolve the persisted clock-offset before constructing the Bridge.
    clock_offset_ms = args.clock_offset_ms
    if clock_offset_ms is None and not args.no_midi:
        import mido
        port_name = args.midi_port
        if port_name is None:
            names = mido.get_output_names()
            port_name = names[0] if names else None
        if port_name is not None:
            import config
            saved = config.get_clock_offset_ms(port_name, default=0.0)
            if saved != 0.0:
                print(f"[config] loaded clock offset {saved:+.1f} ms for {port_name!r}",
                      file=sys.stderr)
            clock_offset_ms = saved
    if clock_offset_ms is None:
        clock_offset_ms = 0.0

    if args.visualize:
        import queue
        from threading import Thread
        from visualizer import run_visualizer

        beat_queue: "queue.Queue[BeatPacket]" = queue.Queue()
        stop_event = Event()

        bridge = Bridge(
            midi_port_name=args.midi_port,
            bind_addr=args.bind,
            port=args.port,
            no_midi=args.no_midi,
            on_beat=beat_queue.put_nowait,
            clock_offset_ms=clock_offset_ms,
            grid_offset_ms=args.grid_offset_ms,
        )

        offset_ctl = None
        if bridge.scheduler is not None:
            offset_ctl = {
                "adjust_grid": bridge.adjust_grid_offset_ms,
                "adjust_clock": bridge.adjust_clock_offset_ms,
                "reset_grid": bridge.reset_grid_offset,
                "get_grid": bridge.get_grid_offset_ms,
                "get_clock": bridge.get_clock_offset_ms,
            }

        worker = Thread(target=bridge.run, args=(stop_event,), name="bridge-net", daemon=True)
        worker.start()
        try:
            run_visualizer(beat_queue, stop_event, offset_ctl=offset_ctl)
        finally:
            stop_event.set()
            worker.join(timeout=2.0)
        return

    Bridge(
        midi_port_name=args.midi_port,
        bind_addr=args.bind,
        port=args.port,
        no_midi=args.no_midi,
        clock_offset_ms=clock_offset_ms,
        grid_offset_ms=args.grid_offset_ms,
    ).run()


if __name__ == "__main__":
    main()
