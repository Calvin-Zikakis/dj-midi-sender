"""Tkinter beat visualizer.

A big circle flashes on every beat packet — red on the downbeat (beat 1),
white on beats 2-4. Four dots below the circle show position in the bar.
Esc or window-close quits.

When given an `offset_ctl` dict (provided by the bridge in MIDI mode), live
offset controls are bound:

    ←/→             ± 1 ms  grid offset  (per-track, session-only)
    ↓/↑             ±10 ms  grid offset
    Space / 0       reset grid offset to 0 (use between tracks)
    Shift+←/→       ± 1 ms  clock offset  (per-port, persisted)
    Shift+↓/↑       ±10 ms  clock offset
"""
from __future__ import annotations

import queue
import tkinter as tk
from threading import Event
from typing import Any, Mapping, Optional

from beat_packet import BeatPacket

BG = "#0a0a0a"
DIM = "#1a1a1a"
DOWNBEAT = "#ff3838"
BEAT = "#f5f5f5"
FLASH_MS = 120
POLL_MS = 10


def run_visualizer(
    beat_queue: "queue.Queue[BeatPacket]",
    stop_event: Event,
    offset_ctl: Optional[Mapping[str, Any]] = None,
) -> None:
    root = tk.Tk()
    root.title("dj-midi-sender")
    root.configure(bg=BG)
    root.geometry("460x580")

    canvas = tk.Canvas(root, width=400, height=340, bg=BG, highlightthickness=0)
    canvas.pack(pady=10)
    circle = canvas.create_oval(60, 20, 340, 300, fill=DIM, outline="")

    dots = []
    for i in range(4):
        x = 110 + i * 60
        d = canvas.create_oval(x, 312, x + 18, 330, fill=DIM, outline="")
        dots.append(d)

    bpm_var = tk.StringVar(value="—  BPM")
    info_var = tk.StringVar(value="waiting for beats…")
    clock_var = tk.StringVar(value="")
    grid_var = tk.StringVar(value="")
    hint_var = tk.StringVar(value="")

    tk.Label(root, textvariable=bpm_var, fg="#f0f0f0", bg=BG,
             font=("Helvetica", 26, "bold")).pack()
    tk.Label(root, textvariable=info_var, fg="#888888", bg=BG,
             font=("Helvetica", 12)).pack(pady=4)
    tk.Label(root, textvariable=grid_var, fg="#7fffaa", bg=BG,
             font=("Helvetica", 14, "bold")).pack(pady=(8, 0))
    tk.Label(root, textvariable=clock_var, fg="#7fbfff", bg=BG,
             font=("Helvetica", 11)).pack()
    tk.Label(root, textvariable=hint_var, fg="#555555", bg=BG,
             font=("Helvetica", 10)).pack(pady=(4, 0))

    def refresh_offsets() -> None:
        if offset_ctl is None:
            return
        grid_var.set(f"grid offset: {offset_ctl['get_grid']():+.1f} ms  (per-track)")
        clock_var.set(f"clock offset: {offset_ctl['get_clock']():+.1f} ms  (persisted, per-port)")
        hint_var.set("←/→ ±1   ↓/↑ ±10   space resets grid    shift+arrow tunes clock")

    refresh_offsets()

    def on_beat(pkt: BeatPacket) -> None:
        color = DOWNBEAT if pkt.beat_in_bar == 1 else BEAT
        canvas.itemconfig(circle, fill=color)
        for i, d in enumerate(dots):
            canvas.itemconfig(d, fill=color if (i + 1) == pkt.beat_in_bar else DIM)
        bpm_var.set(f"{pkt.effective_bpm:6.2f} BPM")
        info_var.set(
            f"track {pkt.track_bpm:.2f} × {pkt.pitch_multiplier:.4f}  "
            f"({pkt.pitch_percent:+.2f}%)  ·  beat {pkt.beat_in_bar}/4  ·  {pkt.device}#{pkt.device_num}"
        )
        root.after(FLASH_MS, lambda: canvas.itemconfig(circle, fill=DIM))

    def poll() -> None:
        if stop_event.is_set():
            root.destroy()
            return
        try:
            while True:
                on_beat(beat_queue.get_nowait())
        except queue.Empty:
            pass
        root.after(POLL_MS, poll)

    def quit_app(*_):
        stop_event.set()
        root.destroy()

    def nudge_grid(delta_ms: float):
        if offset_ctl is None:
            return
        offset_ctl["adjust_grid"](delta_ms)
        refresh_offsets()

    def nudge_clock(delta_ms: float):
        if offset_ctl is None:
            return
        offset_ctl["adjust_clock"](delta_ms)
        refresh_offsets()

    def reset_grid():
        if offset_ctl is None:
            return
        offset_ctl["reset_grid"]()
        refresh_offsets()

    root.protocol("WM_DELETE_WINDOW", quit_app)
    root.bind("<Escape>", quit_app)

    root.bind("<Right>", lambda e: nudge_grid(+1.0))
    root.bind("<Left>",  lambda e: nudge_grid(-1.0))
    root.bind("<Up>",    lambda e: nudge_grid(+10.0))
    root.bind("<Down>",  lambda e: nudge_grid(-10.0))

    root.bind("<Shift-Right>", lambda e: nudge_clock(+1.0))
    root.bind("<Shift-Left>",  lambda e: nudge_clock(-1.0))
    root.bind("<Shift-Up>",    lambda e: nudge_clock(+10.0))
    root.bind("<Shift-Down>",  lambda e: nudge_clock(-10.0))

    root.bind("<space>", lambda e: reset_grid())
    root.bind("0",       lambda e: reset_grid())

    root.after(POLL_MS, poll)
    root.mainloop()
