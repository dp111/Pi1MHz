# F-code chapters

STATUS (2026-11-20): BUILT on branch `fcode-chapters`, host-tested (with a
control: the new F-code checks fail 20/48 on master's fcode.c), NOT
hardware-tested.  All six configs build warning-free.

## What

The VP415's chapter F-codes, from a per-side `chapters.txt` beside
`video.pvf` (format in `src/chapters.h` and docs/user/screen-and-video.md):

| F-code | Reply | Behaviour |
|---|---|---|
| `QxxR` | `A6` / `AN` | goto the chapter's first picture, halt (`videoplayer_goto(p,'R')`) |
| `QxxN` | `A6` / `AN` | the same, then play (`'N'`) |
| `?C` | `Cxx` / `X` | chapter of the picture on screen; `X` with no player, no table, or before the first chapter; `O` while the tray is open comes from `fcodeReadBuffer` as for every reply |
| `?P` | | 2nd status character bit 2 = "chapter numbers exist" when the side has a table |
| `C0`/`C1` | | the player's 2-digit chapter overlay; `C1` turns the picture number off, `D0`/`D1` turn the chapter number off (VP415 manual) |

`AN` also answers a malformed `Q`, three digits, and the play-sequence form
`QxxyyzzS` (not built: nothing known sends it).

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
