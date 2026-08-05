# Working agreement for this repo

## Always update docs before committing

**Before every `git commit`, update the docs that the change affects** — do it
in the *same* commit (or an immediately preceding one), never "later." If a
change alters behavior, hardware wiring, build/flash steps, status, or the
roadmap, the docs must move with it. When in doubt, update.

Doc map — what to touch for what (all public/tracked):

| Doc | Update when… |
|---|---|
| [README.md](README.md) | user-facing behavior, build/flash, controls, feature overview |
| [ROADMAP.md](ROADMAP.md) | status (done vs next) and roadmap changes |
| [docs/architecture.md](docs/architecture.md) | protocol, packet offsets, or clock/PLL design changes |
| [docs/hardware.md](docs/hardware.md) | board, pin map, wiring, resistor values, hardware gotchas |

`docs/local/` holds personal working notes (session log, historical handoff/
context docs) and is **gitignored** — keep it as a private scratchpad if you
use it, but nothing there is published and the doc map above does not depend on
it.

After updating, sanity-check that README + ROADMAP don't contradict the code or
each other.

**Keep the run tasks current.** [`.vscode/tasks.json`](.vscode/tasks.json) holds
the one-click build / flash / monitor / run configs (the `fw: *` tasks for
firmware, the desktop bridge/replay tasks). Whenever a build, flash, monitor,
run command, env, or port changes, update the matching task so the run tasks
keep working.

**Keep the roadmap current too.** The Done / Next lists in
[ROADMAP.md](ROADMAP.md) are the public task tracker — as work progresses, move
finished items to Done and add new follow-ups under Next, so the roadmap always
reflects what's actually left to do.

> A PreToolUse hook (`.claude/settings.json` → `.claude/hooks/docs-gate.sh`)
> enforces the first rule: it blocks a `git commit` that stages code under
> `lib/`, `firmware/`, or `desktop/` without staging any docs.

## Commit conventions

- **Do NOT add a `Co-Authored-By: Claude` trailer** to commit messages.
- Use a HEREDOC for multi-line messages.
- Don't push unless explicitly asked.

## What this project is

A standalone hardware bridge: Pioneer XDJ-XZ Pro DJ Link (Ethernet) → MIDI
clock. The pure-C++17 core in [`lib/prolink/`](lib/prolink/) is shared verbatim
between the desktop binary ([`desktop/`](desktop/)) and the ESP32-S3 firmware
([`firmware/`](firmware/)); only the I/O layer (UDP / MIDI / timer) differs per
platform. **A change in `lib/prolink/` affects both** — keep desktop building.

Current target hardware is a **Waveshare ESP32-S3-ETH** (onboard W5500). See
[ROADMAP.md](ROADMAP.md) for status and [docs/hardware.md](docs/hardware.md)
for the board and wiring.

## Firmware: build, flash, monitor

- Two PlatformIO envs: **`waveshare_esp32s3_eth`** (production, USB-MIDI host
  mode) and **`diag`** (counter-stub MIDI, skips `usb_host_install` so the
  USB-C serial console stays alive). Use `diag` to debug the
  network→master→clock half.
- **Flashing:** the host-mode build claims the USB PHY, so the serial port
  disappears. Enter the ROM bootloader first — **hold BOOT, tap RESET, release
  BOOT** — then `pio run -e <env> -t upload --upload-port /dev/cu.usbmodemXXXX`.
- Native-USB upload **sometimes drops mid-flash** ("Device not configured") —
  just re-run; it usually takes on the second try.
- A piped `pio run | tail` **swallows the real exit code** — end build/flash
  commands with `; echo "EXIT=${PIPESTATUS[0]}"`.
- **Serial monitor:** the system `python3` has no pyserial (externally managed).
  Use PlatformIO's python `~/.platformio/penv/bin/python`, or `pio device
  monitor -e diag`.

## Debugging without serial (host mode)

- In production there is **no serial** — USB-Serial-JTAG and USB-OTG share the
  one internal USB PHY. Diagnostic channels: the **RGB LED state codes**, the
  **OLED**, or flash the **`diag`** build (serial over USB-C).
- Diag-only instrumentation lives in the tree under `#ifdef DIAG_SERIAL_STUB`
  (I²C bus scan, raw input-pin dump, `[ui]` events) — zero cost in production.
  Extend it there rather than adding ad-hoc prints to the production path.
- The approach that's worked: **isolate the pipeline** — `diag` proves
  network→master→clock independently; LED/OLED report USB-enumeration state.

## Mixed arduino + espidf gotchas

- IDF config goes in **`firmware/sdkconfig.defaults`**, *not* `board_build.*`
  keys (silently ignored in mixed mode). After editing it, **delete the cached
  `sdkconfig.<env>`** so it regenerates.
- The build falls back to the generic **`esp32` Arduino variant** (logged at
  build time), whose default Wire pins (SDA 21 / SCL 22) are wrong on the S3 —
  always pass **explicit I²C pins** (e.g. the U8g2 HW-I²C constructor). Do
  **not** use software I²C for the OLED: its per-bit pin-direction toggling +
  IDF gpio logging tripped the task watchdog.
- USB-MIDI host: `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` defaults to **256**,
  too small for real synths' config descriptors — they silently fail to
  enumerate while a USB mouse works. It's bumped to **2048**.

## Hardware facts (this board)

- **Power rule:** with a synth in the USB-A jack, power from a charger/wall, not
  a computer — two hosts on the shared IO19/20 lines conflict.
- **WS2812 LED:** this unit is wired to **GP21** (R15 stuffed); firmware drives
  both GP4 and GP21 to be population-agnostic. R13/R15 select the **LED** data
  source, *not* USB.
- **Status-packet pitch:** use **Pitch1 @ 0x8C**, never Pitch2 @ 0x98 (the
  original handoff's `0x28`/`0x30` were wrong).
