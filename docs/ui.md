# Front-panel interaction reference

The complete control surface of the box: a 128x64 OLED, an EC11 encoder
(spin + push), and three buttons (nudge -, nudge +, tap).

Implementation: the mode machine lives in `ui_task` in
[`firmware/src/main.cpp`](../firmware/src/main.cpp), input decoding and
debouncing in [`ui_input.cpp`](../firmware/src/ui_input.cpp), rendering in
[`ui_display.cpp`](../firmware/src/ui_display.cpp).

## The one idea that organizes everything

**The clock source answers "what drives the tempo."** Every other control
behaves differently depending on whether the box is *following* something or
*owns* the tempo itself:

| Source | Short | Meaning | Who owns the tempo |
|---|---|---|---|
| follower master | `folw` | follow whichever deck holds the DJ-Link master role | the deck |
| player 1-4 | `P1`-`P4` | pin to that deck | the deck |
| sync master | `sync` | the box claims the DJ-Link master role; decks sync to it | **the box** |
| off (standalone) | `off` | link ignored | **the box** |

**`sync master` is hidden unless "Act as player" is enabled** in the settings
menu. Claiming the master role means claiming a player slot on the link, which
is exactly what that setting authorises — and it means a stray encoder spin can
never hijack the master role mid-set. With it off, the source list has six
entries and the box stays a quiet follower.

The select list spells the names out; the status line uses the short forms.
`sync master` and `off` are the *tempo-owner* sources — wherever the tables
below say "tempo-owner", they behave identically.

## Modes

```
                 push encoder
      Normal  ──push encoder──▶  Source-select
        ▲                            │  push = confirm
        │◀───────────────────────────┘  tap = cancel, 10 s idle
        │
        │  hold both nudges 1 s
        ▼
       Menu  ──push──▶  Menu-edit
        │                  │  push = save, tap = cancel
        │◀─────────────────┘
        │
        └──▶ Normal   (tap = back, or 12 s idle; Menu-edit idles out too)
```

## Normal screen

| Control | Follower sources (`folw`, `P1`-`P4`) | Tempo-owner sources (`sync`, `off`) |
|---|---|---|
| **Spin** | nothing | **adjust BPM** by *BPM step* |
| **Tap** (short) | **re-sync**: re-emit MIDI Start on the master's next downbeat, realigning a slave that lost bar alignment. Flashes `RSYN` | **tap-tempo**: averages the last 8 taps (250-2000 ms apart; a longer gap starts a new series) |
| **Nudge -/+** | trim the clock offset by *Offset step*; hold to auto-repeat with acceleration. Persists to NVS ~2 s after it stops changing | same |
| **Hold both nudges ~1 s** | open the Settings menu | same |
| **Push encoder** | open Source-select | same |

### Status line

```
 128.0      BPM        <- big: current clock tempo
            +0.0%      <- pitch % from the master deck
 o o o o   PLAY        <- bar position, transport, [tag]
 src folw   mst 3      <- selected source / who holds DJ-Link master
 off +30.0ms   L rdy   <- clock offset / link + USB state
```

The tag slot shows only what `src` does not already tell you:

| Tag | Meaning |
|---|---|
| `RSYN` | brief confirmation that a re-sync was requested |
| `REQ` | master requested, handshake in flight |
| (blank) | nothing to report |

`mst` shows the device number currently holding DJ-Link master, or `us` when
that is the box.

## Source-select

Spin moves a `>` cursor; the active source keeps its `(on)` marker until you
confirm. **Push** confirms, **tap** cancels, 10 s of inactivity cancels.

Selecting `sync master` runs the tempo-master takeover; selecting anything else
while the box is master releases the role gracefully (it appoints a deck first,
so the link is never left without a master) and restores whatever the box was
doing before. See [architecture.md](architecture.md) for the protocol.

If a deck reclaims master from the box, the bridge steps down on its own and
the panel falls back to `follower master`.

## Settings menu

Spin to scroll, push to edit, spin to change, push to save, tap to back. All
three settings persist to NVS.

| Item | Values | What it governs |
|---|---|---|
| **Act as player** | yes / no | Whether the box may take a player slot on the link. Unlocks `sync master` in the source list. Turning it off while the box is master releases the role first |
| **BPM step** | 0.1 / 0.25 / 0.5 / 1 | BPM per encoder detent when the box owns the tempo (`sync` / `off`) |
| **Offset step** | 0.1 / 0.5 / 1 ms | Clock-offset change per nudge press |

## Deliberate design decisions

- **Tap is overloaded by context, not by timing.** It is re-sync when
  following, tap-tempo when the box owns the tempo, and back/cancel in every
  sub-mode. Each meaning is unambiguous within its mode, and re-sync would be
  meaningless while the box *is* the authority.
- **A tap used as back/cancel is consumed.** Otherwise its release lands in
  Normal and fires a re-sync — a real Stop+Start on the slave just for backing
  out of a menu.
- **The source list is the only mode selector.** There is no separate
  Sync/Free setting: "keep clocking independently" is what `sync master` and
  `off` are for. One list answers "what drives the tempo", and nothing else
  competes with it.
- **One BPM step, not two.** The step only ever applies where the box owns the
  tempo, so a single setting covers it.
- **Nudge auto-repeat accelerates** rather than using a fixed rate, so both a
  single 1 ms trim and a long sweep are practical from the same button.
- **Frames are only pushed when they change.** A full 128x64 transfer is
  1024 bytes, about 25 ms of I2C at 400 kHz, and the UI renders at 25 fps — so
  redrawing an unchanged screen would keep the bus busy continuously. The
  renderer compares the composed buffer against what the panel already holds and
  skips identical frames. Comparing the buffer rather than the snapshot keeps
  that correct regardless of which fields the renderer uses, and an unchanged
  frame is resent every couple of seconds anyway so a glitched panel heals
  itself instead of staying stale forever.
- **Taking master is opt-in, not one spin away.** "Act as player" gates it, so
  the destructive option is absent from the list until you have said you want
  the box on the link as a player.

## History

Earlier revisions had a **Mode (Sync/Free)** menu item and a second BPM-step
setting for a hold-tap + spin gesture that only worked in Free mode. Both were
removed once `sync master` and `off` covered the same ground from the source
list. `Bridge::set_free_run()` still exists in the core but nothing calls it —
it is unused API kept for the moment rather than a desktop-only feature.
