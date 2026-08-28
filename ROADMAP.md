# Roadmap

The end product is a standalone box that plugs into a Pro DJ Link Ethernet
switch (alongside the CDJs) and exposes MIDI clock over both a 5-pin DIN jack
and a USB MIDI host port — no laptop in the signal chain. A drum machine
plugged into it behaves like another deck in sync: tempo follows the master's
pitch fader, bar position aligns with the master.

See [docs/architecture.md](docs/architecture.md) for the protocol and clock
design, and [docs/hardware.md](docs/hardware.md) for the board and wiring.

## Done

- **Desktop bridge (reference implementation).** Full network path validated
  live against an XDJ-XZ: packet capture to parse to master tracking to
  dual-source PLL to MIDI clock, driving a class-compliant USB MIDI synth via
  the host's MIDI stack. Includes an ImGui debug/visualizer panel, a pcapng
  replay tool for offline development, and `xdj_clockmon`, a MIDI-clock input
  analyzer (derived BPM, per-tick jitter, Start/Stop events).
- **Firmware on the ESP32-S3.** The same `lib/prolink/` core runs on the
  Waveshare ESP32-S3-ETH: Ethernet in over the W5500, parse, dual-source PLL,
  24 PPQN clock generation.
- **USB MIDI host output.** Class-compliant USB MIDI devices enumerate on the
  USB-A jack and clock in sync with the master, following pitch-fader moves.
- **DIN-5 MIDI out.** UART1 sink fanned out alongside the USB host from the one
  PLL. Validated on hardware (steady BPM, sub-0.1 ms per-tick jitter into a
  MIDI interface). Both outputs run simultaneously off the single clock.
- **Front-panel UI.** OLED status screen, EC11 encoder, and buttons drive
  source-select (master / per-deck / standalone), a settings menu, offset trim,
  free-run, and a standalone tap-tempo mode. All settings persist to NVS.
- **Manual beat re-sync.** A front-panel tap re-emits MIDI Start on the
  master's next downbeat to snap a locally-restarted slave back into bar
  alignment (MIDI clock carries tempo but not position).
- **Lock quality.** Drift-free timer plus continuous-microsecond phase lock;
  bar-slip realignment gated behind a confidence counter so a single dropped
  beat packet cannot cause a false stop.

- **ESP32 as tempo master.** The box claims the Pro DJ Link tempo-master role
  so CDJs sync *to* it: it emits its own beat and status packets, performs the
  documented handoff (request `0x26`, the master's `0x27` acknowledgement, then
  the `Mm`/`Mh`/`Syncn` dance), and always yields the role back when a DJ asks
  for it from a deck. Verified live against an XDJ-XZ and an XDJ-700. Selected
  from the front panel as the `sync master` source, gated behind an "Act as
  player" setting.
- **Unit tests and CI.** A dependency-free suite covers the reverse-engineered
  wire formats and the bridge state machine; GitHub Actions runs it on
  gcc/clang/macOS, under ASan/TSan/UBSan, and builds the desktop and firmware
  targets.

## Next

## Later / maybe

- Custom PCB combining the ESP32-S3, W5500, DIN buffer, USB-A jack, and power.
- Enclosure.
- Multi-CDJ master handoff tracking via status packets.
- Bidirectional Ableton Link bridge.
- OSC output for lighting / visuals.
- DMX output triggered on downbeat.

## Constraints worth knowing

- **Export mode only.** The XDJ-XZ drops Pro DJ Link entirely in Performance
  mode.
- **Rekordbox-analyzed tracks for valid BPM.** Unanalyzed audio marks the tempo
  invalid; the clock freezes the last known good tempo in that case.
- **Virtual CDJ uses a safe device number.** Deck numbers 1-4 are reserved for
  real decks and 5-6 for mixers.
