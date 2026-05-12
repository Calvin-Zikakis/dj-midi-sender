# dj-midi-sender

A bridge that makes external drum machines and synths behave **like another CDJ
deck in Sync mode** when slaved to a Pioneer **XDJ-XZ** (or any CDJ on the Pro
DJ Link network).

Concretely, that means:

- **Tempo follows the pitch slider.** Move the slider on the master deck and the
  drum machine's BPM moves with it, in real time.
- **Bar/beat position aligns with the master deck.** When the bridge starts the
  external device, its bar 1 lands on the master deck's bar 1 — so kicks line up
  with downbeats, not just tempo.

The primary target is keeping a **Teenage Engineering OP-XY** drum machine
locked to whatever's playing on the XZ; the same MIDI clock output drives synth
arpeggiators in lock-step too (Moog Subsequent 27 etc.).

## End product

A **small standalone box** — no laptop in the signal chain. The device has:

```
       ┌─────────────────────────────────┐
RJ45 ──┤ Ethernet (into the Pro DJ Link  ├── USB-A MIDI host (→ OP-XY)
       │ switch alongside the CDJs)      │
       │                                 ├── 5-pin DIN MIDI out (→ Sub 27)
       │   ESP32-S3 + W5500              │
       │                                 │
       └─────────────────────────────────┘
```

You plug it into the network switch that the CDJ(s) are on, plug your drum
machine and synth into its MIDI outputs, power it, and it follows the master
deck.

The XDJ-XZ does **not** emit MIDI clock natively over DIN or USB; Pro DJ Link
is the only path. The current phase is a Python reference implementation on a
laptop, which exists **purely to de-risk the firmware** — see
[project_phases.md](project_phases.md) for how the laptop prototype maps to
the eventual microcontroller build.

## Status

**Phase 1 — Python prototype.** Reverse-engineering of the beat packet wire
format is complete and validated against two pcapng captures (see
[../wireshark/](../wireshark/)). See [project_phases.md](project_phases.md)
for the roadmap.

## Repository layout

| Path | Purpose |
|------|---------|
| [../prototype/](../prototype/) | Python reference implementation (Phase 1) |
| [../wireshark/](../wireshark/) | Reference packet captures (Export mode, with pitch sweep) |
| [../claude/](../claude/) | Original project handoff notes |
| [architecture.md](architecture.md) | Wire-protocol details, PLL design |
| [project_phases.md](project_phases.md) | Phased roadmap |

## Quick start

Requires Python 3.10+. Recommended: a venv.

```bash
pip install mido python-rtmidi scapy
```

Hardware-free smoke test using a recorded capture:

```bash
# Terminal 1 — replay packets from a capture to localhost:50001
python prototype/replay.py wireshark/xdj-xz-export-mode-pitch-sweep.pcapng

# Terminal 2 — list available MIDI outputs, then run the bridge
python prototype/bridge.py --list-midi
python prototype/bridge.py --bind 127.0.0.1 --midi-port "IAC Driver Bus 1"
```

Live test against an actual XDJ-XZ (XZ must be in **Export mode**):

```bash
# Sanity-check beat reception with the on-screen flasher (no MIDI hardware required)
python prototype/bridge.py --no-midi --visualize

# Drive a synth, with the visualizer mirroring the beat
python prototype/bridge.py --midi-port "Your USB MIDI interface" --visualize

# Drive a synth, headless
python prototype/bridge.py --midi-port "Your USB MIDI interface"
```

The bridge sends MIDI **Start** on the first beat packet, **Stop** after 2
seconds of silence, and free-runs a PLL-corrected 24-PPQN clock in between.
`--visualize` opens a Tk window that flashes red on the downbeat and white
on beats 2–4 (Esc or window-close to quit). See [architecture.md](architecture.md)
for design details.
