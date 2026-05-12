# Project phases

The end product is a **standalone microcontroller-based box** that plugs into
the Pro DJ Link Ethernet switch (alongside the CDJs) and exposes a 5-pin DIN
MIDI out and a USB MIDI host port. No laptop in the signal chain. A drum
machine plugged into its USB port behaves like another CDJ deck in Sync mode —
tempo follows the master's pitch slider, bar/beat position aligns with the
master.

The Python work in [../prototype/](../prototype/) is a **reference
implementation, not the deliverable.** Its only job is to de-risk the
firmware: validate the wire-protocol decoding, prove the PLL+MIDI approach
sounds good on real synths, and serve as a working spec to port from.

See [architecture.md](architecture.md) for protocol/PLL details.

## Phase 1 — Python prototype on laptop  ◀ current

```
XDJ-XZ ─ethernet─→ Mac (Python bridge) ─USB MIDI─→ Sub 27 DIN MIDI in
                                                  └→ OP-XY USB MIDI in (later)
```

**Goal:** validate the full path — packet capture → parse → PLL → MIDI clock
with bar alignment — against real hardware before committing to firmware.

Deliverables:

- [x] Beat packet parser ([prototype/beat_packet.py](../prototype/beat_packet.py))
- [x] Deadline-based 24-PPQN MIDI clock scheduler driven by `ms_next_beat`
      ([prototype/scheduler.py](../prototype/scheduler.py)) — replaces the
      original reactive-PLL approach
- [x] Bridge entry point ([prototype/bridge.py](../prototype/bridge.py))
- [x] On-screen beat visualizer with live offset controls
      ([prototype/visualizer.py](../prototype/visualizer.py))
- [x] Offline pcapng replay tool ([prototype/replay.py](../prototype/replay.py))
- [x] Hold MIDI Start until `beat_in_bar == 1` so the slave's bar 1 aligns
      with the master's
- [x] Persisted per-port clock-latency offset
      ([prototype/config.py](../prototype/config.py)) + session-only beat-grid
      offset, tunable live with arrow keys vs shift+arrow keys
- [x] Smoke test: XZ → bridge → OP-XY, manually verified locking with the
      OP-XY's sequencer playing in time with the master deck
- [ ] Live test across a pitch sweep — does the OP-XY hold time without any
      grid-offset re-tuning?
- [ ] Live test with the Sub 27 on DIN (will need a USB MIDI → DIN interface)
- [ ] Calibrate the OP-XY's clock offset by ear and save it

**Exit criteria:** with the per-port clock offset dialed in once and saved,
plugging the OP-XY in and starting a track on the XZ produces in-time playback
without further intervention. Per-track grid nudges, if needed, are a few key
presses.

## Phase 1.5 — Drift recovery

Bar-alignment as implemented in Phase 1 is **set at Start only**. If a beat
packet is dropped, the slave may end up offset from the master without the
bridge knowing. To stay genuinely CDJ-Sync-equivalent:

- [ ] Track `beat_in_bar` across successive packets; detect missed-beat gaps.
- [ ] On detected slip, force a re-align — Stop, then Start on the next
      downbeat. Audible but quick.
- [ ] Optional: emit MIDI **Song Position Pointer (SPP)** every bar so devices
      that honor it (TBD whether OP-XY does) snap back without restarting.

## Phase 2 — ESP32-S3 firmware (DIN MIDI only)

```
Pro DJ Link switch ─RJ45─→ [ W5500 ─SPI─ ESP32-S3 ─UART─ DIN MIDI ] ──→ Sub 27
```

Port Phase 1 to C/C++ (or Rust + `esp-hal`, TBD). The microcontroller is the
product from this point on; the Python prototype is reference material.

Hardware:

- **ESP32-S3 dev board** with native USB OTG (required so Phase 3 doesn't need
  a board swap). Need to confirm which boards on hand qualify.
- **W5500 SPI Ethernet module** (~$8) — handles the Pro DJ Link RJ45 input.
- **5-pin DIN female panel jack** + standard MIDI output resistors (~$2).
- USB-C jack for power (and dev/flash).

Firmware structure (mirrors the Python modules):

| Python module | Firmware analogue |
|---------------|-------------------|
| [`beat_packet.py`](../prototype/beat_packet.py) | `beat_packet.c` — pure parsing, byte-for-byte identical offsets |
| [`pll.py`](../prototype/pll.py) | hardware-timer ISR (microsecond precision built-in; busy-wait disappears) |
| [`bridge.py`](../prototype/bridge.py) | `main.c` — lwIP UDP listener + UART TX state machine |

Likely fixed-point conversion: tick interval in microseconds is comfortably
within `uint32_t`, no FPU needed.

**Exit criteria:** standalone box matches the Phase-1 laptop bridge's lock
quality driving the Sub 27. No laptop in the signal path.

## Phase 3 — Add USB MIDI host (OP-XY)

```
                                                  ┌─UART──→ DIN MIDI ──→ Sub 27
Pro DJ Link switch ─RJ45─→ [ W5500 ── ESP32-S3 ]──┤
                                                  └─USB MIDI host (A jack) ──→ OP-XY
```

The product gains its second output: a USB-A jack acting as a **USB MIDI
host** so any class-compliant USB MIDI device (OP-XY, Digitakt, MPC, etc.)
plugs straight in. Requires native USB on the ESP32-S3 (already in place from
Phase 2). USB MIDI host stack: tinyusb has a class driver; whether we use it
directly or via Arduino-on-ESP32 is TBD.

Both outputs share the same PLL — there is exactly one tick generator. The
DIN and USB sinks are just two destinations the tick callback writes to.

**Exit criteria:** plug an OP-XY into the USB-A jack; its sequencer plays in
time with the master deck, downbeat-aligned, while the Sub 27 simultaneously
clocks off the DIN jack.

## Phase 4 — Productization

- Custom PCB combining ESP32-S3 + W5500 + DIN buffer + USB-A host jack +
  power, replacing the modular dev boards.
- Enclosure (3D-printed or off-the-shelf project box).
- OLED display: current BPM, pitch %, lock state, master deck #.
- Downbeat LED (gate on `beat_in_bar == 1`).
- Tap-tempo button for unanalyzed audio / non-DJ-Link sources.
- MIDI **Start / Stop** driven by parsed status packets (port 50002).
  Requires announcing as a virtual CDJ — adds protocol surface area.

## Deferred / "maybe someday"

- Multi-CDJ master handoff tracking via status packets.
- Bidirectional **Ableton Link** bridge.
- **OSC** output for lighting / visuals.
- **DMX** output triggered on downbeat (would slot in with existing ESP32 LED
  work).
