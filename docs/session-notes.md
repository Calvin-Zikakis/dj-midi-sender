# Session notes — picking back up

This is a working scratch doc, not a spec. It captures the *current state of
the bridge* and the *exact next thing to test* so the next session doesn't
re-derive everything. The canonical references are still
[architecture.md](architecture.md) and [phases.md](phases.md); when this
doc disagrees with those, those are right (or this doc is stale).

## Where we are right now

Phase 1 desktop binary is feature-complete on paper: `xdj_bridge` /
`xdj_replay` build clean, the GUI works, two-axis offsets persist. The last
*live* test against the XDJ-XZ was the run that produced
[the GUI screenshot showing pitch = 16.1252×](#the-screenshot-bug) — i.e.
status packets were flowing but the bridge was reading the wrong byte
offsets and the displayed BPM was garbage. The clock still tracked
"decently" because the beat-only fallback was carrying tempo while the
status path was effectively no-ops.

**Everything below this point has been fixed in code since that test but
has not yet been re-run live.** First task next session: run the live test
again, see whether the GUI now shows the correct track BPM + a sensible
pitch multiplier near 1.0×, and whether the master / playing / sync /
on-air flag dots light up.

## What changed in this session (chronological)

Five distinct bugs were found and fixed in this order. Each one masked the
next, which is why it took several test cycles:

1. **Phase correction had the wrong sign**
   [`lib/prolink/clock.cpp`](../lib/prolink/clock.cpp) — `correct_phase()`
   was computing `err = -t * period - off`. When the clock was ahead of
   the beat, this produced a negative error that *sped up* the clock
   further, instead of slowing it down. The PLL was unstable in beat-only
   mode. Fixed to `err = t * period - off`.

2. **Tempo froze the moment the clock started**
   [`lib/prolink/bridge.cpp`](../lib/prolink/bridge.cpp) — the beat-only
   tempo update path was guarded by `!playing_.load()`, so once the clock
   started it stopped tracking the pitch slider. Removed the guard so
   `effective_bpm()` from beat packets keeps updating tempo continuously.

3. **Timer jitter from `sleep_until`**
   [`desktop/timer_posix.cpp`](../desktop/timer_posix.cpp) — `std::this_thread::sleep_until`
   on macOS has ~1-2ms variance, which read as ±5 BPM in the GUI's 0.5s
   sample window. Replaced with `mach_wait_until` + `thread_policy_set`
   (the macOS real-time scheduling class CoreAudio uses) + a final ~50µs
   busy-spin. Sub-100µs jitter now.

4. **Keep-alive socket bound to `0.0.0.0` — broadcast `sendto` failing intermittently**
   [`desktop/main.cpp`](../desktop/main.cpp) — without an interface bind,
   macOS sometimes couldn't find a route to `169.254.255.255` (link-local
   broadcast) and returned `ENETUNREACH`. Fixed by binding the keepalive
   socket to the discovered interface IP (e.g. `169.254.239.191:0`)
   instead of `0.0.0.0:0`.

5. **Keep-alive packet layout was wrong — XZ ignored every announce**
   [`lib/prolink/packets.cpp`](../lib/prolink/packets.cpp) → `build_keepalive_packet()` —
   our packet was off-by-one from byte 0x0B onward (we omitted the
   padding byte between `type` at 0x0A and `model` at 0x0C) and missing
   several constants the XZ checks for (`u1=0x01`, `device_type=0x02`,
   subtype `0x36`, `device_count=0x02`, `flags=0x01`, `u4=0x64`).
   Rewrote the builder to match python-prodj-link's verified layout byte
   for byte. Also changed default device number from `7` → `5`
   (python-prodj-link's working default).

   **This is the fix that unlocked status packets.** Confirmed in the
   last test — 2380 status packets received in the session.

6. **Status packet field offsets were ~104 bytes off**
   [`lib/prolink/packets.cpp`](../lib/prolink/packets.cpp) → `parse_status_packet()` —
   the offsets that came in from the initial handoff doc (`0x28`, `0x30`,
   `0x5A`) were from an unverified source and did not match beat-link,
   dysentery, or python-prodj-link. We were reading
   `loaded_player_number + loaded_slot + track_analyze_type` as a fake
   "pitch", which is where the 16.1252× GUI value came from.

   Corrected offsets (triangulated from beat-link `CdjStatus.java`,
   python-prodj-link `packets.py`, and dysentery):
   - `Pitch1` (effective pitch — use this): **0x8C** (4 B)
   - `Pitch2` (local fader — ignore): **0x98** (4 B)
   - BPM × 100: **0x92** (2 B)
   - `Mv` / BpmState: **0x90** (2 B)
   - Flags (low byte of state u16 at 0x88): **0x89** (1 B) — bits unchanged
   - Beat-in-bar: **0xA6** (1 B) — unchanged

Bridge-level features added alongside these:
- **Bar-slip detection** in `Bridge::handle_beat_packet` — tracks expected
  `beat_in_bar` sequence (1→2→3→4→1); if a beat packet is dropped, the
  bridge queues a hard `MIDI Stop` + `MIDI Start` on the next downbeat to
  realign the slave's bar 1 with the master's. Resets cleanly on
  master-pause and silence-timeout. This is the feature for the user's
  question "should we reset off the first beat occasionally".
- **PLL default gain** bumped 16 → 32 (smoother per-tick corrections;
  still overridable with `--gain`).
- **Verbose keepalive logging** in the bridge (`[bridge] keepalive → ip:port device=N ok/FAILED`)
  for diagnosing connectivity.
- VS Code `.vscode/launch.json` and `.vscode/tasks.json` with one-click
  run configs for live / replay / tcpdump tasks.

## The screenshot bug

For reference, this is what the last live run looked like:

```
Track BPM:    0.00 × 16.1252  (+1512.52 %)
Effective:    0.00 BPM
Flags:        master=·  playing=·  sync=·  on-air=·
Mv valid:     yes  (0x8000)
Packets:      311 beat / 2380 status
```

- `Mv valid: yes` proved we *were* receiving and parsing real status
  packets (0x90 was the one offset that happened to be right).
- BPM = 0 proved 0x5A is wrong (lands in a 32-byte padding region).
- Pitch = 16.1252 proved 0x28 is wrong (lands on
  `loaded_player_number/loaded_slot/track_analyze_type`).
- All-unlit flags is consistent with the bridge reading garbage and
  passing it through `is_playing()` / `is_master()`, which then return
  false. With the offset fix this should clear up.

## Next test, exactly

1. Run task **"run: bridge — live verbose + GUI"** with the XZ playing a
   rekordbox track at a known BPM.
2. In the **Master** panel verify:
   - **Track BPM** shows the real BPM (115 in the user's earlier test).
   - **Pitch multiplier** is near 1.0× when the slider is at 0.
   - **Flags**: `playing` is lit. `master` should also be lit since the
     XZ is the only deck. `sync` and `on-air` depend on XZ state.
3. Move the XZ pitch slider. The Effective BPM in the GUI should follow
   smoothly within ~200 ms (status packet cadence) — *not* stepped every
   beat like the beat-only path used to do.
4. In the **MIDI clock out** panel, watch the tick rate. With status
   packets driving tempo, the rate should be much tighter than the ±5 BPM
   wobble we were seeing. Phase error should settle near 0 within a few
   beats of starting playback.
5. Listen to the OP-XY. The sequencer should hold time through pitch
   sweeps without manual intervention.

## What's still on the Phase 1 punch list

From [phases.md](phases.md), unticked items at the start of this session
that remain unticked (re-evaluate after the next live test):

- [ ] Live test confirming status packets unlock + tempo tracks pitch slider smoothly
- [ ] Tune PLL gain by ear (`/16` snappy vs `/32` smooth vs `/64` smoother). Default is now `/32`.
- [ ] Full pitch-sweep live test — OP-XY must hold time and bar alignment without intervention
- [ ] Calibrate OP-XY clock offset by ear and confirm persistence across restart
  (the persistence side already works — last test loaded `+9.0 ms` for OP-XY automatically)

## Known issues / follow-ups

- **Architecture doc had wrong status-packet offsets.** Fixed in this
  session. If we ever find another field offset that looks plausible but
  isn't tested live, that's still suspect — the original doc's offsets
  cited python-prodj-link as a source but didn't match.
- **Bar-slip detection isn't tested live yet.** The code path runs only
  when a beat packet is missed; we won't know it works until we actually
  drop a beat or get a real-world slip. Worth provoking deliberately
  (e.g. by briefly unplugging the ethernet) once basics are confirmed.
- **The handshake question is open but probably moot.** Dysentery
  documents a 4-stage announce handshake (types 0x0A → 0x00 → 0x02 →
  0x04 → 0x06 keepalive). python-prodj-link skips stages 1-4 and only
  sends 0x06 — and it works. We do the same. If the XZ ever refuses to
  respond again, implementing the full handshake is the obvious next
  lever to pull. Skip until needed.
- **`ms_next_beat` is parsed but unused.** Beat packets carry the exact
  time until the next beat. Could be used as a feedforward to lock tick 0
  of each beat precisely. Current implementation just lets the PLL
  converge. Probably good enough; revisit only if jitter is still
  audible after status packets are working.
- **CDJ-3000 compatibility** of the keepalive packet uses the
  `device_count=2` / `u4=0x64` defaults from python-prodj-link. If we
  ever test against a CDJ-3000 specifically, double-check those.

## Build / run reference (in case context cache is cold)

```bash
# build
cmake --build build

# live, GUI, verbose (the main task)
./build/desktop/xdj_bridge --midi-port 'OP-XY' --visualize --verbose

# offline (replay capture in T1, bridge in T2)
./build/desktop/xdj_replay --file captures/xdj-xz-export-mode-pitch-sweep.pcapng --loop
./build/desktop/xdj_bridge --bind 127.0.0.1 --midi-port 'OP-XY' --no-vcdj --verbose
```

VS Code: Cmd+Shift+P → "Tasks: Run Task" → pick. tcpdump tasks need sudo.

## Files touched this session

```
desktop/main.cpp                 # device_num default 5; keepalive socket bound to iface IP; default gain 32
desktop/timer_posix.cpp          # mach_wait_until + thread_policy_set
lib/prolink/bridge.cpp           # tempo always updates from beats; verbose keepalive log; bar-slip detection
lib/prolink/bridge.hpp           # default device_num 5; bar-slip state fields
lib/prolink/clock.cpp            # phase correction sign fix
lib/prolink/packets.cpp          # keepalive packet layout rewrite; status packet offset fix
lib/prolink/packets.hpp          # build_keepalive_packet param rename
docs/architecture.md             # corrected status-packet offset table; corrected Pitch1 reference
.vscode/launch.json              # new — debug configs (cppdbg/lldb)
.vscode/tasks.json               # new — one-click run / tcpdump tasks
```

No commits have been made; everything is on the working tree.
