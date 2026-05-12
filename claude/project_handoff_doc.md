# XDJ-XZ → MIDI Clock Bridge: Project Handoff v2

**Author**: Calvin (staff backend engineer, Go/Python/Rust)  
**Status**: Research complete, ready to build Phase 1 Python prototype  
**Last updated**: After full Wireshark capture analysis + dual-source tempo tracking research

---

## The goal

Build a standalone hardware bridge that listens to a Pioneer XDJ-XZ's Pro DJ Link Ethernet broadcast, extracts the master deck's live tempo and beat phase, and outputs rock-solid MIDI clock that tracks tempo changes in real time — including smooth slider sweeps — to:

- **Moog Subsequent 27** → DIN MIDI in
- **Teenage Engineering OP-XY** → USB MIDI host

The XDJ-XZ has no native MIDI clock output. Pro DJ Link over Ethernet is the only extraction path.

**End-state hardware**: ESP32-S3 + W5500 SPI Ethernet + DIN MIDI out + USB MIDI host, enclosed.  
**Current phase**: Phase 1 — Python prototype on laptop.

---

## What has been empirically verified

Two Wireshark pcapng captures were taken of the XZ in Export mode with a rekordbox-analyzed track playing. Both were fully parsed with Python/tshark. Everything below is confirmed against real hardware data, not assumptions.

### Beat packets — port 50001

The XZ broadcasts beat packets **natively without a virtual CDJ**. No announce spoofing needed to get beat data. One packet fires per beat.

**Packet structure** (all offsets from start of UDP payload):

```
0x00–0x09  magic header: 51 73 70 74 31 57 6d 4a 4f 4c  ("Qspt1WmJOL")
0x0A       packet type: 0x28
0x0B–0x1F  device name, ASCII null-padded (e.g. "XDJ-XZ")
0x20       device subtype
0x21       device number (1–4)
0x22–0x23  payload length (uint16 BE)
0x24–0x27  ms until next beat (uint32 BE) — at normal pitch, must scale by multiplier
0x28–0x2B  ms until 2nd beat
0x2C–0x2F  ms until next bar (downbeat)
0x30–0x33  ms until 4th beat
0x34–0x37  ms until 2nd bar
0x38–0x3B  ms until 8th beat
0x3C–0x53  reserved / 0xFF padding
0x54–0x57  pitch as uint32 BE  ← ACTUAL effective pitch (see below)
0x58–0x59  reserved
0x5A–0x5B  track native BPM × 100 as uint16 BE  (e.g. 0x3520 = 136.00 BPM)
0x5C       beat-within-bar (1–4)
0x5D       device number (redundant)
```

**Verified timing quality**: ±2ms jitter at constant pitch. At 132 BPM (454.5ms theoretical interval), actual ranged 452.6–455.9ms. Excellent for PLL input.

**Missed packet handling**: One gap of 934ms (double interval) was observed during the pitch sweep. The PLL must free-run from last known tempo when packets are late — do not assume continuous arrival.

### Pitch encoding (confirmed across all slider ranges)

The pitch field at `0x54` is a uint32 BE. Decoding:

```python
multiplier    = raw / 0x00100000      # 0x00100000 == 1.0x == 0%
pitch_percent = (multiplier - 1.0) * 100
effective_bpm = track_bpm * multiplier
```

Reference values confirmed in captures:
```
0x00100000 = 1.000x =   0.00%  (resting)
0x0010F5C2 = 1.060x =  +6.00%  (standard range max)
0x0010F5C2 = 1.060x =  +6.00%
0x000F0A3D = 0.940x =  -6.00%
0x00128F5C = 1.160x = +16.00%  (±16% range)
0x00200000 = 2.000x = +100.00% (WIDE range max)
0x0004CCCC = 0.300x =  -70.00% (WIDE range min observed)
```

Pitch was swept across ±6%, ±10%, ±16%, and WIDE range modes. **Encoding is identical across all modes — handle the full uint32 range.**

The BPM field (`0x5A`) is the **track's native BPM, never changes with pitch**. Effective BPM must be computed: `track_bpm × multiplier`. Confirmed: track BPM stayed at 136.00 across all 144 pitch-sweep packets.

### Status packets — port 50002

Status packets were **not seen** in the Wireshark captures because a Virtual CDJ was not present on the network. Status packets require a virtual CDJ announce to unlock. Beat packets do not.

Status packets are the **critical addition in v2** — they are what enables smooth real-time tempo tracking during pitch slider changes, equivalent to native CDJ sync follower behavior. See the architecture section below.

---

## Why dual-source (status + beat) is necessary

**Beat-packet-only approach** (original plan):
- Tempo update latency during a slider sweep: ~500ms (one beat at 130 BPM)
- The drum machine hears a BPM snap at the next beat boundary when the XZ's pitch has already moved significantly

**Dual-source approach** (what CDJ sync followers actually do):
- Status packets arrive every ~200ms and carry the live `Pitch1` field
- Tempo is updated immediately from every status packet
- Beat packets are used **only for phase correction** — not tempo
- Tempo update latency during a slider sweep: ~200ms
- Drum machine hears a smooth continuous tempo change, indistinguishable from native CDJ sync behavior

This is confirmed by beat-link's architecture: `tempoChanged()` fires from status packet `Pitch1` updates, producing multiple callbacks per second during a sweep. Beat arrivals only trigger phase correction.

---

## Status packet structure (port 50002)

Status packets are type `0x0A`, approximately 208 bytes. They are sent directly (unicast) to your virtual CDJ's IP on port 50002 once you announce yourself on the network.

**Key fields** (all offsets from start of UDP payload):

```
0x00–0x09  magic header (same as all packets)
0x0A       packet type: 0x0A
0x0B–0x1F  device name (20 bytes)
0x21       device number
0x28–0x2B  Pitch1 (uint32 BE) — ACTUAL effective pitch, same encoding as beat packet pitch
           USE THIS ONE. Reflects actual playing tempo whether from local fader or sync.
0x2C–0x2F  (reserved / other fields)
0x30–0x33  Pitch2 (uint32 BE) — LOCAL FADER POSITION ONLY. Ignore this.
           Pitch2 is always tied to where the physical fader sits.
           When CDJ is synced to a master, Pitch1 ≠ Pitch2.
0x5A–0x5B  track BPM × 100 (uint16 BE) — same as beat packet
0x89       flags byte:
             bit 6 = Playing (1 = playing, 0 = stopped/cued)
             bit 5 = Master  (1 = this player is tempo master)
             bit 4 = Sync    (1 = sync mode on)
             bit 3 = On-Air  (1 = fader up, heard in mix)
             bit 1 = BPM-only sync (beat alignment lost, tempo only)
0x90–0x91  Mv (uint16 BE) — track validity flag:
             0x8000 = rekordbox-analyzed track loaded → BPM valid, use it
             0x7FFF = no track loaded
             0x0000 = non-rekordbox track (CD or unanalyzed) → BPM invalid, freeze last value
0xA6       beat-within-bar (1–4) — same as beat packet 0x5C
```

**Critical rule**: Only trust BPM when `Mv == 0x8000`. If `Mv != 0x8000`, freeze the last known good effective BPM and keep the clock free-running at that rate.

**Critical rule**: Always use `Pitch1` at `0x28`, never `Pitch2` at `0x30`. When a CDJ is in sync mode following a master, `Pitch1` tracks the master's tempo while `Pitch2` stays at the local fader position. They diverge.

---

## Virtual CDJ: required to unlock status packets

To receive status packets, you must announce yourself as a virtual CDJ by sending keep-alive packets to port 50000 every ~1500ms. The XZ then sends status packets directly (unicast) to your IP on port 50002.

**Keep-alive packet structure** (mirror an XZ keep-alive from the capture, substitute your MAC/IP):

```
0x00–0x09  magic header
0x0A       type: 0x06
0x0B–0x1F  device name (20 bytes, null-padded): e.g. "xdj-bridge\0\0..."
0x20       device subtype: 0x01
0x21       device number: 0x07  (safe value, won't conflict with CDJ deck numbers 1–4)
0x22–0x23  packet length
...        (copy structure exactly from XZ's own keep-alive packet in Wireshark capture)
           Use your actual MAC address and your actual IP
```

In Python, the `python-prodj-link` library (`flesniak/python-prodj-link`) already implements this correctly — see its `vcdj.py`. Use it as the reference or import it directly.

**Important**: The virtual CDJ announcement is also what triggers the XZ to send more complete data in its own status packets. Some fields are only populated when another device is "present" on the network.

---

## Two-source clock architecture

This is the core architecture. Tempo and phase have separate, independent update paths.

```
┌─────────────────────────────────────────────────────────────────┐
│  Network (Core 0 / network thread)                              │
│                                                                 │
│  port 50000 keep-alive sender  ──► broadcast every 1500ms      │
│                                                                 │
│  port 50002 status receiver:                                    │
│    parse Pitch1, BPM, flags, Mv                                 │
│    if master && playing && Mv==0x8000:                          │
│      tempo_update(track_bpm × (pitch1 / 0x100000))  ──────────►│
│                                                                 │
│  port 50001 beat receiver:                                      │
│    parse beat_in_bar, pitch, device_num                         │
│    if master device:                                            │
│      phase_correction(beat_in_bar)  ──────────────────────────►│
└───────────────────────────────────────────────────────────────┬─┘
                                                                │
                          ┌─────────────────────────────────────▼──┐
                          │  Clock generator (Core 1 / timer thread) │
                          │                                          │
                          │  free-running timer at tick_period_us   │
                          │  fires 24x per beat (24 PPQN)           │
                          │                                          │
                          │  on tempo_update(new_bpm):              │
                          │    tick_period_us = 60M / new_bpm / 24  │
                          │    (immediate, no smoothing)             │
                          │                                          │
                          │  on phase_correction(beat_in_bar):      │
                          │    compute phase error                   │
                          │    apply gentle PLL slew                 │
                          │    update beat_in_bar for bar tracking   │
                          │                                          │
                          │  on each tick:                           │
                          │    send 0xF8 to DIN MIDI                │
                          │    send 0xF8 to USB MIDI host            │
                          └──────────────────────────────────────────┘
```

### Tempo update (from status packets, ~200ms cadence)

```python
def on_status_packet(pkt):
    if not is_master(pkt):        return
    if not is_playing(pkt):       return
    if pkt['mv'] != 0x8000:       return   # no valid BPM

    pitch_mult  = pkt['pitch1'] / 0x100000
    new_bpm     = pkt['track_bpm'] * pitch_mult

    with clock_lock:
        clock.tick_period_us = int(60_000_000 / new_bpm / 24)
        clock.current_bpm    = new_bpm
```

No smoothing on tempo. Apply the new period immediately. The ~200ms cadence is already smooth enough for a drum machine to track without perceiving steps.

### Phase correction (from beat packets, 1× per beat)

```python
def on_beat_packet(pkt):
    if pkt['device_num'] != master_device_num:  return

    with clock_lock:
        # Packet arrival = beat boundary. Compute how far we've drifted into the beat.
        actual_us_into_beat   = clock.tick_in_beat * clock.tick_period_us
        expected_us_into_beat = 0
        clock.phase_error_us  = expected_us_into_beat - actual_us_into_beat
        clock.beat_in_bar     = pkt['beat_in_bar']
```

### PLL timer callback (24 PPQN)

```python
def on_tick():
    midi_send(0xF8)
    clock.tick_in_beat = (clock.tick_in_beat + 1) % 24

    # Apply gentle phase slew — tune gain empirically
    correction            = clock.phase_error_us // 16   # loop gain
    next_interval_us      = clock.tick_period_us + correction
    clock.phase_error_us -= correction

    schedule_next(max(next_interval_us, 1000))   # clamp minimum to 1ms
```

**PLL loop gain `/16`**: Start here. Higher value (e.g. `/8`) = faster phase lock but more jitter. Lower value (e.g. `/32`) = smoother output but slower to correct drift. Tune by ear with the Sub 27 arp running.

**Why phase lock still matters even with fast status updates**: Status packets give you accurate *tempo*, but they don't tell you *where in the beat you are*. Without phase correction, your clock can be perfectly on-tempo but a quarter beat ahead of the XZ's downbeat. The beat packet says "I am at beat 1 right now" — that's the ground truth for phase alignment.

---

## Master device tracking

When only the XDJ-XZ is on the network (single device), all packets come from device number 1 and it's always master. But implement proper master tracking from the start so it works when you eventually add CDJs:

```python
current_master = None   # device_num of whoever is currently master

def update_master(pkt):
    global current_master
    flags = pkt['flags']
    is_playing = (flags >> 6) & 1
    is_master  = (flags >> 5) & 1
    if is_master and is_playing:
        current_master = pkt['device_num']

def is_master(pkt):
    return pkt['device_num'] == current_master
```

Master handoff (when you crossfade between two CDJs) is handled by the status packet flags changing. No special packet type needed to detect it — just track which device currently has the master flag set.

---

## MIDI transport messages

Beyond clock (`0xF8`), implement these:

```python
MIDI_CLOCK    = 0xF8   # 24x per beat, continuous
MIDI_START    = 0xFA   # send when master goes from stopped → playing
MIDI_STOP     = 0xFC   # send when master goes from playing → stopped
MIDI_CONTINUE = 0xFB   # send when master resumes from pause (optional, can use START)
```

Track play state from the `flags` byte in status packets (bit 6 = Playing). Debounce state transitions — the XZ flickers the play flag briefly during cue operations.

Implement a **silence timer**: if no status or beat packets arrive from master for >2 seconds, send `MIDI_STOP` and free-run at last known BPM. Resume with `MIDI_START` when packets return.

---

## Reference packet parsers (verified against real XZ captures)

### Beat packet parser

```python
import struct

MAGIC = b"\x51\x73\x70\x74\x31\x57\x6d\x4a\x4f\x4c"

def parse_beat_packet(payload: bytes) -> dict | None:
    if not payload.startswith(MAGIC):  return None
    if len(payload) < 0x60:            return None
    if payload[0x0A] != 0x28:          return None

    pitch_raw  = struct.unpack(">I", payload[0x54:0x58])[0]
    track_bpm  = struct.unpack(">H", payload[0x5A:0x5C])[0] / 100.0
    pitch_mult = pitch_raw / 0x00100000

    return {
        "type":          "beat",
        "device_num":    payload[0x21],
        "device_name":   payload[0x0B:0x1F].rstrip(b"\x00").decode("ascii", errors="replace"),
        "ms_next_beat":  struct.unpack(">I", payload[0x24:0x28])[0],
        "ms_next_bar":   struct.unpack(">I", payload[0x2C:0x30])[0],
        "pitch_raw":     pitch_raw,
        "pitch_mult":    pitch_mult,
        "pitch_percent": (pitch_mult - 1.0) * 100,
        "track_bpm":     track_bpm,
        "effective_bpm": track_bpm * pitch_mult,
        "beat_in_bar":   payload[0x5C],
    }
```

### Status packet parser

```python
def parse_status_packet(payload: bytes) -> dict | None:
    if not payload.startswith(MAGIC):  return None
    if len(payload) < 0xD4:            return None   # ~208 bytes minimum
    if payload[0x0A] != 0x0A:          return None

    pitch1     = struct.unpack(">I", payload[0x28:0x2C])[0]
    pitch2     = struct.unpack(">I", payload[0x30:0x34])[0]   # local fader, usually ignore
    track_bpm  = struct.unpack(">H", payload[0x5A:0x5C])[0] / 100.0
    flags      = payload[0x89]
    mv         = struct.unpack(">H", payload[0x90:0x92])[0]
    pitch_mult = pitch1 / 0x00100000

    return {
        "type":          "status",
        "device_num":    payload[0x21],
        "device_name":   payload[0x0B:0x1F].rstrip(b"\x00").decode("ascii", errors="replace"),
        "pitch1_raw":    pitch1,
        "pitch2_raw":    pitch2,
        "pitch_mult":    pitch_mult,                          # use this for BPM calculation
        "pitch_percent": (pitch_mult - 1.0) * 100,
        "track_bpm":     track_bpm,
        "effective_bpm": track_bpm * pitch_mult,
        "beat_in_bar":   payload[0xA6],
        "flags":         flags,
        "is_playing":    bool((flags >> 6) & 1),
        "is_master":     bool((flags >> 5) & 1),
        "is_synced":     bool((flags >> 4) & 1),
        "is_on_air":     bool((flags >> 3) & 1),
        "mv":            mv,
        "bpm_valid":     mv == 0x8000,                        # only trust BPM when True
    }
```

---

## Phase 1 deliverable: what to build

A Python project at `~/code/xdj-midi-bridge/` with this structure:

```
xdj-midi-bridge/
├── pyproject.toml
├── bridge.py        ← main entrypoint
├── prolink.py       ← packet parsers + virtual CDJ keep-alive sender
├── clock.py         ← PLL clock generator + MIDI output
├── replay.py        ← pcapng playback for offline dev
└── captures/
    ├── xdj-xz-export-mode.pcapng
    └── xdj-xz-export-mode-pitch-sweep.pcapng
```

### `bridge.py` responsibilities

1. Parse CLI args: `--midi-port`, `--interface`, `--replay <file>`, `--bpm <fallback>`
2. Start the virtual CDJ keep-alive sender (port 50000 broadcast every 1500ms)
3. Open UDP sockets:
   - Bind port 50001 for beat packets
   - Bind port 50002 for status packets
4. Dispatch incoming packets to `prolink.py` parsers
5. Feed parsed data to the clock module:
   - Status packets → `clock.update_tempo(effective_bpm)`
   - Beat packets → `clock.correct_phase(beat_in_bar)`
   - Status packets → master tracking
   - Status packets → play/stop transitions → `clock.start()` / `clock.stop()`
6. Print live status to terminal: BPM, beat-in-bar, phase error, MIDI port, packet counts

### `clock.py` responsibilities

1. Threading: run the tick generator in a `threading.Timer` chain (laptop) or a tight loop with `time.sleep` + drift correction
2. Send `0xF8` on every tick
3. Send `0xFA`/`0xFC`/`0xFB` on state transitions
4. Expose `update_tempo(bpm)` — immediate period update, thread-safe
5. Expose `correct_phase(beat_in_bar)` — updates `phase_error_us`, thread-safe
6. MIDI output via `mido` + `python-rtmidi`

### `replay.py` responsibilities

Reads a `.pcapng` file and re-emits the UDP payloads to `localhost:50001` and `localhost:50002` at the original inter-packet timing. This lets `bridge.py` run completely offline against captured data.

```bash
# Two terminal windows:
python replay.py --file captures/xdj-xz-export-mode-pitch-sweep.pcapng --loop
python bridge.py --interface lo --midi-port "your-interface"
```

This is the primary development loop before the XZ needs to be powered on.

### Dependencies

```toml
[project]
dependencies = [
    "mido>=1.3",
    "python-rtmidi>=1.5",
    "scapy>=2.5",     # for pcapng replay
]
```

---

## Reference libraries to consult

| Library | Language | What to look at |
|---|---|---|
| [python-prodj-link](https://github.com/flesniak/python-prodj-link) | Python | `midiclock.py`, `vcdj.py` — virtual CDJ announce, `client.bpm * client.actual_pitch` pattern |
| [beat-link](https://github.com/Deep-Symmetry/beat-link) | Java | `VirtualCdj.java` — status packet parsing, `tempoChanged()` callback pattern, Pitch1/Pitch2 distinction |
| [prolink-cpp](https://github.com/grantHarris/prolink-cpp) | C++17 | Closest to eventual ESP32 firmware — beat clock, follow_master mode, status packet field extraction |
| [cardinia](https://github.com/nudge/cardinia) | C/C++ embedded | Shipped hardware product doing exactly this — read firmware for enclosure/hardware validation |
| [DJ Link Ecosystem Analysis](https://djl-analysis.deepsymmetry.org/djl-analysis/) | Docs | Ground truth for all packet formats |

Most important single reference for the dual-source architecture: beat-link's `VirtualCdj.java` — specifically how `tempoChanged()` is fired from status packet Pitch1 updates, and how beat arrival only triggers phase correction.

---

## Hardware path (after Phase 1 validates)

**Phase 2**: Port Python bridge to ESP32-S3 firmware (C++ with ESP-IDF or Arduino + PlatformIO)
- W5500 SPI Ethernet module (~$8) for Pro DJ Link network
- Core 0: network tasks (UDP receive, virtual CDJ keep-alive, packet parsing)
- Core 1: uClock library (`midilab/uClock`) in EXTERNAL_CLOCK mode — handles hardware-timer-driven 24 PPQN with phase lock. Call `uClock.clockMe()` on each status packet tempo update and `uClock.adjustPhase()` on beat packets.
- DIN MIDI out: UART TX → 220Ω → DIN pin 5; DIN pin 4 → 220Ω → 3.3V

**Phase 3**: USB MIDI host to OP-XY
- ESP32-S3 native USB OTG running TinyUSB in host mode
- OP-XY is USB MIDI class-compliant, no special driver needed
- `sauloverissimo/ESP32_Host_MIDI` library handles USB host + DIN UART simultaneously

**Phase 4**: Polish
- OLED display: current BPM, master device, beat-in-bar indicator
- Downbeat LED (blink on beat_in_bar == 1)
- Tap-tempo button for fallback when no valid BPM (`Mv != 0x8000`)
- Enclosure: Hammond 1593 or custom laser-cut (you have a laser cutter)

---

## Known hard constraints

- **Export mode only**: XZ disconnects from Pro DJ Link in Performance mode (when tethered to rekordbox on a laptop). Not a problem for standalone jamming.
- **rekordbox-analyzed tracks only for valid BPM**: Check `Mv == 0x8000` before trusting BPM. Freeze last known good BPM for unanalyzed sources.
- **Pitch range is WIDE**: Handle full uint32 pitch range. Observed range: −70% to +100%. Don't assume ±10%.
- **Virtual CDJ required for status packets**: Beat packets arrive without it. Status packets (needed for smooth tempo tracking) require keep-alive announcement. Use device number 7 to avoid conflicting with CDJ deck numbers 1–4.
- **Pitch1 not Pitch2**: Always read `Pitch1` at `0x28` in status packets. `Pitch2` at `0x30` is the physical fader position only and diverges from actual effective pitch when in sync mode.

---

## Captures available for offline development

Located in `captures/` in the project directory:

- `xdj-xz-export-mode.pcapng` — 297 beat packets, steady 132 BPM, ~134 seconds, no pitch movement. Good for baseline PLL tuning.
- `xdj-xz-export-mode-pitch-sweep.pcapng` — 144 beat packets, 136 BPM track, pitch swept across ±6%, ±10%, ±16%, and WIDE range modes (observed −70% to +100%), ~60 seconds. Good for testing tempo tracking responsiveness.

Note: neither capture contains port 50002 status packets because no virtual CDJ was present during capture. To test the dual-source architecture end-to-end you'll need the XZ running with the virtual CDJ keep-alive active. The replay tool is useful for beat-packet-only testing in the meantime.

---

## What to build first (in order)

1. `replay.py` — get this working first so all further development is offline
2. `prolink.py` — both parsers, tested against replay
3. `clock.py` — PLL tick generator, tempo update, phase correction, MIDI output. Test with a constant-BPM replay, confirm Sub 27 locks.
4. Integrate keep-alive sender into `bridge.py`
5. Run against live XZ — confirm status packets arrive, confirm smooth tempo tracking during pitch sweep
6. Tune PLL loop gain by ear (start at `/16`, try `/8` and `/32`)