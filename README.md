# dj-midi-sender

Standalone bridge that turns a Pioneer XDJ-XZ's Pro DJ Link Ethernet broadcast
into rock-solid MIDI clock with continuous tempo tracking. End-state hardware
is an **ESP32-S3 + W5500** box with a USB MIDI host jack (and a planned 5-pin
DIN output) — no laptop in the signal chain.

**It runs on real hardware today:** a Waveshare ESP32-S3-ETH clocks a
**Teenage Engineering OP-XY** and a **Moog Sub 25** over USB, both plugged into
the box's USB-A host jack and locked to the XZ's tempo. (Phase 1 also drove the
OP-XY directly from a Mac over CoreMIDI/RtMidi — still useful for protocol
debugging; see below.)

Slave targets:

- **OP-XY** and **Moog Sub 25** via **USB MIDI** — the box is the USB host;
  both enumerate on the USB-A jack. ✅ working
- **5-pin DIN** out (IO17 → 220 Ω → DIN pin 5) — planned, not yet wired/coded.

The XDJ-XZ has no native MIDI clock output. Pro DJ Link over Ethernet is the
only extraction path.

## Architecture in one paragraph

Pioneer broadcasts two relevant packet types on the link:
**beat packets** (port 50001, one per beat) and **status packets**
(port 50002, ~5 Hz). Beat packets stream natively; status packets only flow
once we announce ourselves as a virtual CDJ on port 50000. Tempo is taken
from status packets (so pitch-slider sweeps follow within ~200 ms instead of
~500 ms), and beat packets are used only to nudge phase. A 24 PPQN clock is
generated on a hardware timer (desktop: `std::thread`; firmware: a native
ESP-IDF `esp_timer` one-shot shim — uClock was dropped due to an arduino-esp32
v2/v3 timer-API mismatch).

See [docs/architecture.md](docs/architecture.md) for protocol details,
field offsets, and PLL design. See [docs/phases.md](docs/phases.md) for the
roadmap from Phase 1 (desktop binary) to Phase 4 (custom enclosure).

## Repo layout

```
dj-midi-sender/
├── CMakeLists.txt              # workspace root
├── lib/
│   └── prolink/                # core library — compiles unchanged on macOS AND ESP32-S3
│       ├── types.hpp           # shared constants
│       ├── packets.hpp/.cpp    # beat + status packet parsers
│       ├── clock.hpp/.cpp      # 24 PPQN PLL clock engine
│       └── bridge.hpp/.cpp     # orchestration, master tracking, state machine
├── desktop/                    # Phase 1 — macOS/Linux binary
│   ├── udp_posix.hpp/.cpp      # POSIX UDP socket
│   ├── midi_rtmidi.hpp/.cpp    # RtMidi MIDI output
│   ├── timer_posix.hpp/.cpp    # std::thread-based ITimer
│   ├── main.cpp                # xdj_bridge entry point
│   └── replay.cpp              # xdj_replay — pcapng playback for offline dev
├── firmware/                   # Phase 2/3/4 — Waveshare ESP32-S3-ETH (built; runs on hardware)
│   ├── platformio.ini          # envs: waveshare_esp32s3_eth (prod) + diag (serial debug)
│   ├── sdkconfig.defaults      # IDF config: W5500 SPI, USB host, 16 MB flash / PSRAM
│   └── src/
│       ├── udp_w5500.*         # IUdpSocket — lwIP over the onboard W5500
│       ├── timer_esp.*         # ITimer — esp_timer one-shot
│       ├── midi_host_usb.*     # IMidiOut — native USB MIDI host (esp_usb_host)
│       ├── ui_display.*        # SSD1306 status screen (U8g2, hardware I2C)
│       ├── ui_input.*          # EC11 encoder + nudge/tap buttons
│       └── main.cpp            # ethernet bring-up + wiring it all together
├── captures/                   # canonical pcapng captures for offline replay
└── docs/                       # architecture, phases, session-notes (live handoff), v3/v4 context
```

`lib/prolink/` is pure C++17 with no platform headers beyond `<cstdint>`,
`<cstring>`, `<functional>`, and `<optional>`. All I/O is injected via
three small interfaces (`IUdpSocket`, `IMidiOut`, `ITimer`) so the same
code drives both Phase 1 and the firmware.

## Building (Phase 1, macOS)

Prerequisites:

```bash
brew install cmake rtmidi glfw          # libpcap ships with the Xcode SDK
```

(`glfw` is only needed for the `--visualize` GUI panel. To skip it,
configure with `-DXDJ_GUI=OFF` — the bridge will build headless-only.
Dear ImGui itself is pulled in by CMake's FetchContent and pinned to a
specific tag — no manual setup needed.)

Configure and build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Two binaries land in `build/desktop/`:

- `xdj_bridge` — listens on the link and sends MIDI clock
- `xdj_replay` — replays a pcapng capture to localhost (offline dev loop)

## Running

### Offline (against a capture)

Two terminals:

```bash
# T1 — replay the pitch-sweep capture in a loop
./build/desktop/xdj_replay --file captures/xdj-xz-export-mode-pitch-sweep.pcapng --loop

# T2 — run the bridge against localhost
./build/desktop/xdj_bridge --bind 127.0.0.1 --midi-port "<your-interface>"
```

Note: the canonical captures contain beat packets only (no status packets —
no virtual CDJ was present during capture). Replay-mode runs validate the
parsers and the phase-correction path; tempo-tracking smoothness has to be
validated against a live XZ.

### Live (against the XDJ-XZ)

Plug the XZ into your Mac's Ethernet (USB-Ethernet dongle is fine). Put the
XZ in Export mode with a rekordbox-analyzed track loaded, then:

```bash
./build/desktop/xdj_bridge --midi-port "OP-XY"
```

(Use `--list-midi` to print the exact CoreMIDI port name the OP-XY
enumerates as — it usually matches the device name verbatim.)

The bridge auto-detects the interface, sends a virtual-CDJ keep-alive on
port 50000 broadcast every 1500 ms, and starts receiving status packets on
port 50002. Pressing play on the XZ should produce MIDI Start + 24 PPQN
clock immediately.

CLI flags:

```
--bind <addr>           UDP bind address (default 0.0.0.0; use 127.0.0.1 with replay)
--interface <iface>     Network interface for virtual-CDJ announce (default: auto)
--midi-port <name>      MIDI output port (default: first available)
--list-midi             List MIDI output ports and exit
--bpm <float>           Fallback BPM when no valid signal (default 120.0)
--device-num <n>        Virtual CDJ device number (default 7)
--gain <n>              PLL phase correction gain divisor (default 16)
--clock-offset-ms <f>   Lead the beat by N ms (chain latency). Persisted per port.
--grid-offset-ms <f>    Per-track beat-grid offset (session only).
--no-vcdj               Skip the virtual-CDJ announce (beat packets only)
--visualize             Open the ImGui debug panel
--verbose               Per-packet debug output
```

### The visualizer

`--visualize` opens a small GUI window (Dear ImGui via GLFW + OpenGL) with
three panels:

- **Master** — device + bar position (4 dots, downbeat red), track BPM,
  pitch %, effective BPM, status flags (master / playing / sync / on-air),
  Mv validity, packet counts.
- **MIDI clock out** — port, tick-pulse bar (one blip per emitted clock
  byte), tick rate (`ticks/sec` and derived BPM), tick-in-beat (0–23),
  phase error, current tick period.
- **Offsets** — clock and grid offsets with `±1 ms` / `±10 ms` buttons.

Keyboard:

| Key | Action |
|-----|--------|
| `←` / `→` | Grid offset ∓1 ms (per-track, session only) |
| `↑` / `↓` | Grid offset ±10 ms |
| `Shift+←` / `Shift+→` | Grid offset ∓10 ms (alternative to `↑`/`↓`) |
| `[` / `]` | Clock offset ∓1 ms (per-port, persisted) |
| `Shift+[` / `Shift+]` | Clock offset ∓10 ms |
| `0` or `R` | Reset grid offset |
| `Q` or `Esc` | Quit |

### The two-axis offset model

| Axis | What it compensates | Scope | Persistence |
|------|---------------------|-------|-------------|
| **Clock offset** | USB / slave physical processing latency | per output port | `~/.config/dj-midi-sender.json`, loaded on startup |
| **Grid offset**  | Per-track rekordbox beat-grid skew | per session | never persisted |

The scheduler sees only the sum. Splitting them means the OP-XY's clock
calibration stays correct across tracks while you twiddle grid-offset
between songs whose intros are wonky.

## Firmware (ESP32-S3)

The box is a **Waveshare ESP32-S3-ETH** (integrated W5500). `lib/prolink/`
compiles unchanged — only the I/O layer differs (`firmware/src/`). Built with
PlatformIO; the same core that runs the desktop binary drives the hardware.

```bash
cd firmware

# production — USB MIDI host mode (this is the box)
pio run -e waveshare_esp32s3_eth -t upload --upload-port /dev/cu.usbmodemXXXX

# diag — counter-stub MIDI, keeps the USB-C serial console alive for debugging
pio run -e diag -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor -e diag
```

Gotchas (full list in [docs/session-notes.md](docs/session-notes.md)):

- **No serial in host mode.** USB-Serial-JTAG and USB-OTG share one PHY, so the
  production build's console is dead — status shows on the OLED and the RGB LED
  (blue = waiting, red = device without a MIDI interface, green = clocking).
  Use the `diag` build when you need serial.
- **To flash**, force the ROM bootloader: hold BOOT, tap RESET, release BOOT.
- **Power rule:** with a synth in the USB-A jack, power from a charger/wall, not
  a computer — two hosts on the shared IO19/20 lines conflict.

Board pinout, the USB-host enumeration fix, and wiring notes are in
[docs/xdj-midi-bridge-context-v4.md](docs/xdj-midi-bridge-context-v4.md).

### Front-panel controls

128×64 OLED, an EC11 encoder (rotate + push), three buttons (nudge −, nudge +,
tap). The UI is a small mode machine — **Normal** status screen, **Source-select**,
and a **Settings menu**.

**Normal screen**
- **Nudge − / +** — trim the clock offset by the configured *Offset step*;
  **hold** to auto-repeat with acceleration. Offset persists to NVS (+30 ms
  first-boot fallback).
- **Encoder push** → Source-select. **Hold both nudges ~1 s** → Settings menu.

**Source-select** (`mstr / P1–P4 / off`) — **spin** moves a `>` cursor (the
active source stays put), **push** confirms, **tap** cancels. `mstr` follows the
master deck; `P1–P4` pin a deck; **`off`** ignores all decks → standalone.

**Standalone (`off`)** — no DJ-Link needed. The clock cold-starts on a manual
tempo (OLED shows `OFF`). **Tap in rhythm** = tap-tempo (averages the last 8
taps); **spin** = fine-tune by the *Tap-fine* step (default 0.1 BPM).

**Free mode** (player source) — keeps clocking when the deck stops; **hold tap +
spin** trims BPM live (`MAN`). A master beat re-syncs.

**Settings menu** — spin to scroll, push to edit, spin to change, push to save,
tap to back. All persisted to NVS: **Mode** (Sync/Free), **BPM step**
(0.1/0.5/1/5), **Tap fine** (0.1/0.25/0.5/1), **Offset step** (0.1/0.5/1 ms).

The **encoder direction** is inverted from the raw quadrature to match the
panel feel ([ui_input.cpp](firmware/src/ui_input.cpp)).

## Captures

Two pcapng files in [captures/](captures/) verified against real XZ hardware:

| File | Contents | Use for |
|------|----------|---------|
| `xdj-xz-export-mode.pcapng` | 297 beat packets, steady 132 BPM, ~134 s | baseline lock verification |
| `xdj-xz-export-mode-pitch-sweep.pcapng` | 144 beat packets, 136 BPM track, pitch swept ±6%/±10%/±16%/WIDE (−70% to +100%) | pitch encoding + tempo tracking |

Neither contains port 50002 status packets — no virtual CDJ was present
when the captures were taken. To exercise the dual-source path end-to-end,
you need the live XZ.

## Constraints worth knowing

- **Export mode only.** The XZ drops Pro DJ Link entirely in Performance mode.
- **Rekordbox-analyzed tracks only for valid BPM.** Unanalyzed audio sets
  `Mv != 0x8000`; freeze the last known good tempo in that case.
- **Use `Pitch1` (status `0x8C`), never `Pitch2` (`0x98`).** When a CDJ
  is synced to a master, `Pitch1` tracks the master and `Pitch2` stays
  pinned to the local fader — they diverge. (The original handoff's `0x28`/
  `0x30` were wrong; corrected offsets are in [docs/architecture.md](docs/architecture.md).)
- **Virtual CDJ uses device number 7.** CDJ deck numbers 1–4 are reserved
  for real decks; 5–6 are reserved for mixers. 7 is safe.

## Status

- **Phase 1 — desktop binary:** ✅ done, validated live against the XZ.
- **Phase 2/3 — firmware:** ✅ running on a Waveshare ESP32-S3-ETH. Full pipeline
  on hardware: Ethernet → parse → dual-source PLL → 24 PPQN → USB MIDI host →
  OP-XY / Sub 25, both locking to the master's tempo and pitch.
- **Phase 4 — front panel:** the full UI is working — OLED, EC11 encoder, and
  buttons drive source-select, a persisted **settings menu**, offset trim,
  free-run, and a **standalone tap-tempo mode** (`off` source). All settings
  persist to NVS.
- **Tuned for hardware:** drift-free timer + continuous-µs phase lock keep the
  OP-XY tight across tempo (offset ~+30 ms, persisted); a dropped beat packet
  no longer causes a false Stop+Start dropout.
- **Next:** DIN-5 MIDI out, then the big one — ESP as tempo master so CDJs sync
  *to* the box (free-run + manual BPM are its building blocks).

[docs/session-notes.md](docs/session-notes.md) is the live handoff;
[docs/phases.md](docs/phases.md) is the roadmap.

## License

Personal project — no license declared yet.
