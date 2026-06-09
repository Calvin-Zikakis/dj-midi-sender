# XDJ-XZ → MIDI Clock Bridge: Hardware Build Status (v4 addendum)

**Read this together with `xdj-midi-bridge-context-v3.md`.** That v3 doc has the full protocol
analysis, packet structures, dual-source PLL architecture, and the C++ codebase plan — all
still current. This v4 file is the hardware addendum: it records the actual board chosen, the
verified pin map, the USB host decision, and what's been physically built and tested so far.
Where this file and v3 disagree on hardware specifics, **this file wins** (v3 was written
before the board was in hand).

---

## Where the project is right now

**Status: 2026-06-01 — the full bridge works end-to-end on real hardware.**
`XDJ-XZ → W5500/Ethernet → parse → dual-source PLL → 24 PPQN clock → USB-A
host → synth`, verified live with **both a Moog Sub 25 and an OP-XY** locking
to the XZ's tempo. See [session-notes.md](session-notes.md) for the running
handoff and the exact next steps. Summary:

- Protocol fully reverse-engineered and verified against two real captures (see v3).
- Firmware (`firmware/`) running on the board: Ethernet up, Pro DJ Link
  received, master tracked, clock generated, **USB MIDI host enumerating real
  devices and clocking them**. The one non-obvious fix that made USB host work:
  IDF 4.4 aborts enumeration if a device's config descriptor exceeds
  `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` (default 256) — USB-MIDI synths
  blow past that, so it's bumped to 2048 in `sdkconfig.defaults`.
- **Wired + working:** USB-A host jack, the **SSD1306 OLED** (HW I²C), the
  **nudge buttons** (offset trim), the **EC11 encoder**, and the **tap button**
  — together driving the full front-panel UI: source-select (`mstr/P1–P4/off`),
  an NVS-persisted settings menu, free-run, and standalone tap-tempo. **Not yet
  wired:** DIN-5 MIDI out. (Pins reserved below.)
- The earlier **clock-jitter and OP-XY-dropout bugs are fixed** (drift-free
  timer + continuous-µs phase lock; bar-slip confidence counter). Offset sits
  ~+30 ms and is tempo-independent. See [session-notes.md](session-notes.md).
- The board is on header pins; peripherals will be connected with dupont jumpers for bring-up,
  then made permanent (likely a crimped dupont harness, not soldered-to-pin) once proven.

---

## The actual board: Waveshare ESP32-S3-ETH

This replaces the generic "ESP32-S3 DevKitC-1 + separate W5500 module" placeholder in v3.
It's a single board with the W5500 already integrated.

- Chip: **ESP32-S3R8** (8MB octal PSRAM, dual-core LX7 @ 240MHz)
- Flash: 16MB (W25Q128)
- Ethernet: **onboard W5500** over SPI (no separate module needed)
- USB-C: native USB, used for power + flashing + serial. **Wired as a device port**
  (5.1kΩ CC pulldowns). Connects to the ESP32 native USB pins (IO19/IO20) through 30Ω series
  resistors. There is **no separate USB-UART bridge** — programming is over native USB CDC.
- Pico-style 2×20 headers (H1, H2) expose the usable GPIO.

Wiki: https://www.waveshare.com/wiki/ESP32-S3-ETH
Schematic: https://files.waveshare.com/wiki/ESP32-S3-ETH/ESP32-S3-ETH-Schematic.pdf

### PlatformIO target

The v3 `platformio.ini` used `esp32-s3-devkitc-1`. For this board, use a generic S3 env with
PSRAM enabled. `esp32-s3-devkitc-1` works as the board target in practice (same MCU family),
but confirm flash/PSRAM settings:

```ini
[env:esp32-s3-eth]
platform = espressif32
board = esp32s3box            ; or esp32-s3-devkitc-1; both are generic S3, verify on first flash
framework = arduino
board_build.flash_mode = qio
board_build.psram_type = opi  ; R8 = octal PSRAM
board_upload.flash_size = 16MB

lib_deps =
    midilab/uClock
    sauloverissimo/ESP32_Host_MIDI
    ; W5500: use the ESP32 built-in ETH (ETH.h) with the W5500 SPI driver,
    ; OR an Ethernet_Generic-style lib. Confirm which the Waveshare examples use.

build_flags =
    -std=gnu++17
    -DBOARD_HAS_PSRAM

; USB host mode is required for the OP-XY — set USB mode accordingly
; (Arduino IDE equivalent: Tools > USB Mode > "USB-OTG"; in PlatformIO set the
;  corresponding build flags / sdkconfig for TinyUSB host).
```

First-flash note: the Waveshare programming dance if it won't enter download mode — hold BOOT,
tap RESET, release RESET, release BOOT. Their `IO_Test` example is a good first flash to
confirm the board is alive before any project code.

---

## USB architecture decision: ESP32 is the HOST (settled)

This is a decision that wasn't finalized in v3. It is now.

**The ESP32 is the USB host; the OP-XY is a USB device plugged into it.** A second USB-A
receptacle was added for this. Reasoning:

- The board's only USB transceiver lives on IO19/IO20, and the onboard USB-C is hardwired as
  a **device** port (CC pulldowns). You cannot make it a host without reworking SMD resistors.
- A **USB-A receptacle has no CC pins** — the connector type itself fixes the ESP32 as host.
  So adding a USB-A and selecting host mode in firmware is the clean, no-rework path.
- Host mode (vs the ESP32 being a device to the OP-XY) is the right call because it makes the
  bridge a **self-powered standalone box**: it runs on its own power, always drives the Sub 25
  over DIN, listens to Pro DJ Link independently, and can host multiple class-compliant USB
  MIDI devices through a hub if desired. (If the ESP32 were the device and the OP-XY the host,
  the OP-XY would have to power the bridge, coupling everything to the OP-XY being on — rejected.)

Conceptual note for whoever picks this up: **USB host/device ≠ MIDI direction.** Clock flows
ESP32 → OP-XY regardless of roles. The ESP32 being host just means it powers and enumerates
the bus. Firmware runs the USB-OTG core in **host** mode (TinyUSB host via ESP32_Host_MIDI).

### Operational rule (not a wiring issue, a usage rule)

Do **not** connect a computer to the USB-C **and** a synth to the USB-A at the same moment —
both would try to be bus host simultaneously. In practice:
- **Dev/flash/serial:** USB-C to computer, OP-XY unplugged.
- **Normal running:** power via USB-C wall charger (or the synth via USB-A), OP-XY in the USB-A.
  A plain charger only drives VBUS/GND and does not act as a host, so charger-on-USB-C +
  OP-XY-on-USB-A simultaneously is fine. Only a *computer* on USB-C conflicts.

---

## Verified pin map (board silk labels these "IOxx")

The board silkscreens pins as `IO15`, `IO20`, etc. (Espressif "IO" naming). `IO15` == `GPIO15`;
same pin, different prefix. Firmware uses the bare number (e.g. `GPIO_NUM_15` / `15`).

Reserved by the board (do not use):
- **W5500 Ethernet (fixed):** IO9 = RST, IO10 = INT, IO11 = MOSI, IO12 = MISO, IO13 = SCLK,
  IO14 = CS. (Confirmed from schematic.)
- **Native USB:** IO19 = D−, IO20 = D+ (shared with the USB-C connector via 30Ω resistors).
- **Avoid:** IO33–IO37 (octal PSRAM on the R8 chip), strapping pins IO0/IO3/IO45/IO46,
  and IO21 (onboard WS2812 RGB LED — though it can be repurposed as a downbeat indicator
  since it's already there; left unassigned below).

Peripheral assignments (all verified as free, non-strapping GPIO present on the headers):

| Peripheral | Signal | Pin (board silk) | Header | Notes |
|---|---|---|---|---|
| **USB-A → OP-XY** (ESP32 = host) | D+ | IO20 | H2 | native USB |
| | D− | IO19 | H2 | native USB |
| | VBUS (5V out) | `VBUS` | H1 | board's 5V rail; powers OP-XY |
| | GND | `GND` | either | |
| **DIN-5 MIDI out → Sub 25** | UART TX | IO17 | H1 | via 220Ω; MIDI = 31250 baud 8N1 |
| **SSD1306 OLED (I²C)** | SDA | IO15 | H1 | |
| | SCL | IO16 | H1 | |
| | VCC | `3V3` | H1 | 3.3V, NOT 5V |
| | GND | `GND` | either | |
| **EC11 rotary encoder** | A | IO18 | H1 | INPUT_PULLUP |
| | B | IO2 | H1 | INPUT_PULLUP |
| | push (SW) | IO1 | H1 | INPUT_PULLUP |
| | common | `GND` | either | |
| **Button: nudge left** | in | IO38 | H2 | INPUT_PULLUP, other side → GND |
| **Button: nudge right** | in | IO39 | H2 | INPUT_PULLUP, other side → GND |
| **Button: tap tempo** | in | IO40 | H2 | INPUT_PULLUP, other side → GND |

Power pin reference on the board (all on H1): `VBUS` = 5V from USB-C (use this for USB-A VBUS),
`VSYS` = regulator input (don't use for the synth), `3V3` = regulated 3.3V (OLED + logic),
`GND` = ground. The board has no pin literally labeled "5V" — `VBUS` is the 5V.

### USB-A wiring (built and bench-verified)

USB-A receptacle pinout: 1=VBUS, 2=D−, 3=D+, 4=GND.
- pin 1 (VBUS) → board `VBUS` (H1)
- pin 2 (D−) → IO19 (H2)
- pin 3 (D+) → IO20 (H2)
- pin 4 (GND) → `GND`

Optional, only if the OP-XY fails to enumerate on first firmware test: 15kΩ from D+ to GND and
15kΩ from D− to GND (host pull-downs; the ESP32-S3 enables internal ones in software, so try
bare first). Not currently fitted.

**Bench test already performed and PASSED:**
- Continuity good on all four lines (VBUS↔VBUS, GND↔GND, D+↔IO20, D−↔IO19).
- No shorts between any of the four (critically, no VBUS↔GND short).
- Powered on USB-C, measured **+5.0V** (correct polarity) between the breakout's VBUS and GND.
- So the USB-A is electrically safe to connect the OP-XY once host-mode firmware is running.

---

## Firmware mapping to the v3 architecture

The v3 codebase plan is unchanged. Concretely, for this board the platform I/O implementations
(`firmware/`) bind to:

- `UdpW5500 : IUdpSocket` → the onboard W5500. Use the ESP32 Arduino `ETH` API with the W5500
  SPI driver, configured on the SPI pins above (SCLK=13, MISO=12, MOSI=11, CS=14, INT=10,
  RST=9). Then UDP sockets via the standard lwIP/`WiFiUDP`-style API over the ETH interface.
- `MidiUart : IMidiOut` → UART1 TX on **IO17** at 31250 baud → 220Ω → DIN pin 5.
- `MidiUsbHost : IMidiOut` (second output) → TinyUSB host via `ESP32_Host_MIDI`, USB-OTG in
  host mode, native USB on IO19/IO20 routed to the USB-A.
- Clock generation → `uClock` in EXTERNAL_CLOCK mode (replaces the desktop `TimerPosix`):
  `uClock.setTempo(effective_bpm)` from status-packet updates, `uClock.clockMe()` on beat
  packets for phase. The `onSync(PPQN_24, …)` callback fans `0xF8` to BOTH MidiUart and
  MidiUsbHost.

The OLED, encoder, and buttons are Phase-4 UI (see v3) and are not on the critical path for
first sync. Pins are reserved for them above so the wiring is final.

UI behaviors when implemented: encoder selects which player (1–4) is the clock source / shows
master; nudge buttons add/subtract one MIDI clock to shift phase (same idea as the OP-1's
documented nudge); tap tempo is the fallback when `Mv != 0x8000` (no valid rekordbox BPM).

---

## Recommended firmware bring-up order (hardware is ready for all of this)

Do this on the desktop side first per v3 (replay tool → parsers → clock → run against the live
XZ on a Mac) to validate the protocol logic, THEN move to the board. On the board:

1. Flash Waveshare `IO_Test` — confirm the board is alive and pins toggle.
2. Bring up Ethernet: get the W5500 link up, join the Pro DJ Link network, confirm beat
   packets arrive on UDP 50001 (and, once the virtual-CDJ keep-alive is sending, status
   packets on 50002). Log inter-packet timing on first connect to confirm the ~200ms status
   cadence holds on the XZ.
3. DIN MIDI out (IO17) → Sub 25. Easiest output to validate — its arp/sequencer should lock.
4. USB-A host → OP-XY. Set USB-OTG host mode; confirm the OP-XY enumerates and follows clock.
   In the OP-XY: com → M3 devices page, enable clock receive; set it to follow external clock.
5. Tune the PLL phase-correction gain by ear (v3: start `/16`, try `/8` and `/32`).
6. Then layer in the OLED, encoder, buttons (Phase 4).

Bring up one peripheral at a time so each addition is the only thing that could break.

---

## Tooling notes (for reference, not blocking)

- Soldering: Pinecil V2 with the genuine Pine64 short **fine** tip set (ST-BC2, ST-C1, ST-ILS,
  ST-KU). For through-hole/headers/connectors, **ST-KU (large 45° wedge)** is the workhorse;
  ST-BC2 (fine wedge) for tighter spots. Feed the Pinecil from a 65W/20V USB-C PD supply (short
  tips are low-resistance and want the power). 63/37 leaded solder, ~300–320°C.
- Multimeter (AstroAI AM33D): DC volts is the white **V⎓** side; use the `20` range for 5V
  checks. (Yellow V~ side is AC — not used here.)
- Prototyping approach: headers soldered to the board; peripherals on dupont jumpers for
  bring-up; USB-A's four lines soldered (short, clean path). Permanent build later as a crimped
  dupont harness so the board stays removable. Enclosure: laser-cut acrylic stack first
  (M3 brass standoffs), 3D-printed final enclosure once layout is locked.

---

## Quick correction log vs v3

- Synth over DIN is the **Moog Sub 25** (v3 said "Subsequent 27" in places — the DIN wiring is
  identical regardless of model; use IO17 → 220Ω → DIN pin 5).
- Board is the integrated **Waveshare ESP32-S3-ETH**, not DevKitC + separate W5500 module.
- W5500 SPI pins are **fixed by the board** at IO9–14 (not free to reassign as v3's generic
  module wiring implied).
- USB role is **decided: ESP32 = host via added USB-A** (v3 left "USB MIDI host" as a Phase-3
  TODO; it's now the committed architecture with the port built and tested).
