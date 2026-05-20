# Wiring handoff — USB-A host jack + UART console breakout

Picking up from the 2026-05-20 session. The firmware is feature-complete
end-to-end on `main` (network → parse → PLL → 24 PPQN tick → USB MIDI
host), but the **Waveshare ESP32-S3-ETH dev board's USB-C jack hardware-
pins us to the USB device role** (5.1 kΩ CC pulldowns). The OP-XY plugs
into a USB-C cable expecting to be the host and wins, so our host code
never gets to enumerate it.

This doc is the plan to unblock that with two soldering jobs:

1. **Add a USB-A jack** wired to the ESP32-S3's native USB pins
   (GPIO19/20). USB-A is mechanically a host connector with no CC pins —
   role ambiguity disappears.
2. **Add a USB-UART breakout** on GPIO43/44 (U0TXD/U0RXD) so we still
   have a serial console after USB-OTG is repurposed for the host jack.

Reference schematic:
- **PDF:** https://files.waveshare.com/wiki/ESP32-S3-ETH/ESP32-S3-ETH-Schematic.pdf
- **Wiki:** https://www.waveshare.com/wiki/ESP32-S3-ETH
- A local copy was downloaded during the 2026-05-19 session — re-fetch
  with `curl -O` if needed.

---

## Parts and tools

| Part | Notes |
|---|---|
| USB-A female panel/PCB jack | Any USB 2.0 receptacle. Cheap from Adafruit / Amazon. |
| 2× 5.1 kΩ resistors (optional) | Only if we want to add USB-A's nominal CC equivalent — not needed for host signalling on USB-A which has no CC pins. |
| USB-to-UART adapter | FTDI / CP2102 / CH340. 3.3 V signalling. |
| Dupont jumpers or hookup wire | Thin enough to fit pin headers. |
| Soldering iron, flux, fine solder | 0402 work — fine-tip iron recommended. |
| Magnifier or microscope | R13 / R15 are 0402 — easy to mistake adjacent pads. |
| Multimeter (continuity mode) | To verify which side of the R13/R15 footprint is which net before lifting. |

---

## Job 1 — Route GPIO19/20 from USB-C to a new USB-A jack

### Background on the selection resistors

From the schematic (Type-C section, near the ESP32-S3 module):

| Designator | Default value | Function |
|---|---|---|
| `R13` | **0 Ω (populated)** | routes ESP32-S3 GPIO19/20 → USB-C D-/D+ |
| `R15` | **NC (not populated)** | routes ESP32-S3 GPIO19/20 → header H2 pins 1-2 |

The two resistor positions are alternates — exactly one should be
populated at a time. Default ships with R13 populated so USB-C is the
data path.

The schematic data shows `R15 NC/0R` and `R13 0R/NC` — the slash
indicates "default / alternate" populations. Confirm visually with a
microscope before desoldering: R13 should currently have a 0 Ω jumper
across it, R15 should have empty pads.

### Step-by-step

1. **Confirm with multimeter** continuity from header H2 pin 1 to
   ESP32-S3 GPIO19 net. (If R13 is populated, you'll see continuity
   from USB-C D- to GPIO19 instead.) Identify R13's pads before lifting.
2. **Desolder R13** (lift cleanly — it's a 0402 0 Ω jumper). The USB-C
   jack now has no data signal path; it becomes a power-only port.
3. **Solder a 0 Ω jumper (or short blob of solder) onto R15's pads** to
   close that route. Now GPIO19 → H2 pin 1, GPIO20 → H2 pin 2.
4. **Wire from H2 to the new USB-A jack:**
   - H2 pin 1 (GPIO19) → USB-A pin 3 (D-)
   - H2 pin 2 (GPIO20) → USB-A pin 2 (D+)
   - USB-A pin 4 (GND) → any GND on the board (H1 has multiple)
   - **USB-A pin 1 (VBUS)** → tap the **USB-C VBUS net** (USB_VCC) so
     the board can supply 5 V to whatever's plugged into USB-A. Easiest
     tap: the cathode side of D1 (1N4148, which sits between the USB-C
     VBUS rail and the 3V3 regulator input). Verify the tap point with
     the schematic and a multimeter before soldering.

> ⚠️  **Power consideration:** with USB-A acting as host, the board now
> **supplies** VBUS to whatever's plugged in. The OP-XY (which used to
> power us through USB-C) can't do that anymore — the board needs an
> upstream power source on USB-C (laptop / wall wart) or PoE. Plan a
> power topology before plugging anything in.

### Verifying the swap before plugging anything important in

- Power the board via USB-C from a wall wart or laptop.
- Don't plug anything into USB-A yet.
- Check USB-A's VBUS pin reads ~5 V with a multimeter.
- Check D+ and D- both read low (~0 V) when no device is plugged in.
- Plug a known class-compliant USB MIDI device into USB-A. The
  `MidiHostUsb` enumeration should run; over the UART console (Job 2)
  you should see `[usb-midi] MIDI device attached (addr=N, intf=…)`.

---

## Job 2 — Solder a USB-UART adapter to GPIO43/44

The USB-Serial-JTAG console on USB-C dies once GPIO19/20 are routed to
USB-A. UART0 (GPIO43 / GPIO44) is still routed to header H1 and can
serve as our development console. arduino-esp32 doesn't direct `printf`
there by default — see the firmware change in [Step 4](#step-4--firmware-tweak-route-printf-to-uart0).

### Step-by-step

1. **Identify the right header H1 pins** from the silkscreen (or schematic):
   - `U0TXD` (ESP32-S3 GPIO43) → adapter's **RX** pin
   - `U0RXD` (ESP32-S3 GPIO44) → adapter's **TX** pin
   - `GND` → adapter's **GND**
2. **Optionally tie the adapter's DTR / RTS** to nothing for now — the
   ROM bootloader's auto-reset works via these lines on traditional
   esp32 boards, but the ESP32-S3 native USB layout doesn't expose them
   in the same way. Manual BOOT + EN button presses for flashing.
3. **Solder pins to H1** (or use a temporary breadboard jumper while
   testing).
4. Plug the adapter into your laptop. `ls /dev/cu.usbserial-*` or
   `cu.SLAB_*` should show the adapter's tty.

### Flashing over UART (replacing `pio run -t upload`)

```
pio run -t upload --upload-port /dev/cu.usbserial-<X>
```

You'll need to put the board into bootloader mode manually for each
flash:
1. Hold `BOOT` button.
2. Press `EN` (reset) once and release.
3. Release `BOOT`.
4. Run `pio run -t upload --upload-port /dev/cu.usbserial-<X>`.

If `pio device monitor` doesn't auto-reset the chip on each upload,
that's fine — just BOOT+EN before each flash. Add `monitor_speed=115200`
+ `monitor_port=/dev/cu.usbserial-<X>` to `firmware/platformio.ini` once
the tty is stable.

---

## Step 4 — Firmware tweak: route printf to UART0

Right now `printf` goes to whichever console ESP-IDF picked at boot
(USB-Serial-JTAG by default, on GPIO19/20). After Job 1, those pins are
the USB-A data lines and the console isn't usable.

Two-line change in `firmware/sdkconfig.defaults`:

```
# Route the ESP-IDF console to UART0 (GPIO43/44 via H1) instead of
# USB-Serial-JTAG, which conflicts with USB-OTG host mode on GPIO19/20.
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n
```

Delete `firmware/sdkconfig.waveshare_esp32s3_eth` (the cached generated
file) and rebuild so the change propagates. See the
`firmware_mixed_framework` memory entry for why.

---

## End-state hardware diagram

```
                                       ┌──── USB-A ──── OP-XY / Digitakt / …
                                       │       (host:  we supply VBUS)
                                       │
                            ┌─ GP19/20 ┘  (was USB-C; now via R15)
            ESP32-S3R8 ─────┤
                            ├─ GP43/44 ──── UART0 ──── USB-UART adapter ──── laptop
                            │                                                 (dev console + flashing)
                            └─ SPI ─── W5500 ─── RJ45 ─── XDJ-XZ Pro DJ Link

   USB-C jack (data lines lifted via R13 removal) ─── power-only input
                                                     (laptop / wall wart / PoE)
```

---

## Smoke test plan once both jobs are done

1. Power-only USB-C plugged in (laptop), UART adapter plugged in to a
   different USB port.
2. Ethernet cable to XDJ-XZ.
3. Open serial monitor on the UART tty (115200 baud).
4. Boot — should see `[xdj-bridge] firmware — full Bridge integration +
   USB MIDI host` followed by ethernet init, IP assignment.
5. Plug OP-XY into the new USB-A jack. Console should print
   `[usb-midi] MIDI device attached (addr=N, intf=…)`.
6. Play a track on the XZ. Console:
   - `[stat]` line on play-state transition
   - `[beat]` line per beat (~2 Hz)
   - `[status] link=up usb_dev=attached clocks=… sent=… dropped=0`
7. OP-XY should show clock-receive activity and lock to the XZ's
   tempo. Pitch-slider sweeps should track on the OP-XY in real time.

If `usb_dev=none` despite plugging the OP-XY in, the enumeration failed —
look for `[usb-midi]` errors and consult `midi_host_usb.cpp`. If
`dropped` keeps climbing, the OP-XY isn't draining the bulk-OUT endpoint
(probably needs clock receive enabled on its side).

---

## Open hardware questions for later (deferred)

- **WS2812B LED only shows green.** Schematic labels U_WS_L1 as
  `WS2812B TBD` — the actual chip on the board may be a single-color
  green LED, not a real WS2812B. Investigate with a logic analyzer on
  GPIO4 to see if the standard 800 kHz GRB protocol is being driven
  correctly. If so, the chip is the problem and can be reflowed with a
  proper WS2812B.
- **DIN MIDI output** (Sub 27 path). Per [docs/phases.md](phases.md)
  Phase 2, the box should also have a 5-pin DIN female jack on
  HardwareSerial1 (GPIO17 default) → 220 Ω → DIN pin 5. Lower priority
  than USB host since you don't have a Sub 27 ready to clock yet.
