# dj-midi-sender

Standalone bridge that turns a Pioneer XDJ-XZ's Pro DJ Link Ethernet broadcast
into rock-solid MIDI clock with continuous tempo tracking. End-state hardware
is an **ESP32-S3 + W5500** box with a 5-pin DIN output and a USB MIDI host
jack — no laptop in the signal chain.

Slave targets:

- **Teenage Engineering OP-XY** via USB MIDI (the Phase-1 target — driven
  directly from the Mac's USB through CoreMIDI / RtMidi)
- **Moog Subsequent 27** via 5-pin DIN (Phase 2 — needs the firmware's UART
  DIN output; not exercised in Phase 1)

The XDJ-XZ has no native MIDI clock output. Pro DJ Link over Ethernet is the
only extraction path.

## Architecture in one paragraph

Pioneer broadcasts two relevant packet types on the link:
**beat packets** (port 50001, one per beat) and **status packets**
(port 50002, ~5 Hz). Beat packets stream natively; status packets only flow
once we announce ourselves as a virtual CDJ on port 50000. Tempo is taken
from status packets (so pitch-slider sweeps follow within ~200 ms instead of
~500 ms), and beat packets are used only to nudge phase. A 24 PPQN clock is
generated on a hardware timer (desktop: `std::thread`; firmware: ESP32
hardware timer via [`uClock`](https://github.com/midilab/uClock)).

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
├── firmware/                   # Phase 2/3 — ESP32-S3 (not yet built)
├── captures/                   # canonical pcapng captures for offline replay
├── docs/                       # architecture + phases
└── claude/                     # project handoff doc (source of truth for plan)
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
- **Use `Pitch1` (status `0x28`), never `Pitch2` (`0x30`).** When a CDJ
  is synced to a master, `Pitch1` tracks the master and `Pitch2` stays
  pinned to the local fader — they diverge.
- **Virtual CDJ uses device number 7.** CDJ deck numbers 1–4 are reserved
  for real decks; 5–6 are reserved for mixers. 7 is safe.

## Status

Phase 1 in progress. Currently working through the desktop binary against
the captures — see [docs/phases.md](docs/phases.md) for the punch list.

## License

Personal project — no license declared yet.
