# Project phases

The end product is a **standalone microcontroller-based box** that plugs into
the Pro DJ Link Ethernet switch (alongside the CDJs) and exposes a 5-pin DIN
MIDI out and a USB MIDI host port. No laptop in the signal chain. A drum
machine plugged into its USB port behaves like another CDJ deck in Sync mode —
tempo follows the master's pitch slider, bar/beat position aligns with the
master.

The C++ in [`../lib/prolink/`](../lib/prolink/) is the **firmware**, not a
prototype. The Phase-1 macOS binary is the same library wrapped in POSIX
sockets, RtMidi, and a `std::thread` timer. Every line in `lib/prolink/`
ships in the final box.

See [architecture.md](architecture.md) for protocol / PLL details, and
[session-notes.md](session-notes.md) for the live running status + handoff.

> **Status (2026-06-01):** Phase 1 ✅ done. Phases 2 + 3 are effectively done
> for the **USB** output — the box runs the full pipeline on the Waveshare
> ESP32-S3-ETH and clocks a Sub 25 / OP-XY over the USB-A host jack. **DIN MIDI
> out is not wired yet**, and the Phase-4 UI (OLED, encoder, buttons) is not
> wired. Two implementation choices diverged from the plan below: the firmware
> timer is a native `esp_timer` shim (`TimerEsp`), **not uClock**, and the USB
> host uses ESP-IDF's native `usb_host` library, **not `ESP32_Host_MIDI`**.

## Phase 1 — Desktop binary on Mac  ✅ done

```
XDJ-XZ ─ethernet─→ Mac (xdj_bridge) ─USB MIDI─→ OP-XY
```

**Goal:** validate the full network-side path — packet capture → parse →
master tracking → dual-source PLL → MIDI clock — against real hardware
before writing firmware. Slave is the OP-XY connected to the Mac via USB.
macOS exposes the OP-XY as a CoreMIDI port; RtMidi opens it like any other
output.

Deliverables:

- [ ] CMake workspace, builds `xdj_bridge` and `xdj_replay`
- [ ] `lib/prolink/packets.cpp` — beat + status packet parsers, unit tests
      against hex fixtures derived from the captures
- [ ] `lib/prolink/clock.cpp` — 24 PPQN PLL engine with `update_tempo()`,
      `correct_phase()`, gain-tunable phase slew
- [ ] `lib/prolink/bridge.cpp` — orchestration, master tracking, play-state
      debounce, hold-Start-until-downbeat
- [ ] `desktop/udp_posix.cpp` — BSD UDP socket implementation of `IUdpSocket`
- [ ] `desktop/midi_rtmidi.cpp` — RtMidi-backed `IMidiOut`
- [ ] `desktop/timer_posix.cpp` — `std::thread` + `sleep_until` `ITimer`
- [ ] `desktop/replay.cpp` — pcapng playback for offline iteration
- [x] Smoke test: replay capture → bridge → OP-XY, confirm clock arrives
- [x] Persisted per-port clock-offset + session-only grid-offset
      ([desktop/config_posix.cpp](../desktop/config_posix.cpp)),
      tunable live in the ImGui visualizer
- [x] ImGui debug panel ([desktop/gui_imgui.cpp](../desktop/gui_imgui.cpp))
      — Master / MIDI clock out / Offsets panels with live nudge buttons
- [ ] Live test: XZ → bridge → OP-XY, confirm virtual CDJ unlocks status
      packets and tempo tracks the pitch slider smoothly
- [ ] Tune PLL gain by ear (start at `/16`, try `/8` and `/32`)
- [ ] Live test across a pitch sweep — OP-XY sequencer must hold time and
      bar alignment without manual intervention
- [ ] Calibrate the OP-XY's clock offset by ear and confirm the saved value
      survives a restart

**Exit criteria:** plugging the OP-XY into the Mac and pressing play on the
XZ produces in-time playback that follows pitch-slider sweeps continuously
(not stepped at beat boundaries). MIDI Start/Stop respect play-state changes.

## Phase 1.5 — Drift recovery (optional)

Bar-alignment as implemented in Phase 1 is **set at Start only**. If a beat
packet is dropped, the slave may end up offset from the master without the
bridge knowing.

- [ ] Track `beat_in_bar` across successive packets; detect missed-beat gaps.
- [ ] On detected slip, force a re-align — Stop + Start on the next downbeat.
- [ ] Optional: emit MIDI **Song Position Pointer (SPP)** every bar for
      devices that honor it (TBD whether the OP-XY does).

Punt on this until Phase 1 is otherwise solid — drift hasn't been an
audible problem in the captures we have.

## Phase 2 — ESP32-S3 firmware  ◀ current (USB done; DIN pending)

> Network/clock half ✅ (validated on the `diag` build over serial). DIN MIDI
> output on IO17 is **not wired or coded yet** — the Sub 25 was tested over
> USB, not DIN. Board is the **Waveshare ESP32-S3-ETH** (integrated W5500),
> not the DevKitC-1 + standalone module described below.

```
Pro DJ Link switch ─RJ45─→ [ W5500 ─SPI─ ESP32-S3 ─UART─ DIN MIDI ] ──→ Sub 27
```

`lib/prolink/` compiles unchanged; only the I/O layer is replaced. The
microcontroller is the product from this point on; the desktop binary
becomes reference / debug material.

Hardware:

- **ESP32-S3 DevKitC-1 N16R8** — 16 MB flash, 8 MB PSRAM, native USB OTG
  (required so Phase 3 doesn't need a board swap)
- **W5500 SPI Ethernet module** (~$8, HiLetgo or similar — own crystal so
  no EMAC/Wi-Fi PLL clock issues)
- **5-pin DIN female panel jack** + 2× 220 Ω resistors
- USB-C jack for power and dev/flash

Firmware structure (mirrors `desktop/`, swaps the I/O implementations):

| Concern   | Desktop                    | Firmware                                              |
|-----------|----------------------------|-------------------------------------------------------|
| Sockets   | `UdpPosix` — BSD sockets   | `UdpW5500` — lwIP over SPI                            |
| MIDI out  | `MidiRtMidi` — CoreMIDI    | `MidiUart` — UART TX → 220 Ω → DIN pin 5              |
| Timer     | `TimerPosix` — std::thread | [`uClock`](https://github.com/midilab/uClock) hw timer |

uClock replaces `TimerPosix` entirely. Don't roll our own ESP32 hardware
timer — use `uClock.setTempo(bpm)` from status updates and
`uClock.clockMe()` from beat arrivals. uClock owns the 24-PPQN tick
emission, the FreeRTOS dual-core split, and the hardware timer ISR.

Toolchain: **PlatformIO** (not raw ESP-IDF) — simpler dep management, all
required libraries available as packages.

**Exit criteria:** standalone box matches the Phase-1 Mac bridge's lock
quality driving the Sub 27. No laptop in the signal path.

## Phase 3 — Add USB MIDI host (OP-XY on the box)  ✅ working (USB-A jack)

> Done and verified: USB-A jack wired straight to the native USB pins
> (D−→IO19, D+→IO20), ESP32 as host, OP-XY + Sub 25 enumerate and follow the
> master. Implemented with IDF's native `usb_host` (not TinyUSB /
> ESB32_Host_MIDI). Remaining: this output and the (unwired) DIN output share
> the one PLL, which is already true in code.

```
                                                  ┌─UART──→ DIN MIDI ──→ Sub 27
Pro DJ Link switch ─RJ45─→ [ W5500 ── ESP32-S3 ]──┤
                                                  └─USB MIDI host (A jack) ──→ OP-XY
```

The product gains its second output: a USB-A jack acting as a **USB MIDI
host** so any class-compliant USB MIDI device (OP-XY, Digitakt, MPC, etc.)
plugs straight in. The ESP32-S3's native USB OTG drives TinyUSB in host
mode, wrapped by `sauloverissimo/ESP32_Host_MIDI`. The OP-XY is the same
target as Phase 1 — only the host (Mac → ESP32-S3) changes.

Both outputs share the same PLL — there is exactly one tick generator.
The DIN and USB sinks are just two destinations the tick callback writes to.

**Exit criteria:** plug an OP-XY into the USB-A jack on the box; its
sequencer plays in time with the master deck, downbeat-aligned, while the
Sub 27 simultaneously clocks off the DIN jack.

## Phase 4 — Productization

> **Status (2026-06-08):** the front panel is essentially done — **OLED**,
> **EC11 encoder**, and **buttons** drive source-select (`mstr/P1–P4/off`), a
> **settings menu** (Mode, BPM/Tap-fine/Offset steps — all NVS-persisted),
> offset trim, free-run, and a **standalone tap-tempo mode** (`off` source:
> tap-the-rhythm + spin fine-tune, cold-starts the clock). The earlier
> clock-jitter and dropout bugs are fixed. Remaining: **DIN-5 MIDI out**, then
> the ESP-as-tempo-master phase. See [session-notes.md](session-notes.md).

- **Custom PCB** combining ESP32-S3 + W5500 + DIN buffer + USB-A host jack +
  power, replacing the modular dev boards.
- **Enclosure** — laser-cut or off-the-shelf project box (Hammond 1593).
- **OLED display** (I²C SSD1306): current BPM, pitch %, lock state, master
  deck number, beat-in-bar dots.
- **Downbeat LED** gated on `beat_in_bar == 1`.
- **Tap-tempo button** for unanalyzed audio (`Mv != 0x8000`) and non-DJ-Link
  sources.
- **MIDI Start / Stop debouncing** driven by parsed status-packet play-flag
  transitions (100 ms stable window).

## Deferred / "maybe someday"

- Multi-CDJ master handoff tracking via status packets.
- Bidirectional **Ableton Link** bridge.
- **OSC** output for lighting / visuals.
- **DMX** output triggered on downbeat (would slot in with existing ESP32
  LED work).
