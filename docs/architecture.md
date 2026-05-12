# Architecture

This document captures the protocol details and the design of the bridge.
All facts about wire format and timing were verified empirically against the
two captures in [../wireshark/](../wireshark/) — not just lifted from the
[deep-symmetry DJ Link analysis](https://djl-analysis.deepsymmetry.org/djl-analysis/).

## Pro DJ Link on the XDJ-XZ

The XZ blasts Pro DJ Link traffic to broadcast on the link-local network as
soon as a track is playing on the master deck in **Export mode**. No virtual
CDJ announce is required to *receive* beat packets — the XZ transmits
unsolicited.

| Port | Direction | Type | Cadence |
|------|-----------|------|---------|
| 50000 | XZ → broadcast | `0x06` keep-alive announce | ~500 ms |
| 50001 | XZ → broadcast | `0x28` beat packets | one per beat |
| 50002 | XZ → broadcast | status packets | not seen without virtual CDJ |

Source addressing observed: IP `169.254.182.222` (link-local), MAC
`c8:3d:fc:0d:b6:de`; destination `169.254.255.255`.

### Mode requirements

- **Export mode** (XZ playing from USB stick): Pro DJ Link active. **Required.**
- **Performance mode** (USB to rekordbox on laptop): XZ drops off Pro DJ Link
  entirely. Not viable for this project.
- **Unanalyzed audio** (raw MP3 with no rekordbox beat grid): the BPM field is
  not transmitted reliably. Tap-tempo fallback is on the Phase-4 roadmap.

## Beat packet format

All offsets are relative to the start of the UDP payload. Implemented by
[`parse_beat_packet`](../prototype/beat_packet.py).

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| `0x00` | 10 B | Magic header | ASCII `Qspt1WmJOL` |
| `0x0A` | 1 B  | Packet type | `0x28` for beat |
| `0x0B` | 21 B | Device name | ASCII, null-padded (e.g. `XDJ-XZ`) |
| `0x20` | 1 B  | Device subtype | |
| `0x21` | 1 B  | Device number | 1–4, matches deck |
| `0x22` | 2 B  | Payload length | uint16 BE |
| `0x24` | 4 B  | ms until next beat | uint32 BE |
| `0x28` | 4 B  | ms until 2nd beat | uint32 BE |
| `0x2C` | 4 B  | ms until next bar (downbeat) | uint32 BE |
| `0x30` | 4 B  | ms until 4th beat | uint32 BE |
| `0x34` | 4 B  | ms until 2nd bar | uint32 BE |
| `0x38` | 4 B  | ms until 8th beat | uint32 BE |
| `0x3C` | 24 B | reserved (zeros / `0xFF` pad) | |
| `0x54` | 4 B  | Pitch multiplier (raw) | uint32 BE, see below |
| `0x58` | 2 B  | reserved | typically `0x0000` |
| `0x5A` | 2 B  | Track BPM × 100 | uint16 BE |
| `0x5C` | 1 B  | Beat-within-bar | 1–4 |
| `0x5D` | 1 B  | Device number (echo) | |

### Pitch encoding (the gotcha)

The 32-bit pitch field encodes a multiplier, not a percentage. `0x00100000`
(= 1 048 576) represents 1.0×. To decode:

```python
multiplier  = pitch_raw / 0x00100000
pitch_pct   = (multiplier - 1.0) * 100
```

Verified reference points across the XZ's pitch range modes:

| Raw | Multiplier | % | Notes |
|-----|------------|---|-------|
| `0x00100000` | 1.000 | 0 % | resting |
| `0x0010F5C2` | 1.060 | +6.00 % | top of standard ±6 % |
| `0x00200000` | 2.000 | +100 % | max WIDE up |
| `0x000F0A3D` | 0.940 | −6.00 % | bottom of standard ±6 % |
| `0x00080000` | 0.500 | −50 % | |
| `0x0004CCCC` | 0.300 | −70 % | observed minimum |

Range observed in captures: **−70 % to +100 %**. Decoders must accept the full
uint32 range — `int8 percent` field encodings used by some Pioneer protocol
docs do **not** apply here.

### Effective BPM

The transmitted BPM at `0x5A` is the **track's native BPM**, not the
playing BPM. Effective BPM is computed:

```
effective_bpm = track_bpm × pitch_multiplier
```

In capture `xdj-xz-export-mode-pitch-sweep.pcapng`, track BPM stayed at 136.00
across the entire pitch sweep; inter-packet arrival times exactly matched
`60_000 / (136 × multiplier)` ms. The packet *cadence* is itself ground truth
for tempo, but the field values lock faster on tempo changes.

## Timing characteristics

Measured from the steady-tempo capture `xdj-xz-export-mode.pcapng`:

- Track BPM 132 → expected inter-beat interval 454.5 ms.
- Observed range: **452.6–455.9 ms**, jitter ≈ ±2 ms.
- Occasional missed/coalesced packets (e.g. one ~934 ms gap = two intervals
  fused). The PLL must free-run through gaps and resync on the next beat.

## Clock scheduler

Implemented in [`MidiClockScheduler`](../prototype/scheduler.py).

**Wrong design A:** burst 24 `0xF8` ticks on each beat packet. Network jitter
propagates straight into clock jitter, and downstream synths sound terrible.

**Wrong design B:** reactive PLL — free-run at `60 / bpm / 24` and apply
phase corrections each tick. This works but inherits per-packet network jitter
as phase wobble, and creates audible transients on every pitch-slider move
while the loop reconverges. (This was the v1 implementation.)

**Used design — two-step tempo/phase scheduler:** every beat packet carries
the master deck's prediction of when its *next* beat will be (`ms_next_beat`,
offset 0x24). We use it as the tempo source, and we track phase separately:

1. **Tempo (immediate):** `tick_period = ms_next_beat / 24`. Applied on the
   very next tick.
2. **Phase (gradual):** the ideal `_next_tick_time` given the master's
   just-announced boundary at `now` is `(now − offset) + tick_in_beat × period`.
   The signed shortest distance between the current `_next_tick_time` and
   this ideal is the phase error, wrapped to ±half a beat. A fraction of
   that error (default 0.35) is applied per packet, so per-packet network
   jitter averages out over a few beats instead of being passed straight
   through to the tick stream.

A failed earlier design tried to do both at once by computing the period as
`(t_target − t_next_tick) / ticks_remaining`. That blew up when
`ticks_remaining` was small (1–2), exactly the regime that occurs at tempo
changes — a near-full-beat span divided by a tiny denominator yields a
runaway period clamped to ~125 ms (8 ticks/sec). Keeping tempo and phase
strictly separate makes the worst-case period at most `MAX_PERIOD_S` (20
BPM equivalent), but only ever in response to a corrupt packet.

Properties of the working design:

- **Tempo is taken directly from `ms_next_beat / 24`** — no BPM × pitch math,
  no convergence required. The XZ's own beat-grid is the source of truth.
- **Pitch-slider changes propagate within one beat** with no transient. The
  next packet just updates the period; the tick thread picks it up on the
  next iteration.
- **Per-packet network jitter** (±2 ms in the captures) is averaged out by
  the 0.35 phase-correction gain — RMS jitter on tick timing ends up ~0.9 ms.
- **Ports cleanly to firmware**: two scalar updates per packet (period, then
  a small bump to the deadline). The tick thread maps to a hardware-timer
  ISR whose period is the only shared mutable variable.

## Lead-time compensation

The slave device's first sound lags behind our tick due to a chain of physical
delays (USB transit, OP-XY internal scheduling, etc.). The total is on the
order of 10–30 ms and is **constant in milliseconds** — it doesn't scale with
tempo. The scheduler compensates by firing tick 0 of each beat `offset_ms`
*before* the predicted beat boundary.

The compensation is split into **two independent offsets** that sum:

| Axis | Source of error | Scope | Persistence |
|------|-----------------|-------|-------------|
| **Clock offset** | USB / slave processing latency | Per output port | [`~/.config/dj-midi-sender.json`](../prototype/config.py), loaded on startup |
| **Grid offset**  | rekordbox's analyzed beat-grid not lining up with where the kick actually sits in the audio | Per track / per session | Not persisted |

Rationale for splitting: clock latency is set-and-forget for a given device,
but grid skew varies per track (an analyzed track with a sloppy intro, a song
where the producer pushes the kick a few ms ahead of the grid, etc.). Mixing
them into one knob means re-tuning the persisted value every time the next
track has a different feel. Keeping them separate means the persisted clock
offset stays correct across tracks and the user only ever fiddles with the
grid offset between tracks.

The scheduler itself doesn't know about the split — `bridge.py` owns both
values and pushes their sum to `scheduler.set_offset_ms()`.

### Output timing precision

Python's `time.sleep()` on macOS is ~1 ms-precise — too coarse for 24 PPQN at
high BPM (at 200 BPM, a tick interval is 12.5 ms). The PLL thread sleeps to
~500 µs before each tick deadline, then busy-waits the last bit on
`time.perf_counter()`. On modern hardware this lands within ~50–100 µs of
schedule; downstream synths smooth what remains.

## Sync model

The product goal is to behave like a CDJ in **Sync** mode on the link: the
external device's tempo *and* bar position track the master deck. This has two
parts, handled differently.

### Tempo sync (continuous)

Every beat packet carries `track_bpm` and a pitch multiplier. The PLL applies
the new effective BPM immediately on packet arrival, so moving the master
deck's pitch slider continuously retunes the MIDI clock.

### Bar / beat alignment (one-shot, at Start)

MIDI clock is a stream of `0xF8` ticks with no inherent position information.
The downstream device infers bar position from where it received MIDI **Start**:
the next tick is taken as bar 1, beat 1.

So to make the drum machine's bar 1 align with the master deck's bar 1, the
bridge **holds the MIDI Start message until it sees a beat packet with
`beat_in_bar == 1`** (the next downbeat). On a 4/4 track this means the first
sound from the drum machine is delayed by up to one bar — usually < 2 seconds
at typical BPMs. Once locked, continuous tempo tracking via the PLL keeps the
alignment intact.

### Drift recovery (not yet implemented)

The current design re-syncs bar position **only at Start**. If a beat packet is
dropped or coalesced (one ~934 ms gap was observed in the steady-tempo capture)
and the PLL silently phase-corrects to the next received beat, the drum machine
may end up offset by one beat from the master without realizing it.

Two planned mitigations:

- **Track `beat_in_bar` continuously.** When successive packets show
  unexpected `beat_in_bar` deltas (e.g. last seen beat 2 → received beat 4),
  the bridge knows it missed a beat. Action: ride out the gap, then on the next
  downbeat re-send **Stop + Start** to forcibly re-align.
- **Periodic MIDI Song Position Pointer (SPP).** Some drum machines support
  SPP, which encodes position in MIDI beats (16th notes). Sending an SPP
  message every bar lets the device snap back into alignment without an audible
  restart. Needs confirmation that the OP-XY honors SPP mid-stream.

### Start / Stop semantics

- MIDI **Start** (`0xFA`) is sent on the first beat packet with
  `beat_in_bar == 1` after the bridge boots or after a previous Stop.
- MIDI **Stop** (`0xFC`) is sent after 2 seconds of socket silence.

There is no MIDI **Continue** — restart after a stop is a fresh `Start` on the
next downbeat. Parsing port 50002 status packets to detect play/pause directly
is on the Phase-4 roadmap and would require announcing a virtual CDJ on the
network.

## Designing for the microcontroller

The deliverable is firmware, not the Python code. The Python prototype is
written to be **directly portable** — every design decision below was made
with the firmware target in mind:

| Python | Firmware |
|--------|----------|
| `time.perf_counter()` sleep+busy-wait | ESP32 hardware timer ISR (µs-resolution built in) |
| UDP socket `recvfrom` | lwIP raw UDP callback on W5500 |
| `mido` MIDI out | UART TX for the DIN jack + tinyusb MIDI host for the USB-A jack |
| `float` periods / offsets | `uint32_t` / `int32_t` microseconds (no FPU needed) |
| `threading.Thread` scheduler loop | one timer ISR + one UDP packet handler |
| `~/.config/dj-midi-sender.json` | NVS / EEPROM region for per-port clock offset |

The scheduler is integer-friendly end-to-end: `ms_next_beat` arrives as an
int, divided by 24 gives the tick period in microseconds, and the deadline
math is a single signed subtract per beat. No floating-point necessary on the
microcontroller.

The hold-Start-until-downbeat behavior is a single state bit; the silence
timeout is a `millis()` comparison. Nothing in the design assumes the
flexibility of a general-purpose OS.
