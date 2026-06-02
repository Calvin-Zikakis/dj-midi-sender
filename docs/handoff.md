# XDJ-XZ → MIDI Clock Bridge: Project Handoff v3

> **This is the original pre-build planning doc — kept for the research/protocol
> background.** For current state see [session-notes.md](session-notes.md) (live
> status + next steps), [xdj-midi-bridge-context-v4.md](xdj-midi-bridge-context-v4.md)
> (hardware addendum), and [phases.md](phases.md). As of 2026-06-01 the box runs
> the full pipeline on a Waveshare ESP32-S3-ETH and clocks a Sub 25 / OP-XY over
> a USB-A host jack; only that jack is wired (DIN out + OLED/encoder/buttons are
> still to come).

**Author**: Calvin (staff backend engineer, Go/Python/Rust)
**Language**: C++17 throughout — desktop prototype and ESP32-S3 firmware share a core library
**Status**: Research complete, ready to build
**Last updated**: v3 — switched from Python prototype to C++17 unified codebase

---

## The goal

Build a standalone hardware bridge that listens to a Pioneer XDJ-XZ's Pro DJ Link Ethernet
broadcast, extracts the master deck's live tempo and beat phase, and outputs rock-solid MIDI
clock that tracks tempo changes in real time — including smooth pitch slider sweeps — to:

- **Moog Subsequent 27** → DIN MIDI in
- **Teenage Engineering OP-XY** → USB MIDI host

The XDJ-XZ has no native MIDI clock output. Pro DJ Link over Ethernet is the only path.

**End-state hardware**: ESP32-S3 + W5500 SPI Ethernet + DIN MIDI out + USB MIDI host, enclosed.

---

## Why C++ (not Python, not Rust)

The previous plan used Python for Phase 1. Switching to C++17 for all phases because:

- **Code reuse is real**: the core logic library (`lib/prolink/`) compiles identically on macOS
  and ESP32-S3. No port, no rewrite between phases — just swap the I/O layer.
- **The ESP32 ecosystem is C++ native**: every relevant library (uClock, ESP32_Host_MIDI,
  TinyUSB, W5500 drivers) is a PlatformIO/Arduino C++ library. Drop-in, no FFI.
- **USB MIDI host (OP-XY) is solved in C++**: TinyUSB host mode wrapped by ESP32_Host_MIDI.
  This is the hardest piece and it's a library import in C++. In Rust it's unsolved.
- **`prolink-cpp` exists**: `grantHarris/prolink-cpp` is a C++17 library implementing beat
  packet parsing, status packet parsing, virtual CDJ announce, device discovery, tempo master
  tracking, and follow-master mode. It's the direct reference — read and adapt, don't port.
- **Cardinia** (the shipped commercial product doing exactly this) is C++.

Rust would be viable but adds friction specifically around TinyUSB host mode and the absence
of equivalents to uClock and ESP32_Host_MIDI. For a project where the end target is embedded
C++ ecosystem hardware, C++ is the right call.

---

## Project structure (all phases, one CMake workspace)

```
xdj-midi-bridge/
├── CMakeLists.txt              # workspace root
├── lib/
│   └── prolink/                # ← THE CORE: compiles on macOS AND ESP32-S3 unchanged
│       ├── CMakeLists.txt
│       ├── packets.hpp         # packet structs + parsers (no I/O)
│       ├── packets.cpp
│       ├── clock.hpp           # PLL clock engine (no I/O, platform timer injected)
│       ├── clock.cpp
│       ├── bridge.hpp          # orchestration: master tracking, state machine
│       ├── bridge.cpp
│       └── types.hpp           # shared types, constants, MAGIC header
├── desktop/                    # Phase 1 — macOS/Linux binary
│   ├── CMakeLists.txt
│   ├── main.cpp                # entry point, CLI args
│   ├── udp_posix.hpp/cpp       # POSIX UDP socket implementation
│   ├── midi_rtmidi.hpp/cpp     # RtMidi MIDI output implementation
│   ├── timer_posix.hpp/cpp     # std::thread timer implementation
│   └── replay.cpp              # pcapng replay tool (offline dev)
├── firmware/                   # Phase 2/3 — ESP32-S3
│   ├── CMakeLists.txt          # or platformio.ini
│   ├── main.cpp                # ESP-IDF app_main, task creation
│   ├── udp_w5500.hpp/cpp       # W5500 SPI UDP implementation
│   ├── midi_uart.hpp/cpp       # UART DIN MIDI implementation
│   ├── midi_usb_host.hpp/cpp   # TinyUSB host MIDI implementation
│   └── timer_esp.hpp/cpp       # esp_timer hardware timer implementation
└── captures/
    ├── xdj-xz-export-mode.pcapng
    └── xdj-xz-export-mode-pitch-sweep.pcapng
```

### How code reuse actually works

`lib/prolink/` is pure C++17 with no system headers beyond `<cstdint>`, `<cstring>`,
`<functional>`, and `<optional>`. It compiles on any target. All I/O is injected via
abstract interfaces:

```cpp
// lib/prolink/bridge.hpp

class IUdpSocket {
public:
    virtual int  recv(uint8_t* buf, size_t len, uint32_t timeout_ms) = 0;
    virtual bool send(const uint8_t* buf, size_t len,
                      uint32_t ip, uint16_t port) = 0;
    virtual ~IUdpSocket() = default;
};

class IMidiOut {
public:
    virtual void send_byte(uint8_t byte) = 0;
    virtual ~IMidiOut() = default;
};

class ITimer {
public:
    // Call callback every interval_us microseconds
    virtual void set_interval(uint32_t interval_us,
                               std::function<void()> callback) = 0;
    virtual ~ITimer() = default;
};

class Bridge {
public:
    Bridge(IUdpSocket& beat_sock,
           IUdpSocket& status_sock,
           IUdpSocket& keepalive_sock,
           IMidiOut&   midi,
           ITimer&     tick_timer);

    void run();  // call from network task / main loop
    void tick(); // called by ITimer at 24 PPQN — sends 0xF8
};
```

On macOS: `UdpPosix : IUdpSocket`, `MidiRtMidi : IMidiOut`, `TimerPosix : ITimer`.
On ESP32-S3: `UdpW5500 : IUdpSocket`, `MidiUart : IMidiOut`, `TimerEsp : ITimer`.

`lib/prolink/` never changes between phases. You're not porting logic — you're plugging
in platform sockets and timers.

---

## What has been empirically verified

Two Wireshark pcapng captures were taken of the XZ in Export mode with a rekordbox-analyzed
track playing. Both were fully parsed with Python/tshark. Everything below is confirmed
against real hardware data.

### Beat packets — port 50001

The XZ broadcasts beat packets **natively without a virtual CDJ**. One packet per beat.

**Packet structure** (all offsets from start of UDP payload):

```
0x00–0x09  magic: 51 73 70 74 31 57 6d 4a 4f 4c  ("Qspt1WmJOL")
0x0A       type: 0x28
0x0B–0x1F  device name, ASCII null-padded
0x20       device subtype
0x21       device number (1–4)
0x22–0x23  payload length (uint16 BE)
0x24–0x27  ms to next beat (uint32 BE) — at normal pitch, scale by multiplier
0x28–0x2B  ms to 2nd beat
0x2C–0x2F  ms to next bar (downbeat)
0x30–0x33  ms to 4th beat
0x34–0x37  ms to 2nd bar
0x38–0x3B  ms to 8th beat
0x3C–0x53  reserved / 0xFF padding
0x54–0x57  pitch (uint32 BE) — ACTUAL effective pitch
0x58–0x59  reserved
0x5A–0x5B  track BPM × 100 (uint16 BE)  e.g. 0x3520 = 136.00 BPM
0x5C       beat-within-bar (1–4)
0x5D       device number (redundant)
```

**Verified timing**: ±2ms jitter at constant pitch. One gap of 934ms observed during pitch
sweep — PLL must free-run robustly when packets are late.

### Pitch encoding (confirmed across all slider ranges)

```cpp
// lib/prolink/packets.cpp
static constexpr uint32_t PITCH_UNITY = 0x00100000u;  // 1.0x = 0%

float decode_pitch_multiplier(uint32_t raw) {
    return static_cast<float>(raw) / static_cast<float>(PITCH_UNITY);
}

float effective_bpm(uint16_t track_bpm_x100, uint32_t pitch_raw) {
    return (track_bpm_x100 / 100.0f) * decode_pitch_multiplier(pitch_raw);
}
```

Confirmed reference values:
```
0x00100000 = 1.000x =   0.00%
0x0010F5C2 = 1.060x =  +6.00%  (standard range max)
0x000F0A3D = 0.940x =  -6.00%
0x00128F5C = 1.160x = +16.00%
0x00200000 = 2.000x = +100.00% (WIDE max)
0x0004CCCC = 0.300x =  -70.00% (WIDE min observed)
```

Handle the full uint32 range. Do not assume ±10%.
BPM field is the **track's native BPM, never changes with pitch**. Always compute
`effective_bpm = track_bpm × pitch_multiplier`.

### Status packets — port 50002

Not seen in captures (no virtual CDJ was present). Require virtual CDJ announce to unlock.
These are the critical second data source — see dual-source architecture below.

---

## Why dual-source (status + beat) is required

**Beat-packet-only**: tempo update latency ~500ms during a slider sweep. Drum machine hears
a BPM snap at the next beat boundary.

**Dual-source** (what CDJ sync followers actually do):
- Status packets (~200ms cadence) carry live `Pitch1` → update tempo immediately
- Beat packets (1× per beat) → phase correction only
- Result: ~200ms tempo update latency, smooth sweeps, indistinguishable from native CDJ sync

beat-link confirms: `tempoChanged()` fires from status packet `Pitch1` updates, multiple
times per second during a sweep. Beat arrivals only trigger phase correction.

---

## Status packet structure — port 50002

Type `0x0A`, ~208 bytes. Sent unicast to your virtual CDJ's IP once you announce.

```
0x00–0x09  magic
0x0A       type: 0x0A
0x0B–0x1F  device name
0x21       device number
0x28–0x2B  Pitch1 (uint32 BE) ← USE THIS. Actual effective pitch.
                                  Tracks master when in sync mode.
0x30–0x33  Pitch2 (uint32 BE)    Local fader position only. IGNORE.
                                  Diverges from Pitch1 when player is synced.
0x5A–0x5B  track BPM × 100 (uint16 BE)
0x89       flags:
             bit 6 = Playing
             bit 5 = Master
             bit 4 = Sync
             bit 3 = On-Air
             bit 1 = BPM-only sync (beat alignment lost)
0x90–0x91  Mv (uint16 BE):
             0x8000 = rekordbox track loaded → BPM valid
             0x7FFF = no track
             0x0000 = non-rekordbox / CD → BPM invalid, freeze last value
0xA6       beat-within-bar (1–4)
```

**Rules**:
- Use `Pitch1` at `0x28`, never `Pitch2` at `0x30`
- Only trust BPM when `Mv == 0x8000`. Otherwise freeze last known good tempo.

---

## Virtual CDJ announce — required to unlock status packets

Send keep-alive packets to port 50000 broadcast every 1500ms.
Structure: mirror the XZ's own keep-alive from the Wireshark capture, substituting your
MAC and IP. Use device number `0x07` — safe, won't conflict with CDJ deck numbers 1–4.

Reference implementation: `grantHarris/prolink-cpp` → `Session::SendAnnounce()`.
Also: `flesniak/python-prodj-link` → `vcdj.py` for field-by-field breakdown.

```cpp
// lib/prolink/bridge.cpp (sketch)
void Bridge::send_keepalive() {
    uint8_t pkt[0x36] = {};
    // magic
    memcpy(pkt, PROLINK_MAGIC, 10);
    pkt[0x0A] = 0x06;                        // type: keep-alive
    memcpy(pkt + 0x0B, device_name_, 20);    // null-padded name
    pkt[0x20] = 0x01;                        // subtype
    pkt[0x21] = 0x07;                        // device number
    // packet length, MAC, IP — fill from config
    keepalive_sock_.send(pkt, sizeof(pkt), broadcast_ip_, 50000);
}
```

---

## Clock architecture — dual-source PLL

```
┌──────────────────────────────────────────────────────────────┐
│  Network thread (Core 0 on ESP32, std::thread on desktop)    │
│                                                              │
│  keep-alive sender → broadcast port 50000 every 1500ms      │
│                                                              │
│  port 50002 status receiver:                                 │
│    parse Pitch1, BPM, flags, Mv, device_num                  │
│    update_master_from_status()                               │
│    if master && playing && Mv==0x8000:                       │
│      clock.update_tempo(track_bpm × pitch1_mult)  ─────────►│
│    if play state changed:                                    │
│      clock.start() / clock.stop()              ─────────────►│
│                                                              │
│  port 50001 beat receiver:                                   │
│    parse beat_in_bar, device_num                             │
│    if master device:                                         │
│      clock.correct_phase(beat_in_bar)          ─────────────►│
└──────────────────────────────────────────────────────────────┤
                                                               │
         ┌─────────────────────────────────────────────────────▼──┐
         │  Clock engine — lib/prolink/clock.hpp                   │
         │                                                         │
         │  ITimer fires every tick_period_us (24 PPQN)            │
         │                                                         │
         │  update_tempo(bpm):                                     │
         │    tick_period_us = 60'000'000 / bpm / 24              │
         │    (immediate, no smoothing)                            │
         │                                                         │
         │  correct_phase(beat_in_bar):                            │
         │    phase_error_us = -(tick_in_beat * tick_period_us)   │
         │    bar_position = beat_in_bar                           │
         │                                                         │
         │  on_tick() [called by ITimer]:                          │
         │    midi.send_byte(0xF8)                                 │
         │    tick_in_beat = (tick_in_beat + 1) % 24              │
         │    correction = phase_error_us / 16  // PLL gain        │
         │    schedule_next(tick_period_us + correction)           │
         │    phase_error_us -= correction                         │
         └─────────────────────────────────────────────────────────┘
```

**Tempo update** (from status packets): immediate, no filtering. 200ms cadence is already
smooth enough. No PLL smoothing on tempo — apply the new period directly.

**Phase correction** (from beat packets): gentle slew via PLL gain `/16`. Start here, tune
by ear. Higher gain (e.g. `/8`) = faster lock, more jitter. Lower (e.g. `/32`) = smoother,
slower to correct.

**Why phase correction still matters**: status packets give accurate *tempo* but not *phase*.
Without beat packets, your clock drifts off the downbeat over time. Beat packets say "I am
at beat N right now" — that's the ground truth for phase.

---

## C++ packet structs

```cpp
// lib/prolink/packets.hpp
#pragma once
#include <cstdint>
#include <optional>

static constexpr uint8_t PROLINK_MAGIC[10] =
    {0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c};
static constexpr uint8_t PKT_TYPE_KEEPALIVE = 0x06;
static constexpr uint8_t PKT_TYPE_BEAT      = 0x28;
static constexpr uint8_t PKT_TYPE_STATUS    = 0x0A;
static constexpr uint32_t PITCH_UNITY       = 0x00100000u;
static constexpr uint16_t MV_REKORDBOX      = 0x8000;

struct BeatPacket {
    uint8_t  device_num;
    uint32_t ms_next_beat;
    uint32_t ms_next_bar;
    uint32_t pitch_raw;
    uint16_t track_bpm_x100;
    uint8_t  beat_in_bar;

    float pitch_multiplier() const {
        return static_cast<float>(pitch_raw) / static_cast<float>(PITCH_UNITY);
    }
    float effective_bpm() const {
        return (track_bpm_x100 / 100.0f) * pitch_multiplier();
    }
};

struct StatusPacket {
    uint8_t  device_num;
    uint32_t pitch1_raw;   // effective pitch — USE THIS
    uint32_t pitch2_raw;   // local fader only — IGNORE
    uint16_t track_bpm_x100;
    uint8_t  flags;
    uint16_t mv;
    uint8_t  beat_in_bar;

    bool is_playing() const { return (flags >> 6) & 1; }
    bool is_master()  const { return (flags >> 5) & 1; }
    bool is_synced()  const { return (flags >> 4) & 1; }
    bool bpm_valid()  const { return mv == MV_REKORDBOX; }

    float pitch_multiplier() const {
        return static_cast<float>(pitch1_raw) / static_cast<float>(PITCH_UNITY);
    }
    float effective_bpm() const {
        return (track_bpm_x100 / 100.0f) * pitch_multiplier();
    }
};

// Returns nullopt if packet is not valid / wrong type
std::optional<BeatPacket>   parse_beat_packet  (const uint8_t* buf, size_t len);
std::optional<StatusPacket> parse_status_packet(const uint8_t* buf, size_t len);
```

```cpp
// lib/prolink/packets.cpp
#include "packets.hpp"
#include <cstring>

static bool has_magic(const uint8_t* buf, size_t len) {
    return len > 10 && memcmp(buf, PROLINK_MAGIC, 10) == 0;
}

static uint32_t read_u32_be(const uint8_t* p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 |
           (uint32_t)p[2]<<8  | (uint32_t)p[3];
}
static uint16_t read_u16_be(const uint8_t* p) {
    return (uint16_t)p[0]<<8 | (uint16_t)p[1];
}

std::optional<BeatPacket> parse_beat_packet(const uint8_t* buf, size_t len) {
    if (!has_magic(buf, len))  return std::nullopt;
    if (buf[0x0A] != PKT_TYPE_BEAT) return std::nullopt;
    if (len < 0x60)            return std::nullopt;

    BeatPacket p{};
    p.device_num      = buf[0x21];
    p.ms_next_beat    = read_u32_be(buf + 0x24);
    p.ms_next_bar     = read_u32_be(buf + 0x2C);
    p.pitch_raw       = read_u32_be(buf + 0x54);
    p.track_bpm_x100  = read_u16_be(buf + 0x5A);
    p.beat_in_bar     = buf[0x5C];
    return p;
}

std::optional<StatusPacket> parse_status_packet(const uint8_t* buf, size_t len) {
    if (!has_magic(buf, len))    return std::nullopt;
    if (buf[0x0A] != PKT_TYPE_STATUS) return std::nullopt;
    if (len < 0xD4)              return std::nullopt;

    StatusPacket p{};
    p.device_num      = buf[0x21];
    p.pitch1_raw      = read_u32_be(buf + 0x28);  // effective pitch
    p.pitch2_raw      = read_u32_be(buf + 0x30);  // local fader, ignored
    p.track_bpm_x100  = read_u16_be(buf + 0x5A);
    p.flags           = buf[0x89];
    p.mv              = read_u16_be(buf + 0x90);
    p.beat_in_bar     = buf[0xA6];
    return p;
}
```

---

## Phase 1: Desktop binary (macOS/Linux)

**Goal**: validate the full data path on a Mac before writing firmware.
Chain: XDJ-XZ → Ethernet → Mac running desktop binary → USB MIDI interface → Sub 27 DIN MIDI in.

### Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . --target xdj_bridge
cmake --build . --target xdj_replay
```

### Desktop-specific dependencies

```cmake
# desktop/CMakeLists.txt
find_package(RtMidi REQUIRED)   # brew install rtmidi
target_link_libraries(xdj_bridge prolink RtMidi::RtMidi)
```

RtMidi handles cross-platform MIDI output (macOS CoreMIDI, Linux ALSA). Equivalent of
`python-rtmidi` / `mido`.

### Desktop I/O implementations

**`UdpPosix`**: wraps `socket()`, `bind()`, `recvfrom()`, `sendto()`. Standard BSD sockets,
works on macOS and Linux with no changes.

**`MidiRtMidi`**: wraps `RtMidiOut`, opens first available port or port specified by
`--midi-port` arg. Calls `sendMessage()` with a 1-byte vector for clock bytes.

**`TimerPosix`**: uses `std::thread` + `std::chrono::steady_clock` with drift correction.
On each tick, records actual wall time, adjusts next sleep by accumulated drift.

```cpp
// desktop/timer_posix.cpp (sketch)
void TimerPosix::set_interval(uint32_t interval_us, std::function<void()> cb) {
    thread_ = std::thread([=]() {
        using namespace std::chrono;
        auto next = steady_clock::now();
        while (running_) {
            cb();
            next += microseconds(interval_us);
            std::this_thread::sleep_until(next);
        }
    });
}
```

Note: `sleep_until` on macOS has ~100µs resolution. This is acceptable for Phase 1
validation — the PLL corrects accumulated drift. For a tighter desktop implementation,
use `mach_wait_until` on macOS or `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`.
The ESP32 hardware timer is exact; this is only a desktop approximation.

### Replay tool

`xdj_replay` reads a pcapng file and re-emits UDP payloads to `localhost:50001` /
`localhost:50002` at original inter-packet timing. Lets `xdj_bridge` run fully offline.

```bash
# Two terminals:
./xdj_replay --file ../captures/xdj-xz-export-mode-pitch-sweep.pcapng --loop
./xdj_bridge --interface lo0 --midi-port "your-interface" --bpm 120
```

Use `libpcap` for reading pcapng on desktop (`brew install libpcap`, find via CMake
`find_package(PCAP)`). Or use a simple header-only pcapng reader — the format is not
complex for this use case.

### CLI interface

```
xdj_bridge [options]
  --interface <iface>     Network interface (default: auto-detect)
  --midi-port <name>      MIDI output port (default: first available)
  --bpm <float>           Fallback BPM when no valid signal (default: 120.0)
  --device-num <n>        Virtual CDJ device number (default: 7)
  --gain <n>              PLL phase correction gain divisor (default: 16)
  --verbose               Print per-packet debug info
```

### What success looks like in Phase 1

1. Sub 27 arpeggiator locks to the XZ within 1-2 beats of pressing play
2. Moving the XZ pitch slider → Sub 27 tempo follows smoothly within ~200ms
3. Pressing stop on XZ → Sub 27 arp stops (MIDI Stop sent)
4. Pressing play again → Sub 27 arp resumes in phase (MIDI Start + phase correction)

---

## Phase 2: ESP32-S3 firmware

**Goal**: same `lib/prolink/` core, new I/O layer, no laptop required.

### Hardware

| Component | Cost | Notes |
|---|---|---|
| ESP32-S3 DevKitC-1 N16R8 | ~$15 | N16R8 variant — 16MB flash, 8MB PSRAM |
| W5500 SPI Ethernet module | ~$8 | HiLetgo or similar, own crystal (no EMAC clock issues) |
| 5-pin DIN female panel jack | ~$2 | For Sub 27 DIN MIDI |
| USB-A female breakout | ~$3 | For OP-XY USB connection |
| 2× 220Ω resistors | pennies | DIN MIDI output |
| Ethernet cable | ~$5 | Cat5e, short |

### Toolchain

Use **PlatformIO** (not raw ESP-IDF) — simpler dependency management, all required
libraries available as PlatformIO packages.

```ini
; firmware/platformio.ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
board_build.mcu = esp32s3
board_upload.flash_size = 16MB

lib_deps =
    midilab/uClock @ ^2.3.0          ; hardware-timer-driven MIDI clock PLL
    sauloverissimo/ESP32_Host_MIDI    ; USB MIDI host + DIN UART unified API
    khoih-prog/Ethernet_Generic       ; W5500 SPI Ethernet

build_flags =
    -std=c++17
    -DBOARD_HAS_PSRAM
```

### Why uClock replaces the desktop TimerPosix entirely

On the ESP32-S3, don't implement your own hardware timer PLL. Use `midilab/uClock` in
`EXTERNAL_CLOCK` mode — it's battle-tested, uses hardware timer interrupts, handles
the FreeRTOS dual-core correctly, and supports 24 PPQN clock output.

```cpp
// firmware/main.cpp (sketch)
#include <uClock.h>
#include "prolink/bridge.hpp"
#include "udp_w5500.hpp"
#include "midi_uart.hpp"
#include "midi_usb_host.hpp"

// Called by uClock at 24 PPQN (hardware timer interrupt)
void on_midi_clock_tick(uint32_t tick) {
    midi_uart.send_byte(0xF8);
    midi_usb.send_byte(0xF8);
}

void on_clock_start() {
    midi_uart.send_byte(0xFA);
    midi_usb.send_byte(0xFA);
}

void on_clock_stop() {
    midi_uart.send_byte(0xFC);
    midi_usb.send_byte(0xFC);
}

// Network task — Core 0
void network_task(void*) {
    UdpW5500 beat_sock, status_sock, keepalive_sock;
    Bridge bridge(beat_sock, status_sock, keepalive_sock, /* no midi — uClock handles it */);

    bridge.on_tempo_update = [](float bpm) {
        // uClock's external clock mode: feed it tempo
        uClock.setTempo(bpm);
    };
    bridge.on_beat = [](uint8_t beat_in_bar) {
        // uClock phase sync
        uClock.clockMe();  // tells uClock a beat boundary just arrived
    };

    bridge.run(); // blocks, processes packets
}

void setup() {
    uClock.setOutputPPQN(uClock.PPQN_24);
    uClock.setClockMode(uClock.EXTERNAL_CLOCK);
    uClock.setOnSync(uClock.PPQN_24, on_midi_clock_tick);
    uClock.setOnClockStart(on_clock_start);
    uClock.setOnClockStop(on_clock_stop);
    uClock.init();

    // Launch network on Core 0, uClock runs on Core 1 via FreeRTOS
    xTaskCreatePinnedToCore(network_task, "net", 8192, NULL, 1, NULL, 0);
}
```

Key insight: `uClock.setTempo(bpm)` is called from status packet updates (~200ms cadence).
`uClock.clockMe()` is called on beat packet arrival for phase correction.
uClock handles everything else — hardware timer, 24 PPQN generation, FreeRTOS safety.

### DIN MIDI wiring (Sub 27)

```
ESP32-S3 GPIO17 (TX) → 220Ω → DIN pin 5
3.3V                 → 220Ω → DIN pin 4
GND                          → DIN pin 2
```

### USB MIDI host (OP-XY)

ESP32-S3's native USB OTG port in host mode via ESP32_Host_MIDI:

```cpp
// firmware/midi_usb_host.cpp
#include <ESP32_Host_MIDI.h>

// Arduino IDE: Tools > USB Mode → "USB Host"
// ESP32_Host_MIDI handles TinyUSB host init, class-compliant device detection,
// and provides sendMidiMessage() that broadcasts to all connected USB MIDI devices

void MidiUsbHost::send_byte(uint8_t byte) {
    midiHandler.sendMidiMessage(&byte, 1);
}
```

OP-XY is USB MIDI class-compliant — no special driver needed. It enumerates automatically.

---

## Phase 3: USB MIDI host to OP-XY

Covered above — ESP32_Host_MIDI handles this as part of Phase 2. It's not a separate phase
so much as an additional output that comes for free once Phase 2 is working. Test DIN MIDI
(Sub 27) first, then plug in the OP-XY and verify it also receives clock.

---

## Phase 4: Polish

- **OLED display** (I2C SSD1306): BPM, master device name, beat-in-bar dot display,
  lock/unlock status indicator
- **Downbeat LED**: GPIO → LED on `beat_in_bar == 1`. Useful for visual phase check.
- **Tap-tempo fallback**: momentary button → manual BPM input when `Mv != 0x8000`
  (unanalyzed track or CD). Tap 4 times, average the intervals.
- **Enclosure**: laser-cut (you have a cutter). Panel cutouts for RJ45, USB-C power,
  DIN5, USB-A, optional button and LED.
- **Start/stop debounce**: XZ flickers the play flag briefly during cue operations.
  Require play state to be stable for >100ms before sending MIDI Start/Stop.

---

## How Phase 1 changes relative to the previous Python plan

| Aspect | Previous (Python) | Now (C++) |
|---|---|---|
| Language | Python | C++17 |
| Core logic reuse | None — full rewrite for MCU | `lib/prolink/` unchanged across all phases |
| I/O swap for MCU | Rewrite everything | Implement 3 interfaces: IUdpSocket, IMidiOut, ITimer |
| USB MIDI host path | Needed separate solution | ESP32_Host_MIDI — one library import |
| Clock PLL on MCU | Hand-rolled or uClock port | uClock drop-in, no porting |
| Build system | pyproject.toml | CMake workspace (desktop) + PlatformIO (firmware) |
| MIDI output library | mido + python-rtmidi | RtMidi (same API concept, C++ native) |
| pcapng replay | scapy | libpcap or simple custom reader |
| Reference to read | python-prodj-link vcdj.py | prolink-cpp — same language, direct adaptation |
| Development speed | Faster iteration | Slightly slower first build, much faster Phase 2 |

The key point: **Phase 1 is not a throwaway prototype anymore**. The code you write in
`lib/prolink/` during Phase 1 is the firmware. You're writing it once.

---

## Build order (what to do first)

1. Set up CMake workspace, confirm `lib/prolink/` compiles with no dependencies
2. Write `packets.hpp/cpp` + unit tests (feed hex from captures, assert parsed fields)
3. Write `replay` tool — essential for offline iteration
4. Write `clock.hpp/cpp` — PLL engine with injected ITimer
5. Wire up `desktop/` I/O: UdpPosix, MidiRtMidi, TimerPosix
6. Run desktop bridge against replay — confirm Sub 27 locks to constant-BPM capture
7. Add virtual CDJ keep-alive sender to bridge
8. Run against live XZ — confirm status packets arrive
9. Tune PLL gain by ear with pitch sweep
10. Port I/O to firmware/ — UdpW5500, MidiUart, swap TimerPosix for uClock

---

## Reference libraries

| Library | Language | What to look at |
|---|---|---|
| [prolink-cpp](https://github.com/grantHarris/prolink-cpp) | C++17 | Direct reference — packet parsing, virtual CDJ, beat clock, follow_master. Adapt, don't copy wholesale. |
| [uClock](https://github.com/midilab/uClock) | C++ Arduino | Hardware timer PLL for MCU. Use EXTERNAL_CLOCK mode with setTempo() + clockMe(). |
| [ESP32_Host_MIDI](https://github.com/sauloverissimo/ESP32_Host_MIDI) | C++ Arduino | USB host + DIN UART unified MIDI API for ESP32-S3. |
| [python-prodj-link](https://github.com/flesniak/python-prodj-link) | Python | Read `vcdj.py` for virtual CDJ keep-alive packet structure. Read `midiclock.py` for `bpm × actual_pitch` pattern. Don't import. |
| [beat-link](https://github.com/Deep-Symmetry/beat-link) | Java | VirtualCdj.java — authoritative reference for Pitch1/Pitch2 distinction and tempoChanged() pattern. |
| [cardinia](https://github.com/nudge/cardinia) | C/C++ embedded | Shipped hardware doing exactly this. Read firmware for validation. |
| [DJ Link Ecosystem Analysis](https://djl-analysis.deepsymmetry.org/djl-analysis/) | Docs | Ground truth for all packet field offsets. |

---

## Known hard constraints

- **Export mode only**: XZ drops Pro DJ Link in Performance mode.
- **Mv == 0x8000 for valid BPM**: freeze last known good tempo for non-rekordbox sources.
- **Full uint32 pitch range**: observed −70% to +100%. Handle it.
- **Virtual CDJ required for status packets**: use device number 7.
- **Pitch1 not Pitch2**: `0x28` is effective pitch, `0x30` is local fader. They diverge in sync mode.
- **Missed beat packets**: free-run at last known tempo. Don't assume continuous arrival.
- **W5500 over SPI, not native EMAC**: avoids ESP32 EMAC/Wi-Fi PLL clock instability. Keep Wi-Fi disabled while running.

---

## Captures

```
captures/xdj-xz-export-mode.pcapng
  - 297 beat packets, steady 132 BPM, ~134s, no pitch movement
  - Good for: baseline PLL tuning, constant-tempo lock verification

captures/xdj-xz-export-mode-pitch-sweep.pcapng
  - 144 beat packets, 136 BPM track, pitch swept ±6%/±10%/±16%/WIDE
  - Observed range: −70% to +100%
  - Good for: pitch encoding validation, tempo tracking responsiveness
  - Note: no status packets (no virtual CDJ present during capture)
```