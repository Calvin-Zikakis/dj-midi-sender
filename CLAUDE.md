# Working agreement for this repo

## ⚠️ Always update docs before committing

**Before every `git commit`, update the docs that the change affects** — do it
in the *same* commit (or an immediately preceding one), never "later." If a
change alters behavior, hardware wiring, build/flash steps, status, or the
roadmap, the docs must move with it. When in doubt, update.

Doc map — what to touch for what:

| Doc | Update when… |
|---|---|
| [README.md](README.md) | user-facing behavior, build/flash, controls, feature status |
| [docs/session-notes.md](docs/session-notes.md) | **the live handoff** — current state, what's wired, bugs, next steps. Update for almost every change. |
| [docs/phases.md](docs/phases.md) | roadmap / phase status changes |
| [docs/architecture.md](docs/architecture.md) | protocol, packet offsets, or clock/PLL design changes |
| [docs/xdj-midi-bridge-context-v4.md](docs/xdj-midi-bridge-context-v4.md) | hardware: board, pin map, wiring, what's physically built |
| docs/xdj-midi-bridge-context-v3.md, docs/handoff.md | historical/reference — leave unless the protocol research itself changes |

After updating, sanity-check that README + session-notes don't contradict the
code or each other.

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
[docs/session-notes.md](docs/session-notes.md) for live status and the
flash/debug gotchas (host mode hides the serial port → BOOT+RESET dance; no
serial in host mode; charger-not-computer power rule; `diag` env for serial).
