# Session notes / handoff — picking back up

Working scratch doc, not a spec: the *current state of the box* and the
*exact next things to do*, so the next session doesn't re-derive everything.
Canonical references: [architecture.md](architecture.md) (protocol/PLL),
[phases.md](phases.md) (roadmap), [xdj-midi-bridge-context-v4.md](xdj-midi-bridge-context-v4.md)
(hardware addendum + pin map). When this doc disagrees with those on
*hardware/build status*, this doc is newer.

Last updated: **2026-06-02.**

## Where we are right now

**The full bridge works end-to-end on real hardware.** On the Waveshare
ESP32-S3-ETH:

```
XDJ-XZ ─Ethernet─→ W5500 ─SPI─→ ESP32-S3 ─(parse → dual-source PLL → 24 PPQN)
        → USB-A host jack ─USB MIDI─→ synth
```

Verified live: both a **Moog Sub 25** and an **OP-XY** plugged into the USB-A
jack enumerate and lock to the XZ's tempo (LED flashes green on the beat, the
synths follow pitch-fader moves). Phase 1 (desktop binary) is done and is now
reference/debug material.

### What's working
- **Ethernet / W5500** over SPI, static link-local `169.254.42.42/16`, no DHCP.
- **Pro DJ Link reception**, master tracking, dual-source PLL, 24 PPQN clock
  generation. Validated on serial via the `diag` build (`clocks=…` climbing,
  `[beat]`/`[stat]` flowing, `master=1`).
- **USB MIDI host** — enumerates real class-compliant USB-MIDI devices and
  clocks them. See the enumeration fix below.
- **WS2812 status LED** as a downbeat indicator + USB-host-state diagnostic
  (legend below). This board's LED is on **GP21** (R15 stuffed); firmware
  drives both GP4 and GP21 so it's population-agnostic.
- **SSD1306 OLED** (128×64) — live status screen: big BPM, pitch %, beat dots,
  PLAY/STOP, master/source, and the `off` (clock-offset) value. **Use hardware
  I²C, not software** — SW I²C toggles pin direction per bit, the IDF `gpio`
  driver logs each change, and a full frame backed the console up until the UI
  task tripped the task watchdog. `ui_display.cpp` uses the U8g2 HW-I²C
  constructor with explicit pins (it calls `Wire.begin(SDA,SCL)` so the wrong
  default Wire pins on this build don't matter).
- **Nudge buttons** (IO38 −, IO39 +) — each press adjusts the persistent clock
  offset by ±1.0 ms via `Bridge::adjust_clock_offset_ms()`. Verified working on
  hardware (raw-pin + `[ui]` event logging in the `diag` build).

### The one fix that made USB host work (don't lose this)
IDF 4.4's hub driver **aborts enumeration** if a device's configuration
descriptor is larger than `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` (default
**256**, in `hub.c` `ENUM_STAGE_CHECK_SHORT_CONFIG_DESC`). A USB mouse (~50 B)
enumerates; USB-MIDI synths (audio-control + MIDIStreaming + a descriptor per
jack; composite audio+MIDI on the OP-XY) exceed 256, so `NEW_DEV` was never
delivered and our code never saw the device. **Symptom:** mouse → LED red
(`kDeviceNoMidi`), synths → LED stuck blue (`kWaiting`). **Fix:** bumped to
**2048** in `firmware/sdkconfig.defaults`. If a future device still won't
enumerate (stuck blue) but a mouse does, bump it higher (4096) first.

## Hardware: what's wired vs. not

**Wired + working:**
- [x] **USB-A host jack** — D− → IO19, D+ → IO20, VBUS → board `VBUS`, GND → `GND`.
      Bench-verified; clocks the OP-XY / Sub 25.
- [x] **SSD1306 OLED** (128×64) — SDA → IO15, SCL → IO16, VCC → 3V3, GND → GND. Working (HW I²C).
- [x] **Nudge buttons** — IO38 (−), IO39 (+) → GND. Working.

**Partially wired / not working:**
- [ ] **EC11 rotary encoder** — A IO18, B IO2, push IO1. On a **breadboard with
      spotty connections** right now; A/B/push all read stuck-HIGH (`steps=0`),
      i.e. the **common/ground legs aren't reaching GND** (or wrong pin is
      grounded — middle of the 3 is common). **Protoboard is in the mail**;
      redo it there. Firmware is ready (the `diag` build prints a live raw-pin
      dump + `[ui]` events to confirm). Note: if A/B end up swapped it just
      counts backwards — a one-line firmware flip, not a rewire.
- [ ] **Tap button** (IO40) — not installed yet. Firmware reads it (tap-tempo
      computes a BPM) but it's not displayed or fed to the clock.

**Not wired yet** (pins reserved — see v4 pin map):
- [ ] **DIN-5 MIDI out** — IO17 → 220 Ω → DIN pin 5 (Sub 25 was tested over
      *USB*, not DIN; the `MidiUart` sink isn't coded yet either)
- [ ] *(optional)* **USB-UART console adapter** — IO43 (U0TXD) / IO44 (U0RXD)
      / GND, for serial logs while in USB-host mode (see below)

## Known bugs (filed 2026-06-02, to fix next session)

**Bug 1 — MIDI-clock timing jitter on the USB output.** Even after dialing in
the static `off` offset, the OP-XY's timing still *varies*. A static offset can
only cancel a *constant* delay, not jitter. Suspected sources, in order:
- **USB full-speed framing**: each clock byte is a bulk-OUT transfer that goes
  out on the next 1 ms USB frame → up to ~±1 ms quantization per tick (at ~50
  ticks/s that's audible wobble). Prime suspect.
- **tick → queue → sender-task → submit hop** adds variable scheduling latency.
  Try submitting the byte directly from the Clock tick callback, and/or raise
  `sender_task` priority / pin it.
- **PLL gain** (`gain_divisor = 16`) + beat-packet network jitter feeding phase
  corrections — try tuning.
- Minor: the two `led_set()` (WS2812) calls in `on_beat` on the bridge thread.
- *Next session:* instrument the Clock to log per-tick interval jitter; try
  direct-submit from the tick callback; bump sender-task priority; tune gain.

**Bug 2 — OP-XY randomly stops playing.** Intermittently the OP-XY stops mid-
session. Suspected causes:
- **Bar-slip realign** in `lib/prolink/bridge.cpp` sends MIDI **Stop+Start** on
  a detected bar slip; a dropped beat packet (W5500/UDP) → false slip → a Stop
  the OP-XY may not recover from. Prime suspect.
- **Play-state debounce / silence handling**: a brief gap in status packets can
  flip `playing_` or hit `silence_timeout_ms` → spurious MIDI Stop.
- **USB byte drop**: a lost Start after a Stop leaves it stopped (`bytes_dropped`).
- *Next session:* log Start/Stop emissions + bar-slip triggers (diag or UDP),
  measure beat/status drop rate, consider making bar-slip realign not send Stop.

## Next session — exact next steps

1. **Bug 1 — kill the clock jitter** (see Known bugs). Biggest quality win;
   start by instrumenting per-tick interval and trying direct-submit from the
   tick callback.
2. **Bug 2 — stop the OP-XY dropping out** (see Known bugs). Look hard at the
   bar-slip Stop+Start path first.
3. **Finish the encoder** once the protoboard arrives — redo the wiring off the
   breadboard, confirm `src` cycles (watch the `diag` raw-pin dump), then it's
   done (firmware already handles it).
4. **Install + wire the tap button** (IO40) and decide tap-tempo behavior
   (display only vs. drive the clock when `Mv != 0x8000`).
5. **Wire + code DIN MIDI out** (IO17 → 220 Ω → DIN 5) so the Sub 25 can clock
   over DIN simultaneously with USB. Both sinks share the one PLL tick callback.
6. *(optional, anytime)* Wire the **UART console** so we're not LED-only in
   host mode.

## Build / flash / debug reference

```bash
cd firmware

# production (USB host mode; serial console is DEAD once it boots)
pio run -e waveshare_esp32s3_eth -t upload --upload-port /dev/cu.usbmodemXXXX

# diag (counter-stub MIDI, skips usb_host_install → USB-C serial stays alive;
# use this to debug the network → master → clock half)
pio run -e diag -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor -e diag       # or: ~/.platformio/penv/bin/python with pyserial
```

**Flashing gotchas:**
- In **production/host mode the serial port doesn't enumerate** (USB-OTG takes
  the shared USB PHY from USB-Serial-JTAG). To flash, force the ROM bootloader:
  **hold BOOT, tap RESET, release BOOT**, then upload. The port shows up as
  `/dev/cu.usbmodemXXXX`.
- Native-USB upload **sometimes drops mid-flash** (“Device not configured”).
  Just **re-run the upload** — it usually takes on the second try.
- **Power rule:** when a USB device is in the USB-A jack, power the board from
  a **charger/wall**, NOT a computer — a computer on USB-C is a second host on
  the shared IO19/20 lines and they fight. Charger = VBUS only = fine.

**LED legend (production/host build):**
| Color | Meaning |
|---|---|
| yellow (brief) | booted |
| dim **blue** | host driver up, **no device enumerated** |
| **red** | a device enumerated but exposed **no MIDI interface** |
| **magenta** | USB host driver failed to install |
| **green** flashes on the beat | **working** — enumerated + clocking |

## Why there's no serial in host mode (and the options)
USB-Serial-JTAG and USB-OTG share the **one** internal USB PHY on IO19/20, so
installing the host driver kills the USB-C console. You **cannot** view data by
plugging the USB-A into a computer either (both would be hosts). Options to see
logs while hosting: (a) a **USB-UART adapter on IO43/44** + repoint the console
to UART0 (`CONFIG_ESP_CONSOLE_UART_DEFAULT`); (b) **UDP-over-Ethernet logging**
(needs the Mac on the same Ethernet switch as the board + XZ); (c) the **LED
codes** above. For most network/clock debugging, just flash the `diag` build
and watch USB-C serial.

## Known issues / follow-ups
- **Clock jitter + OP-XY dropouts** — see Known bugs above (the two priorities).
- **OLED: hardware I²C only.** SW I²C tripped the task watchdog (see What's
  working). If the screen ever goes blank + the board reboots, that's the
  regression to check first.
- **Encoder** on a breadboard, not yet functional — common/ground; protoboard inbound.
- **DIN MIDI out** not implemented (`MidiUart` sink + wiring).
- **Diag instrumentation is in the tree** (all under `#ifdef DIAG_SERIAL_STUB`,
  zero cost in production): I²C bus scan at boot, a 2 Hz raw input-pin dump, and
  `[ui]` encoder/button event logs. Flash `-e diag` and watch serial to debug
  the OLED/encoder/buttons.
- **uClock was dropped:** the firmware uses a native `esp_timer` one-shot shim
  (`firmware/src/timer_esp.cpp`) as `ITimer`, not uClock (uClock 2.2.x needs
  arduino-esp32 v3 timer APIs; this platform ships v2.x). phases.md still
  describes the old uClock plan.
- **Bar-slip / drift recovery** (Phase 1.5) exists in `lib/prolink/` but hasn't
  been provoked/verified on the firmware.
- The board ships one of two 0 Ω LED jumpers (R13→GP4 default, R15→GP21); this
  unit is **GP21**. R13/R15 are LED selectors, **not** USB selectors (an
  earlier doc got that wrong).
