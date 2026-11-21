# F-code chapters

STATUS (2026-11-21): QxxR/N, ?C, ?P, C0/C1 on master a9fcb8d0.  QxxS and
the A1/B1 reply change BUILT on branch `fcode-qxxs`, host-tested with
controls (the FILM-sequence test fails 24/90 checks on master's fcode.c, and
6 - exactly *AUDIO 1/2/3 - with QxxS but the old A1/B1 reply).  NOT
hardware-tested.  All configs build warning-free.

## What

The VP415's chapter F-codes, from a per-side `chapters.txt` beside
`video.pvf` (format in `src/chapters.h` and docs/user/screen-and-video.md):

| F-code | Reply | Behaviour |
|---|---|---|
| `QxxR` | `A6` / `AN` | goto the chapter's first picture, halt (`videoplayer_goto(p,'R')`) |
| `QxxN` | `A6` / `AN` | the same, then play (`'N'`) |
| `QxxS` | `A2` at the end / `AN` | play the chapter: goto its first picture and play, stop register at its last (next chapter's first - 1, or past the disc's end for the last chapter, which the register clamps). No reply until the stop is reached, as `FxxxxxS` |
| `?C` | `Cxx` / `X` | chapter of the picture on screen; `X` with no player, no table, or before the first chapter; `O` while the tray is open comes from `fcodeReadBuffer` as for every reply |
| `?P` | | 2nd status character bit 2 = "chapter numbers exist" when the side has a table |
| `C0`/`C1` | | the player's 2-digit chapter overlay; `C1` turns the picture number off, `D0`/`D1` turn the chapter number off (VP415 manual) |

`AN` also answers a malformed `Q`, three digits, and a list of several
chapters `QxxyyzzS` (not built: nothing known sends one).

### Who sends QxxS (domsday-95's decode, 2026-11-21)

- VFS 2.02 `*CHAPTER n` (&AB63) appends `S` when no R/N/S is given, then
  sends E1 without reading a reply: plain `*CHAPTER 3` is `Q3S`.
- Domesday National A's FILM overlay (routine G612) plays a National B
  sequence with VP1, a read that drops any stale reply, `QxxS`, `*AUDIO n`,
  `E1`; then reads without blocking every tick and takes the first reply
  starting with `A` (any digit) as "film over", sending `X` and `/`.

So nothing may leave an `A` reply waiting before the chapter ends.  VFS
`*AUDIO n` sends `A<n&1>` then `B<n>>1>` and never reads a reply, and A1/B1
used to post `A`: every film with sound would have ended on the first poll.
A1/B1 now post nothing, like A0/B0.  FILM was written against a real VP415,
so the real player cannot have answered A1/B1 with an A (INFERRED - no
manual text found; the service guide has no command set).  The `A2` digit
at the chapter end is ours (as FxxxxxS); FILM does not look at it.

## Where

- `src/chapters.c` - pure parse and lookup, host-tested in `src/tests/chapters`
  against the real tables (BGB S1/S2, National B).
- `src/videoplayer.c` owns the table: `vp_chapters()` reads the file the
  first time something asks for the current VFS directory and keeps it until
  the directory changes or `videoplayer_media_changed()` marks it stale.  No
  SD work at boot or BBC reset; a side without the file costs one failed
  open.  It does not need the video open - `?P` asks first.
- `src/BeebSCSI/fcode.c` only calls the player API.

## Units

The picture number is the player's own: video.pvf record index + 1.  On a
CAV side that is the disc's picture number; on a CLV side (National B) the
domsday tooling converts each chapter's start into it.  The tables come
from the IEC 60857 chapter codes in the VBI (domsday session, files under
/mnt/s/domsday/aiv2026/chapters/<slot>/).

## Open

- BGB's record-0 = picture-1 mapping is INFERRED (record counts equal the
  highest picture number).  Hardware check: `Q03R` on BeebVFS19, then `?F`
  must answer `F34798`, and the picture must be the chapter's first.
- National B's chapter starts are VBI codes; that the frame is exact is not
  proven (a scene change follows each by 1-1.6 s).
- `?P` still reports CAV on the CLV side, as before.
- Hardware test list, each with a control (unmodified build, same disc):
  `?P` bit; `Q05R`/`Q06N` on BGB S2 with A6 and `?F`; `Q00R` on S2 -> `AN`;
  `?C` while playing across the 05/06 boundary; `C1` overlay on a still and
  in play, `D1` restores the picture number; a side without chapters.txt
  answers exactly as before.
