# Contributing

Thanks for your interest. This is a hobby project reverse-engineering Pioneer
Pro DJ Link into MIDI clock; issues and pull requests are welcome.

## Project shape

- **`lib/prolink/`** is the portable, pure-C++17 core (parsers, PLL clock,
  orchestration). It compiles unchanged for both the desktop binary and the
  ESP32-S3 firmware — all I/O is injected through three interfaces
  (`IUdpSocket`, `IMidiOut`, `ITimer`). **A change here affects both targets;
  keep the desktop build green.**
- **`desktop/`** wraps the core in POSIX sockets, RtMidi, and a thread timer.
  It is the reference implementation and the fastest way to iterate.
- **`firmware/`** wraps the core in W5500/lwIP, an `esp_timer` shim, USB MIDI
  host, and a UART DIN output.

See [docs/architecture.md](docs/architecture.md) and
[docs/hardware.md](docs/hardware.md) for the design and board details, and
[ROADMAP.md](ROADMAP.md) for status.

## Building and testing

Desktop (macOS/Linux):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

You can develop offline without any hardware: `xdj_replay` plays a capture
from `captures/` to localhost, and `xdj_bridge --bind 127.0.0.1 --no-vcdj`
runs the bridge against it. `xdj_clockmon` measures a MIDI clock output.

### Tests

The core has a dependency-free unit suite (no gtest needed):

```bash
cmake -S . -B build -DXDJ_DESKTOP=OFF   # core + tests only, no RtMidi/libpcap
cmake --build build --target xdj_tests
./build/tests/xdj_tests                 # or: cd build && ctest --output-on-failure
```

Most of it pins down wire bytes reverse-engineered from real hardware — beat and
status packets, the tempo-master handshake, the device-number claim — because
those offsets have no compiler to protect them. The bridge tests drive the state
machine through fake sockets and a fake clock, and the UI tests cover the
front-panel source gating (`firmware/src/ui_display.hpp` is header-only enough
to compile on the host).

**The bridge is multi-threaded, so run the sanitizers on changes to
`lib/prolink/`** — CI does, and ThreadSanitizer has already caught real races
here:

```bash
cmake -S . -B build-tsan -DXDJ_DESKTOP=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan && ./build-tsan/tests/xdj_tests
```

Anything the UI thread touches that the run loop also owns must go through an
atomic or a request flag the run loop consumes — see `Bridge::set_master_mode`
and `request_resync` for the pattern.

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs the tests on
gcc/clang/macOS, under ASan/TSan/UBSan, plus desktop and both firmware builds.

Firmware (PlatformIO):

```bash
cd firmware
pio run -e waveshare_esp32s3_eth   # production (USB host)
pio run -e diag                    # serial-alive debug build
```

## Conventions

- Match the surrounding code's style, comment density, and naming.
- Update the docs the change affects, in the same change (see
  [CLAUDE.md](CLAUDE.md) for the doc map). A commit hook enforces this for code
  under `lib/`, `firmware/`, and `desktop/`.
- Keep the one-click configs in `.vscode/tasks.json` current when build, flash,
  or run commands change.

## License

The project is under the [PolyForm Noncommercial License 1.0.0](LICENSE):
free for any noncommercial purpose, commercial use by separate arrangement with
the author. By submitting a contribution you agree it is licensed under the
same terms.

## Scope and safety

This project only receives Pro DJ Link broadcasts and emits MIDI clock. Please
keep contributions to that defensive, interoperability-focused scope.
