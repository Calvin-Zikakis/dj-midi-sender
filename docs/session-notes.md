# Session notes / handoff — picking back up

Working scratch doc, not a spec: the *current state of the box* and the
*exact next things to do*, so the next session doesn't re-derive everything.
Canonical references: [architecture.md](architecture.md) (protocol/PLL),
[phases.md](phases.md) (roadmap), [xdj-midi-bridge-context-v4.md](xdj-midi-bridge-context-v4.md)
(hardware addendum + pin map). When this doc disagrees with those on
*hardware/build status*, this doc is newer.

Last updated: **2026-06-01.**

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

**Wired so far: only the USB-A host jack.** Bench-verified (continuity, no
shorts, +5 V polarity):
- USB-A D− → IO19, D+ → IO20 (native USB, parallel with the USB-C jack)
- USB-A VBUS → board `VBUS`, GND → `GND`

**Not wired yet** (pins reserved — see v4 pin map):
- [ ] **DIN-5 MIDI out** — IO17 → 220 Ω → DIN pin 5 (Sub 25 was tested over
      *USB*, not DIN; the `MidiUart` sink isn't coded yet either)
- [ ] **SSD1306 OLED** (I²C) — SDA IO15, SCL IO16, VCC 3V3
- [ ] **EC11 rotary encoder** — A IO18, B IO2, SW IO1 (INPUT_PULLUP)
- [ ] **Nudge / tap buttons** — IO38, IO39, IO40 (INPUT_PULLUP → GND)
- [ ] *(optional)* **USB-UART console adapter** — IO43 (U0TXD) / IO44 (U0RXD)
      / GND, for serial logs while in USB-host mode (see below)

## Next session — exact next steps

1. **Measure the USB latency vs. the desktop version, then compensate.** The
   ESP32 path has a different (slightly higher) offset than the Mac. Measure:
   record XDJ master out + the synth's output into Audacity/any DAW, read the
   transient gap in ms; do it for desktop and ESP32, the difference is the
   number to dial in. Then **implement a static clock phase-offset** (ms or
   fractional ticks applied to all outgoing clock), and **wire the three nudge
   buttons** (IO38/39/40) for live ±1-tick shifts like the desktop GUI.
2. **Wire + code DIN MIDI out** (IO17 → 220 Ω → DIN 5) so the Sub 25 can clock
   over DIN simultaneously with USB. Both sinks share the one PLL tick callback.
3. **Wire the UI** (OLED, encoder, buttons) — Phase 4. Pins above.
4. *(optional, anytime)* Wire the **UART console** so we're not LED-only in
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
- **USB latency offset** vs desktop — measure + compensate (step 1 above).
- **DIN MIDI out** not implemented (`MidiUart` sink + wiring).
- **uClock was dropped:** the firmware uses a native `esp_timer` one-shot shim
  (`firmware/src/timer_esp.cpp`) as `ITimer`, not uClock (uClock 2.2.x needs
  arduino-esp32 v3 timer APIs; this platform ships v2.x). phases.md still
  describes the old uClock plan.
- **Bar-slip / drift recovery** (Phase 1.5) exists in `lib/prolink/` but hasn't
  been provoked/verified on the firmware.
- The board ships one of two 0 Ω LED jumpers (R13→GP4 default, R15→GP21); this
  unit is **GP21**. R13/R15 are LED selectors, **not** USB selectors (an
  earlier doc got that wrong).
