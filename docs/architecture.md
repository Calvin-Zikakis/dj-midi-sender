# Architecture

> **Status and hardware:** see [../ROADMAP.md](../ROADMAP.md) for what is built
> and [hardware.md](hardware.md) for the board and wiring. This doc is the
> protocol/PLL reference and stays accurate; it does not track day-to-day state.

This document captures the Pro DJ Link wire format, the dual-source clock
design, and the C++ module boundaries. Everything below is verified
empirically against the captures in [../captures/](../captures/) — not
just lifted from upstream documentation. Where a field is documented from
upstream sources but not present in our captures (status packets), the
source is named inline.

## Pro DJ Link on the XDJ-XZ

The XZ broadcasts Pro DJ Link traffic on the link-local network as soon as
a track is playing on the master deck in **Export mode**. No virtual CDJ
announce is required to *receive* beat packets — the XZ transmits
unsolicited. Status packets (port 50002) are different: they are sent
**unicast** to whoever announced themselves as a CDJ on port 50000.

| Port  | Direction      | Type                          | Cadence         | Triggered by |
|-------|----------------|-------------------------------|-----------------|--------------|
| 50000 | XZ → broadcast | `0x06` keep-alive announce    | ~500 ms         | always       |
| 50001 | XZ → broadcast | `0x28` beat packets           | one per beat    | always       |
| 50002 | XZ → unicast   | `0x0A` status packets         | ~200 ms         | only after we announce a virtual CDJ |

Source addressing observed: an RFC 3927 link-local IP (`169.254.0.0/16`) and
the mixer's hardware MAC; destination `169.254.255.255` for broadcast traffic.

### Mode requirements

- **Export mode** (XZ playing from USB stick): Pro DJ Link active. **Required.**
- **Performance mode** (USB to rekordbox on laptop): XZ drops off Pro DJ Link
  entirely. Not viable for this project.
- **Unanalyzed audio** (raw MP3 with no rekordbox beat grid): the `Mv` field
  in status packets is not `0x8000`, so the BPM is not trusted. Tap-tempo
  fallback is on the Phase-4 roadmap.

## Beat packet — port 50001, type `0x28`

All offsets are relative to the start of the UDP payload. Parsed by
`parse_beat_packet()` in [`../lib/prolink/packets.cpp`](../lib/prolink/packets.cpp).

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
| `0x54` | 4 B  | Pitch (raw) | uint32 BE, see below |
| `0x58` | 2 B  | reserved | typically `0x0000` |
| `0x5A` | 2 B  | Track BPM × 100 | uint16 BE |
| `0x5C` | 1 B  | Beat-within-bar | 1–4 |
| `0x5D` | 1 B  | Device number (echo) | |

### Beat packet timing characteristics

Measured from `xdj-xz-export-mode.pcapng`:

- Track BPM 132 → expected inter-beat interval 454.5 ms.
- Observed range: **452.6–455.9 ms**, jitter ≈ ±2 ms.
- Occasional missed/coalesced packets (one ~934 ms gap = two intervals
  fused). The clock engine must free-run through gaps.

## Status packet — port 50002, type `0x0A`

**Not present in our captures.** Status packets are sent unicast only after
a device announces itself on port 50000. The field offsets below come from
[deep-symmetry's DJ Link analysis](https://djl-analysis.deepsymmetry.org/djl-analysis/)
and the
[`flesniak/python-prodj-link`](https://github.com/flesniak/python-prodj-link)
+ [`grantHarris/prolink-cpp`](https://github.com/grantHarris/prolink-cpp)
implementations.

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| `0x00` | 10 B | Magic header | same as beat packet |
| `0x0A` | 1 B  | Packet type | `0x0A` for status |
| `0x0B` | 20 B | Device name | null-padded ASCII |
| `0x21` | 1 B  | Device number | |
| `0x88` | 2 B  | `state` (uint16 BE; `0x84` always set + flags) | flag bits live in the low byte at `0x89` |
| `0x89` | 1 B  | Flags (low byte of state) | bit 6 = Playing, bit 5 = Master, bit 4 = Sync, bit 3 = On-Air |
| `0x8C` | 4 B  | **`Pitch1`** (effective pitch) | uint32 BE — what the player communicates as its actual playing tempo. **Use this.** |
| `0x90` | 2 B  | `Mv` validity / BpmState | uint16 BE; `0x8000` = rekordbox, `0x7FFF` = unanalyzed |
| `0x92` | 2 B  | Track BPM × 100 | uint16 BE |
| `0x98` | 4 B  | `Pitch2` (local fader only) | uint32 BE — diverges from Pitch1 in sync mode. **Ignore.** |
| `0xA6` | 1 B  | Beat-within-bar | 1–4 |

Approximate full packet size: ~208 bytes (varies by player model; XDJ-XZ
is consistent with the XDJ-1000 layout from python-prodj-link).

Sources cross-checked: [Deep Symmetry beat-link `CdjStatus.java`](https://github.com/Deep-Symmetry/beat-link/blob/master/src/main/java/org/deepsymmetry/beatlink/CdjStatus.java),
[python-prodj-link `packets.py`](https://github.com/flesniak/python-prodj-link/blob/master/prodj/network/packets.py)
(`StatusPacket` struct, fields `physical_pitch` / `actual_pitch` / `bpm_state` / `bpm` / `state` / `beat`),
[Deep Symmetry DJL Analysis](https://djl-analysis.deepsymmetry.org/djl-analysis/vcdj.html).
Earlier revisions of this doc carried offsets from an unverified source that
were ~104 bytes off — fixed once Phase 1 saw live status packets and the
discrepancy showed up in the GUI.

### Pitch1 vs Pitch2 — the critical distinction

When a CDJ is in **sync mode** following a master, `Pitch1` tracks the
master's effective tempo and `Pitch2` stays pinned to wherever the local
operator left the pitch fader. They diverge. Reading the wrong one means
the bridge follows the slider on the *follower* CDJ instead of the master.

Always read `Pitch1` at `0x8C`. `Pitch2` at `0x98` exists in our parser only
so the field is documented; it is never consumed.

### `Mv` validity

| Value     | Meaning |
|-----------|---------|
| `0x8000`  | Rekordbox-analyzed track loaded → BPM valid |
| `0x7FFF`  | No track loaded |
| `0x0000`  | Non-rekordbox / unanalyzed source → BPM invalid, freeze last good tempo |

Only trust the BPM and apply tempo updates when `Mv == 0x8000`.

## Pitch encoding (the gotcha)

The 32-bit pitch field — same encoding in both beat and status packets —
encodes a *multiplier*, not a percentage. `0x00100000` (= 1 048 576)
represents 1.0×. To decode:

```cpp
float multiplier  = static_cast<float>(raw) / static_cast<float>(0x00100000u);
float pitch_pct   = (multiplier - 1.0f) * 100.0f;
float effective   = (bpm_x100 / 100.0f) * multiplier;
```

Verified reference points across the XZ's pitch range modes:

| Raw          | Multiplier | %       | Notes |
|--------------|------------|---------|-------|
| `0x00100000` | 1.000      | 0 %     | resting |
| `0x0010F5C2` | 1.060      | +6.00 % | top of standard ±6 % |
| `0x000F0A3D` | 0.940      | −6.00 % | bottom of standard ±6 % |
| `0x00128F5C` | 1.160      | +16.0 % | ±16 % range max |
| `0x00200000` | 2.000      | +100 %  | max WIDE up |
| `0x0004CCCC` | 0.300      | −70 %   | observed minimum |

Range observed in the pitch-sweep capture: **−70 % to +100 %**. Decoders
must accept the full `uint32` range — the `int8 percent` field encodings
used by some Pioneer protocol docs do **not** apply here.

The `BPM × 100` field at offset `0x5A` is the track's **native** BPM and
never changes with pitch. Effective playing BPM must always be computed:

```
effective_bpm = track_bpm × pitch_multiplier
```

In `xdj-xz-export-mode-pitch-sweep.pcapng`, the BPM field stayed at 136.00
across all 144 packets; inter-packet arrival times matched
`60_000 / (136 × multiplier)` ms.

## Virtual CDJ announcement

To unlock status packets, the bridge must announce itself as a virtual CDJ
by broadcasting type-`0x06` keep-alive packets to UDP port 50000 every
~1500 ms. The XZ then begins unicasting status packets to the bridge's IP
on port 50002.

Keep-alive packet (54 bytes total — mirror the XZ's own keep-alive from the
capture, substituting the bridge's MAC and IP):

| Offset | Size | Field |
|--------|------|-------|
| `0x00` | 10 B | Magic header |
| `0x0A` | 1 B  | Type: `0x06` |
| `0x0B` | 20 B | Device name (null-padded ASCII, e.g. `xdj-bridge`) |
| `0x1F` | 1 B  | Subtype: `0x01` |
| `0x20` | 1 B  | Subtype echo: `0x02` |
| `0x21` | 1 B  | Device number: `0x07` (safe — CDJ decks are 1–4, mixers 5–6) |
| `0x22` | 2 B  | Packet length: `0x0036` |
| `0x24` | 6 B  | MAC address |
| `0x2A` | 4 B  | IP address |
| `0x2E` | 8 B  | Padding / flags (zeros) |

Reference implementations: `Session::SendAnnounce()` in
[`grantHarris/prolink-cpp`](https://github.com/grantHarris/prolink-cpp);
`vcdj.py` in [`flesniak/python-prodj-link`](https://github.com/flesniak/python-prodj-link).

Note: `lib/prolink/bridge.cpp` builds the keep-alive packet but only the
desktop layer can fill in the bridge's actual MAC and IP. Both are passed
into `Bridge::set_local_iface(mac, ip)` before `run()` is called.

## Dual-source clock architecture

The reason for caring about status packets at all: tempo update latency
during a pitch-slider sweep. With beat-packets-only, the next tempo update
arrives at the next beat boundary — up to ~500 ms at 130 BPM, audibly
stepped. Status packets arrive every ~200 ms and carry the live `Pitch1`,
so tempo follows the slider continuously.

`tempoChanged()` in beat-link's `VirtualCdj.java` is the canonical
reference — it fires from status packet `Pitch1` updates multiple times
per second during a sweep. Beat arrivals only trigger phase correction.

```mermaid
flowchart TD
    KA["keep-alive sender -> broadcast :50000 every 1500 ms"]
    ST["status receiver :50002"]
    BT["beat receiver :50001"]
    ST -->|"Pitch1, BPM, flags, Mv"| MT["master tracking"]
    MT -->|"if master and playing and Mv ok"| UT["clock.update_tempo(bpm x pitch1)"]
    ST -->|"play/stop transition"| SS["clock.start / clock.stop"]
    BT -->|"if master device"| CP["clock.feed_beat (phase only)"]
    UT --> CLK["Clock engine (24 PPQN)"]
    SS --> CLK
    CP --> CLK
    CLK -->|"0xF8 ticks"| OUT["MidiFanOut -> USB host + DIN"]
```

The pseudocode below shows the same flow in detail.

```
┌──────────────────────────────────────────────────────────────────┐
│  Network thread                                                  │
│                                                                  │
│  keep-alive sender → broadcast port 50000 every 1500 ms          │
│                                                                  │
│  port 50002 status receiver:                                     │
│    parse Pitch1, BPM, flags, Mv, device_num                      │
│    update master tracking                                        │
│    if master && playing && Mv == 0x8000:                         │
│      clock.update_tempo(bpm × pitch1_mult)  ─────────────────►  │
│    on play/stop transition:                                      │
│      clock.start() / clock.stop()             ───────────────►  │
│                                                                  │
│  port 50001 beat receiver:                                       │
│    parse beat_in_bar, device_num                                 │
│    if master device:                                             │
│      clock.correct_phase(beat_in_bar)         ───────────────►  │
└──────────────────────────────────────────────────────────────────┤
                                                                   │
         ┌─────────────────────────────────────────────────────────▼─┐
         │  Clock engine — lib/prolink/clock.hpp                      │
         │                                                            │
         │  ITimer fires every tick_period_us (24 PPQN)               │
         │                                                            │
         │  update_tempo(bpm):                                        │
         │    tick_period_us = 60_000_000 / bpm / 24                 │
         │    (immediate, no smoothing — 200 ms cadence is enough)    │
         │                                                            │
         │  correct_phase(beat_in_bar):                               │
         │    phase_error_us = -(tick_in_beat * tick_period_us)      │
         │    bar_position = beat_in_bar                              │
         │                                                            │
         │  on_tick():                                                │
         │    midi.send_byte(0xF8)                                    │
         │    tick_in_beat = (tick_in_beat + 1) % 24                 │
         │    correction = phase_error_us / gain                      │
         │    schedule_next(tick_period_us + correction)              │
         │    phase_error_us -= correction                            │
         └────────────────────────────────────────────────────────────┘
```

### Why phase correction still matters with fast tempo updates

Status packets give accurate *tempo* but not *position*. Without beat
packets nudging phase, the clock will end up perfectly on-tempo but a
quarter beat ahead of (or behind) the master's downbeat after enough
drift. Beat packets carry `beat_in_bar` — that's the ground truth for
phase alignment.

### PLL gain

`phase_error_us / 16` per tick is the starting gain. Higher (`/8`) =
faster phase lock, more jitter passthrough. Lower (`/32`) = smoother,
slower to correct drift. Tune by ear with the OP-XY running.

### Implementation refinement (2026-06-02): continuous-time phase

The pseudocode above shows the original `correct_phase(beat_in_bar)`, which
measured phase by the integer `tick_in_beat` index — i.e. resolved only to the
nearest whole tick (~20 ms at 120 BPM), a real jitter source. The shipping
clock instead uses `Clock::feed_beat()`: it timestamps tick 0 and the beat
packet via `ITimer::now_us()` and computes the lead in **continuous
microseconds**. Same intent (drive our tick-0 lead toward `offset_us`, which
stays a constant ms lead → tempo-independent), far finer resolution. On the
firmware the `esp_timer` shim (`TimerEsp`) also arms each tick to a cumulative
absolute deadline so dispatch latency doesn't accumulate. `ms_next_beat` is
parsed and reserved for a future predictive (jitter-rejecting) refinement but
isn't steered on yet.

## Lead-time compensation (two-axis offset)

The slave's first sound lags our tick by a chain of physical delays (USB
transit, OP-XY internal scheduling). Total is on the order of 10–30 ms
and is **constant in milliseconds** — it does not scale with tempo. The
scheduler compensates by firing tick 0 of each beat `offset_us` *before*
the predicted beat boundary, where `offset_us` is the sum of two
independent knobs:

| Axis | What it compensates | Scope | Persistence |
|------|---------------------|-------|-------------|
| **Clock offset** | USB / slave physical processing latency | per output port | `~/.config/dj-midi-sender.json` ([`desktop/config_posix.cpp`](../desktop/config_posix.cpp)) |
| **Grid offset**  | rekordbox beat-grid not lining up with where the kick actually lives | per track / per session | never persisted |

Rationale for splitting: clock latency is set-and-forget for a given
device, but grid skew varies per track. Mixing them into one knob means
re-tuning the persisted value every time the next track has a different
feel. Keeping them separate means the persisted clock offset stays
correct across tracks and the user only ever fiddles with the grid
offset between tracks.

Both axes live on `Bridge` ([`bridge.hpp`](../lib/prolink/bridge.hpp)).
The bridge pushes their sum into the clock via
`IClockSink::set_offset_us`. The clock applies the delta to
`phase_error_us` immediately scaled by the PLL gain, so a 1 ms nudge in
the GUI is audible within one tick rather than over the next ~200 ms.

### Phase correction with the offset

The `correct_phase()` math becomes:

```cpp
int32_t err = -(t * period) - offset_us;       // wrap to ±half a beat
```

Where `t` is the next pending tick index. With `offset = 0` this is the
no-offset case (catch up by `t` ticks). With `offset > 0` (lead), the
target moves earlier, so `err` becomes more negative.

## Master device tracking

The master flag is in status-packet flags (bit 5). Track which device
currently has the bit set:

```cpp
if (status.is_master() && status.is_playing()) {
    current_master_ = status.device_num;
}
bool is_from_master = (pkt.device_num == current_master_);
```

When only the XDJ-XZ is on the network, all packets come from device 1
and it is always master. This logic also handles future multi-deck setups
without changes — master handoff happens by the flag flipping between two
CDJ status streams.

## Start / Stop semantics

- **MIDI Start (`0xFA`)** is sent when the master transitions
  from stopped → playing AND the next beat packet carries `beat_in_bar == 1`.
  This holds first sound until the slave's bar 1 aligns with the master's.
- **MIDI Stop (`0xFC`)** is sent when:
  - The master transitions playing → stopped (status packet flag drops), OR
  - No status or beat packets arrive from the master for >2 seconds.
- **Play-state debounce**: the XZ flickers the play flag briefly during cue
  scrubbing. Require >100 ms of stable state before reacting.

Restart after a stop is a fresh `Start` on the next downbeat — no MIDI
**Continue** is used.

### Manual re-sync

MIDI clock carries tempo but not bar position, so a slave whose transport is
stopped/started from its own front panel keeps the tempo but loses bar
alignment with the master. `Bridge::request_resync()` (front-panel tap)
re-emits a Stop+Start to snap it back:

- **Deferred (sync mode):** the Stop+Start fires on the master's next downbeat
  (`beat_in_bar == 1`), reusing the bar-slip realign path, so the slave's bar 1
  realigns with the master's.
- **Immediate (free/standalone, or when no master beats are arriving):** the
  Stop+Start fires right away, since there is no master downbeat to wait for.

The request is a small atomic set from the UI thread and consumed on the bridge
thread — deferred requests in `handle_beat_packet`, immediate ones in the run
loop (`maybe_resync`).

## Module boundaries — lib/prolink

The core is pure C++17 with no platform headers beyond `<cstdint>`,
`<cstring>`, `<functional>`, and `<optional>`. All I/O is injected.

```cpp
// lib/prolink/bridge.hpp
class IUdpSocket {
public:
    virtual int  recv(uint8_t* buf, size_t len, uint32_t timeout_ms) = 0;
    virtual bool send(const uint8_t* buf, size_t len,
                      uint32_t ip_be, uint16_t port) = 0;
    virtual ~IUdpSocket() = default;
};

class IMidiOut {
public:
    virtual void send_byte(uint8_t byte) = 0;
    virtual ~IMidiOut() = default;
};

class ITimer {
public:
    virtual void set_interval_us(uint32_t interval_us,
                                 std::function<void()> on_tick) = 0;
    virtual void cancel() = 0;
    virtual ~ITimer() = default;
};
```

| Concern      | Desktop (Phase 1)                              | Firmware (Phase 2+)                            |
|--------------|------------------------------------------------|------------------------------------------------|
| `IUdpSocket` | `UdpPosix` — BSD sockets                       | `UdpW5500` — lwIP over SPI                     |
| `IMidiOut`   | `MidiRtMidi` — CoreMIDI → OP-XY (USB MIDI)     | `MidiUart` (DIN) + `MidiUsbHost` (TinyUSB)     |
| `ITimer`     | `TimerPosix` — `std::thread` + `sleep_until`   | replaced by [`uClock`](https://github.com/midilab/uClock) — hardware timer + 24 PPQN |

### Phase 1 MIDI path: OP-XY via the Mac's USB

The Phase-1 slave is the **Teenage Engineering OP-XY**, connected to the
Mac with a USB-C cable. The OP-XY is USB MIDI class-compliant — macOS
exposes it as a CoreMIDI output port automatically, and RtMidi opens it
the same way it opens any other MIDI port (`--midi-port "OP-XY"` or
`--list-midi` to see the exact string). No DIN, no USB-MIDI-to-DIN
adapter, no host-mode anything.

This is a deliberate Phase 1 / Phase 2 split. Phase 1 validates the
network-side logic (parsers + dual-source PLL + virtual CDJ + master
tracking) by leveraging macOS's USB MIDI stack. Phase 2 replaces that
stack with the ESP32-S3's TinyUSB host mode wrapped by ESP32_Host_MIDI,
keeping the OP-XY as the same target — only the host changes.

The Sub 27 (DIN MIDI) is a Phase-2 target — it needs the firmware's UART
DIN output. We're not testing DIN MIDI in Phase 1.

### Firmware variation — no `ITimer`

The firmware doesn't actually use `ITimer`. uClock owns the hardware
timer and the bridge feeds it `setTempo(bpm)` + `clockMe()` callbacks.
The core exposes both hooks (a `Clock` class that consumes `ITimer`,
*and* a callback-style `IClockSink` interface) so the same `Bridge`
works either way.

### Why C++17 (not Python, not Rust)

- **Code reuse is real**: `lib/prolink/` compiles identically on macOS and
  ESP32-S3. No rewrite between Phase 1 and Phase 2.
- **The ESP32 ecosystem is C++ native**: uClock, ESP32_Host_MIDI, TinyUSB,
  W5500 drivers are all PlatformIO/Arduino C++ libraries.
- **USB MIDI host is solved in C++**: ESP32_Host_MIDI wraps TinyUSB host
  mode. In Rust, this is unsolved.
- **`prolink-cpp` is a direct reference**: same language, adapt directly.
- **`cardinia`** (a shipped commercial product doing exactly this) is C++.

The previous plan had a Python prototype as a throwaway de-risking step.
Switching to C++ means Phase 1 *is* the firmware — every line written in
`lib/prolink/` ships in the final box.

## Designing for the microcontroller

The deliverable is firmware, not the desktop binary. Every design decision
in `lib/prolink/` was made with the firmware target in mind:

| Desktop                                      | Firmware                                                       |
|----------------------------------------------|----------------------------------------------------------------|
| `std::thread` + `sleep_until` (TimerPosix)   | uClock — ESP32 hardware timer ISR, µs-resolution               |
| BSD UDP socket                               | lwIP raw UDP callback on W5500                                 |
| RtMidi → CoreMIDI → OP-XY (USB)              | UART TX for DIN + TinyUSB MIDI host for USB-A                  |
| `float` periods                              | `uint32_t` microseconds (no FPU needed in firmware)            |
| `~/.config/dj-midi-sender.json` for offset   | NVS / EEPROM region                                            |

The scheduler is integer-friendly end-to-end: tempo in milli-BPM,
`tick_period_us` derived as `60_000_000 / milli_bpm / 24 * 1000`,
phase error in microseconds, single signed subtract per beat. No
floating-point necessary on the microcontroller.
