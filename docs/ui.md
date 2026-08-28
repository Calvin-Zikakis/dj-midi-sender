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

| Source | Meaning | Who owns the tempo |
|---|---|---|
| `auto` | follow whichever deck holds the DJ-Link master role | the deck |
| `P1`-`P4` | pin to that deck | the deck |
| `mstr` | the box claims the DJ-Link tempo-master role; decks follow it | **the box** |
| `off` | standalone, link ignored | **the box** |

`mstr` and `off` are the *tempo-owner* sources. Wherever the tables below say
"tempo-owner", they behave identically.

## Modes

```
                 push encoder
      Normal  ─────────────────▶  Source-select
        ▲  │                          │  push = confirm, tap = cancel, 5 s idle
        │  │  hold both nudges 1 s    │
        │  └──────────────▶ Menu ◀────┘
        │                    │  push = edit an item
        └────────────────────┤  tap  = back, 12 s idle
                             ▼
                         Menu-edit
                      push = save, tap = cancel
```

## Normal screen

| Control | Follower sources (`auto`, `P1`-`P4`) | Tempo-owner sources (`mstr`, `off`) |
|---|---|---|
| **Spin** | nothing on its own | **fine-tune BPM** by *Fine step* |
| **Hold tap + spin** | trim BPM by *BPM step* (Free mode only) | (spin already adjusts BPM) |
| **Tap** (short) | **re-sync**: re-emit MIDI Start to realign a slave that lost bar alignment. Sync = on the master's next downbeat, Free = immediately. Flashes `RSYN` | **tap-tempo**: averages the last 8 taps (250-2000 ms apart; a longer gap starts a new series) |
| **Nudge -/+** | trim the clock offset by *Offset step*; hold to auto-repeat with acceleration. Persists to NVS ~2 s after it stops changing | same |
| **Hold both nudges ~1 s** | open the Settings menu | same |
| **Push encoder** | open Source-select (blocked while tap is held, since that is the BPM modifier) | same |

### Status line

```
 128.0      BPM        <- big: current clock tempo
            +0.0%      <- pitch % from the master deck
 o o o o   PLAY  SYNC  <- bar position, transport, mode tag
 src auto   mst 3      <- selected source / who holds DJ-Link master
 off +30.0ms   L rdy   <- clock offset / link + USB state
```

Mode tag, in priority order:

| Tag | Meaning |
|---|---|
| `RSYN` | brief confirmation that a re-sync was requested |
| `MSTR` | the box holds the DJ-Link tempo-master role |
| `REQ` | master requested, handshake in flight |
| `OFF` | standalone |
| `MAN` | Free mode with a manually latched tempo |
| `FREE` | Free mode, following |
| `SYNC` | following a deck |

`mst` shows the device number currently holding DJ-Link master, or `us` when
that is the box.

## Source-select

Spin moves a `>` cursor; the active source keeps its `(on)` marker until you
confirm. **Push** confirms, **tap** cancels, 5 s of inactivity cancels.

Selecting `mstr` runs the tempo-master takeover; selecting anything else while
the box is master releases the role gracefully (it appoints a deck first, so
the link is never left without a master). See
[architecture.md](architecture.md) for the protocol.

If a deck reclaims master from the box, the bridge steps down on its own and
the panel falls back to `auto`.

## Settings menu

Spin to scroll, push to edit, spin to change, push to save, tap to back. All
values persist to NVS.

| Item | Values | What it governs |
|---|---|---|
| **Mode** | Sync / Free | Whether the clock keeps running when the followed deck stops. **Only affects follower sources** — `mstr` and `off` own the tempo and always keep clocking |
| **BPM step** | 0.1 / 0.5 / 1 / 5 | BPM per detent for **hold-tap + spin** (follower sources, Free mode) |
| **Fine step** | 0.1 / 0.25 / 0.5 / 1 | BPM per detent for **plain spin** when the box owns the tempo (`mstr` / `off`) |
| **Offset step** | 0.1 / 0.5 / 1 ms | Clock-offset change per nudge press |

## Deliberate design decisions

- **Tap is overloaded by context, not by timing.** It is re-sync when
  following, tap-tempo when the box owns the tempo, and back/cancel in every
  sub-mode. Each meaning is unambiguous within its mode, and re-sync would be
  meaningless while the box *is* the authority.
- **A tap used as back/cancel is consumed.** Otherwise its release lands in
  Normal and fires a re-sync — a real Stop+Start on the slave just for backing
  out of a menu.
- **Encoder push is blocked while tap is held** so the hold-tap + spin BPM
  modifier cannot accidentally open Source-select.
- **Two BPM steps exist on purpose.** Coarse (*BPM step*) is for nudging a
  latched tempo while free-running; fine (*Fine step*) is for dialing in a
  tempo the box owns. They are separate menu items because a comfortable
  free-run nudge is usually coarser than a master-tempo trim.
- **Nudge auto-repeat accelerates** rather than using a fixed rate, so both a
  single 1 ms trim and a long sweep are practical from the same button.

## Known rough edges

- **Mode (Sync/Free) is dead in `mstr` and `off`.** The menu still shows and
  saves it; it simply has no effect while the box owns the tempo.
- **`src mstr` and the `MSTR` tag are redundant** — two places on the status
  screen saying the same thing.
- **Source-select has a 5 s idle timeout** which is tight now that the list has
  seven entries.
