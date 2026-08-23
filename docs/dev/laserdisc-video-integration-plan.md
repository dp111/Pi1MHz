# LaserDisc video integration — plan

Turning the working H264 decoder into a LaserDisc player the Domesday VFS ROM
can actually drive: the Beeb sends F-codes over SCSI, they control **which
frame** is on screen and **how it is overlaid**, and playback streams the
preprocessed video out of the *current* `/BeebVFS<n>` directory while the Beeb
is simultaneously reading data from the same SD card.

Companion to `docs/dev/h264-hardware-decode.md`, which documents the transport
(VCHIQ/MMAL), the `.pvf` container and the decode path. That layer is done and
hardware-proven; this plan is only about the parts above it.

---

## 0. Where we already are

Working, on hardware:

* `rpi/vchiq.c` + `rpi/vcsm.c` + `rpi/mmal_vc.c` + `rpi/h264dec.c` — zero-copy
  hardware decode, 152-165 fps free-run at 768x576, **with the 1MHz bus handler
  running**, given `start.elf` + `gpu_mem=64` + `vd_use_vpu0=1`.
* `videoplayer.c` — full player state machine: still / play / reverse-play /
  step / goto, stop and info registers, 25 fps pacing off the system timer,
  audio ring into the PWM DMA, HVS page flips, and a clean
  "no file, no codec firmware → plane stays off" path.
* `BeebSCSI/fcode.c` — `F…R/N/Q/S/I`, `N`, `O`, `L`, `M`, `*`, `/`, `+yy`,
  `-yy`, `A0/A1`, `B0/B1`, `?F`, `E0/E1`, `VP1/2/3` are wired to the player or
  the HVS planes.

So the gaps are: **where the file comes from**, **surviving the Beeb's own SD
traffic**, **the overlay surface being coherent**, and **the F-code reply
protocol being honest about completion**.

Five work packages, in dependency order. WP1 and WP3 are the two that must be
right; the rest is fidelity.

---

## 1. WP1 — the video source is the current BeebVFS directory

Today: `videoplayer.c:49` hardcodes `PVF_FILENAME "video.pvf"` and
`pvf_open_file()` (`videoplayer.c:518`) opens it from the card root, once, at
`videoplayer_init()`.

Required: the single preprocessed video living in `/BeebVFS<n>`, where `n` is
`filesystemState.lunDirectoryVFS` — which the Beeb can **change at runtime**.

### 1.1 Locate it

Add to `BeebSCSI/filesystem.c`, next to `filesystemGetLunDirectory()`
(`filesystem.c:820`):

```c
uint8_t filesystemGetLunDirectoryVFS(void);      /* the VFS jukebox number */
```

In `videoplayer.c`, replace `pvf_open_file()`'s fixed name with a scan:

1. `snprintf(dir, "/BeebVFS%d", filesystemGetLunDirectoryVFS())` — mirrors
   `fsHostLunNames()` (`filesystem.c:376`); do **not** duplicate the format
   string, export that helper or a VFS-only sibling of it.
2. `f_opendir` / `f_readdir` (`FF_USE_LFN=2`, so long names are available;
   `FF_USE_FIND` is 0, so no `f_findfirst`).
3. Accept the first entry that is a regular file, is not `scsi?.*` (the LUN
   image, descriptor and config live in the same directory), and whose first
   64 bytes validate as a `pvf_header_t` — magic `PVF1`, version 1, the same
   sanity bounds `pvf_open_file()` already applies. Extension is *not* the
   test: header magic is, so a `.pvf`, `.vid` or extensionless file all work.
   Prefer a `.pvf` extension only as a tie-break if several validate.
4. If nothing validates, fall back to `/video.pvf` in the root (keeps the
   existing standalone demo card working). If that is not there either,
   leave the video plane disabled - there is no still-frame path any more
   (`frame.lz` and its LZ4 decompressor were removed with the 4:2:2 plane).

Cost is one directory walk per open — a handful of `f_readdir` calls plus a
64-byte read each, only on init or a jukebox change. Nothing in the steady
state.

### 1.2 Reopen when the directory changes

Two runtime paths change the VFS directory, and **both currently leave the
player streaming from a stale `FIL`**:

* `hd_emulator_write_scsijuke()` (`harddisc_emulator.c:170`) — a write to
  `&FC41`. It calls `filesystemReset()`, which remounts the volume and so
  invalidates every open `FIL` (FatFs `fs->id` check → `FR_INVALID_OBJECT`),
  then `scsiJukebox()`.
* `scsiBeebScsiSelect()` (G6 0x11, `scsi.c:2187`) → `scsiJukebox()` →
  `filesystemSetLunDirectory()` (`filesystem.c:807`), when `scsiHostID >= 16`.

Add one entry point to the player and call it from both:

```c
void videoplayer_media_changed(void);   /* close, rescan, reopen, show pic 1 */
```

`scsiJukebox()` (`scsi.c:2173`) is the single choke point for the directory
change itself; `filesystemReset()` is the choke point for the remount. Hook
`scsiJukebox()` after `filesystemSetLunDirectory()` succeeds, and
`filesystemReset()` unconditionally — the player must treat a remount as
"my handle is dead" even when the directory number has not moved.

### 1.3 Lifecycle split

`videoplayer_init()` (`videoplayer.c:593`) currently does discovery, decoder
bring-up, buffer allocation, plane creation and poll registration in one shot,
and only registers the poll task on the video path. Split it:

* `videoplayer_init()` — allocate frame buffers, bring up `h264dec`, create the
  plane, **always** `Pi1MHz_Register_Poll(videoplayer_poll)` (registration is
  idempotent, `Pi1MHz.c:276`), then call `videoplayer_media_changed()`.
* `videoplayer_media_changed()` — close any open file, free the index, rescan,
  reopen, reload the fastseek link map, `h264dec_resume()` to flush the
  pipeline, reset player state, show picture 1. No decoder teardown, no GPU
  buffer churn: the expensive resources stay up across a jukebox change.
* Keep the existing warm-restart handling (Beeb Break re-runs every init;
  `h264dec_reset()` before releasing buffers) exactly as it is — it is subtle
  and correct.

Decide what "no video in this directory" means for the plane: recommend
**hold the last decoded picture and keep the plane enabled** if a video was
previously open, else leave the plane off. Blanking to black on every jukebox
change looks like a fault.

### 1.4 Don't let the file be pulled from under us

`filesystemHostPathBusy()` (`filesystem.c:394`) already stops the web UI / MTP
/ WebDAV layers from overwriting a mounted LUN image. Extend it to report the
currently open video file as busy too — otherwise a browser upload into
`/BeebVFS0` can truncate the file the decoder is streaming from.

---

## 2. WP2 — streaming while the Beeb hammers the same SD card

This is the real integration risk, and it is not about SD bandwidth.

### 2.1 The problem

Every SCSI byte transfer to the Beeb is a **blocking spin** in the main loop:
`hostadapterPerformReadDMA()` (`harddisc_emulator.c:346`) loops 256 times over
`hd_wait_ack()` (`harddisc_emulator.c:289`), which spins until the 6502 ACKs.
`scsi.c` then loops that over every sector of a multi-sector read. Nothing else
in the main loop runs during that window — including `videoplayer_poll()`.

Meanwhile the audio DMA runway is **two buffers of 448 words**
(`rpi/audio.h:22`) = 224 stereo frames each, i.e. **9.5 ms total** at 46875 Hz.

A Beeb-paced 256-byte sector is a few ms; an ADFS/VFS multi-sector read is tens
to hundreds of ms. So today, every time the Domesday ROM reads data — which it
does *constantly*, interleaved with playback — the audio underruns and the
frame pacing stalls. On a still frame nobody notices. On a playing video it
will click and stutter every single time.

### 2.2 The fix: pump from inside the wait

`hd_wait_ack()` is *idle* CPU: we are waiting on the 6502. Give it a hook:

```c
/* in hd_wait_ack()'s spin, on the same 256-iteration cadence as the
   timeout check, so the fast path stays fast */
if ((counter & 0xFFu) == 0u)
    Pi1MHz_poll_during_wait();
```

with a small, explicitly re-entrancy-safe worker in `videoplayer.c`:

* `audio_pump()` — always safe, touches only the ring and the DMA buffers.
* `h264dec_poll()` — VCHIQ message pumping and buffer recycling; no FatFs.
* `flip_pending()` when the frame is due — three HVS register writes.
* **No SD reads.** `feed_frame()` must not be called from the hook.

That last rule is what makes this safe. FatFs is built with `FF_FS_REENTRANT=0`
and one shared window buffer per volume, so re-entering it mid-operation would
corrupt state. `hd_wait_ack()` is only ever reached *between* FatFs calls (the
sector is already in the buffer by the time the DMA loop starts), but the hook
must not depend on that subtlety — keep it FatFs-free and the invariant is
local and checkable.

Guard against recursion with a static `in_pump` flag, and keep the whole hook
inside `if (videoplayer_active())` so still-frame cards pay nothing.

### 2.3 Ride through the gap that remains

Even pumped, no *new* frames are read during a long SCSI burst. Deepen the
pipeline so the burst is absorbed rather than seen:

* Raise `H264DEC_INPUT_BUFFERS` from 2 and the `in_flight < 2` limit in
  `videoplayer_poll()` (`videoplayer.c:366`) — each extra AU is ~30-60 KB of VC
  heap and buys 40 ms of runway. 4-6 deep is 160-240 ms for ~0.5 MB.
* Add a third frame buffer (`NUM_FRAME_BUFFERS`, `videoplayer.c:79`, and
  `H264DEC_MAX_OUTPUT` is already 3) so a decoded frame can queue behind the
  displayed one instead of the decoder idling. +648 KB of VC heap.
* Grow the audio ring's *lead* rather than its size: `AUDIO_RING_FRAMES 8` is
  320 ms of buffering, which is plenty — the problem was never ring depth, it
  was that nothing drained it into the DMA. WP2.2 fixes that.
* Consider two DMA buffers of 1024 words instead of 448 (`rpi/audio.h:22`) to
  take the runway to ~21 ms. Cheap insurance; check nothing else depends on
  `DMA_BUFFER_SIZE` granularity (`M5000_emulator.c`, `fastsid`, `BeebSID` all
  fill the same buffers).

### 2.4 Bandwidth and seek cost — measure, don't assume

At 25 fps and ~40 KB/frame the sustained requirement is ~1 MB/s, plus whatever
the Beeb is reading. The `.pvf` layout costs one `f_lseek` + two `f_read`s per
frame, with `FF_USE_FASTSEEK` and a 256-entry cluster link map
(`videoplayer.c:529`).

Two things to check on hardware before tuning anything:

1. **Link map coverage.** 256 entries covers 127 fragments. A 2 GB file copied
   onto a well-used card can exceed that, and the fallback is a FAT walk *per
   seek* — catastrophic at 25 fps. Log it loudly (the code already does,
   `videoplayer.c:533`) and consider sizing the table from the file's cluster
   count, or asking the user to defragment.
2. **Per-frame read latency under contention**, measured with the Beeb doing a
   continuous VFS read. Add counters to `/status`: frames decoded, frames late,
   audio underruns, worst-case `feed_frame()` duration. Everything else in this
   WP is a knob; these counters are how we know which knob to turn.

---

## 3. WP3 — the overlay surface

Three HVS planes: **0** = video (`videoplayer.c:47`), **1** = the Beeb screen
(`SCREEN_PLANE`, `framebuffer/screen_modes.c:26`), **2** = the mouse pointer
(`MOUSE_PLANE`, `mouseredirect.c:246`).

`fcode.c` pokes `screen_plane_enable()` and `screen_set_palette()` directly
(`fcode.c:313`, `fcode.c:496-517`). Three problems:

1. **Plane 2 is the mouse, not an overlay layer.** `VP2` and `VP3` enable it
   unconditionally, so a mouse pointer can appear because the Beeb selected an
   overlay mode. Drop plane 2 from the VP handling entirely.
2. **`E0` (video off) and `VP` fight over plane 0**, with no memory of each
   other: `E0` then `VP1` re-enables video the ROM asked to have off.
3. **The still-frame fallback shares plane 0**, so overlay control has to keep
   working when there is no video at all.

Fix by making the player own the plane and giving `fcode.c` an intent API:

```c
void videoplayer_video_enable(bool on);        /* E0 / E1              */
void videoplayer_overlay_mode(int mode);       /* VP1..VP5             */
int  videoplayer_overlay_mode_get(void);       /* VPX request          */
```

`videoplayer.c` keeps `{video_on, overlay_mode}` and derives the plane state
once, in one function, so the combinations are enumerable and testable:

| mode | meaning | plane 0 (video) | plane 1 (Beeb) |
|---|---|---|---|
| VP1 | LaserVision only | on | off |
| VP2 | computer RGB only | off | on, opaque (`set_palette(1,0,3)`) |
| VP3 | hard-keyed | on | on, colour 0 transparent (`set_palette(1,0,2)`) |
| VP4 | mixed | treat as VP3 | as VP3 |
| VP5 | enhanced | treat as VP3 | as VP3 |

`E0` forces plane 0 off regardless of mode and is remembered; `E1` restores the
mode's own answer. VP4/VP5 aliasing to VP3 is deliberate — they are analogue
mixing modes with no digital equivalent, and answering `VPX` with the mode
actually set is more useful than pretending.

Also in scope here, both currently no-ops that the Domesday ROM does send:

* `D0`/`D1` — picture-number display on/off, and `C0`/`C1` — chapter display.
  Both are an on-screen digit overlay drawn by the *player*, not the Beeb.
  Cheapest honest implementation: render into the Beeb-side plane? No — that is
  the ROM's memory. Draw into the Y plane of the decoded frame after decode,
  before the flip, with a small built-in 8x8 font. ~40 lines, and it only runs
  when enabled.
* Keep `VPmode` (`fcode.c:45`) as the answer to `VPX`, but source it from
  `videoplayer_overlay_mode_get()` so it cannot drift from reality.

---

## 4. WP4 — F-code completeness and honest replies

### 4.1 Replies are sent before the action happens

`fcodeWriteBuffer()` fills `scsiFcodeBufferRX` with the completion code at
*command* time (`fcode.c:348-390`): `A0` for a still, `A1` for goto-and-play,
`A2` for the stop register, `A3` for the info register. On a real VP415 those
are sent **when the operation completes** — the seek has landed, the stop
picture has been reached. The ROM reads them with G6 0x08
(`scsi.c:2048`), and `fcodeClearBuffer()` makes each reply one-shot.

Make the reply event-driven:

* `fcode.c` records "a completion is owed, and which code", and leaves the RX
  buffer holding a bare `CR` (= "no reply yet", which is exactly what the VP415
  manual specifies and what `fcodeClearBuffer()` already writes).
* `videoplayer.c` calls back when the seek lands / the stop picture is reached
  / play ends, and `fcode.c` posts the code then.
* Bound it: if the completion has not arrived within, say, 500 ms, post it
  anyway. A ROM that spins forever waiting for a reply is a worse failure than
  an early `A0`, and there is precedent in this codebase for exactly that
  hazard (see the ADFS handshake note in `filesystem.c`).

Do this **after** WP1-WP3 are stable on hardware, and be ready to revert it: it
is the one change here that can make a currently-working ROM hang, and the
current optimistic behaviour is known to work.

### 4.2 Transport F-codes still unimplemented

All present in `fcode.c` as debug-only cases, all sent by VFS:

| code | meaning | work |
|---|---|---|
| `S xx` | set fast/slow speed value | store the multiplier |
| `R` | slow/fast read | apply the multiplier to `frame_period_us` |
| `U` | slow motion forward | play with a stretched frame period |
| `V` (bare) | slow motion reverse | reverse-step with a stretched period |
| `W` | fast forward | play with a shortened period, skipping frames |
| `Z` | fast reverse | reverse, skipping frames |
| `X` | clear | clear stop/info registers |
| `?P` | player status | derive from the player state machine |
| `?D` | disc program status | static answer |

All of these are cheap in the current architecture: every frame is an IDR, so
"fast" is *stride the index* and "slow" is *stretch the pacing interval*. Add a
`speed_num/speed_den` (or a signed frame stride plus a period multiplier) to
`vp` and let the existing `VP_PLAY` / `VP_PLAY_REV` cases honour it.

Chapters (`Q`, `?C`, `C0/C1`) need a chapter→picture table the `.pvf` does not
carry. Either extend the container with an optional chapter table (a new
optional section, `version` stays 1 as long as readers key off
`reserved[]`/offsets being zero), or declare chapters unsupported and answer
`?C` with `X` (not available). **Recommend: unsupported for now** — the
Domesday discs are driven by picture number, and inventing a chapter table
offline is real work in `make_pvf.py` for no known caller.

Time code (`T`) is the same story: the container has no time codes, and
picture number ÷ 25 is a lie on a disc that was never 25 fps in the first
place.

---

## 5. WP5 — configuration, packaging and diagnostics

* **`config.txt`.** The four lines that hardware video needs are still
  commented out (`firmware/config.txt:32-43`): `start_file=start.elf`,
  `fixup_file=fixup.dat`, `gpu_mem=64`, `vd_use_vpu0=1` (plus
  `vd_isp_disable=1`, worth ~5%). The full `start.elf` + `fixup.dat` are **not
  in the repo** — only `start_cd.elf`/`fixup_cd.dat` are. Decide: ship the full
  firmware in `firmware/` (adds ~3 MB, and pins us to a firmware revision), or
  document the download and keep the lines commented. **Recommend shipping
  it** — a Domesday user should not have to fetch firmware by hand, and the
  fallback path is already graceful for everyone else.
  Note `gpu_mem=64` costs the ARM 48 MB against today's `gpu_mem=16`; check the
  heap headroom given the known-tight RAM situation (WiFi + USB together).
* **Audio ownership.** Music 5000 is enabled by default and claims the PWM
  path first, so the player silently stands down (`videoplayer.c:691`) and
  Domesday video has no sound. For a card whose VFS directory contains a video,
  sound is the point. **Recommend: the player wins when a video is open**, with
  `video_audio=0` in `Pi1MHz.cfg` to opt out. That needs `rpi_audio_init()` to
  be re-claimable, and the M5000 to tolerate losing it.
* **`/status` and `/vclog`.** Extend the existing `/status` video section with:
  the resolved video path, frame count, current picture, mode, frames decoded,
  frames late, audio underruns, worst-case read time. These are the only way to
  debug a stutter report from a user.
* **Docs.** `docs/user/screen-and-video.md:94` tells users to put `video.pvf`
  in the card root; update it for the BeebVFS directory rule. Add the
  `make_pvf.py` recipe for a Domesday side.

---

## 6. Sequencing

1. **WP1** (source discovery + jukebox reopen) — self-contained, testable with
   the existing test clip copied into `/BeebVFS0`.
2. **WP2.2** (the `hd_wait_ack` pump) — small, and it is what makes playback
   under Beeb load watchable at all. Measure before and after with the
   counters from WP2.4.
3. **WP2.3** (pipeline depth) — tuned from those measurements, not guessed.
4. **WP3** (overlay ownership) — visible, low risk, no protocol change.
5. **WP4.2** (speed F-codes) — additive.
6. **WP4.1** (deferred replies) — last, behind a config gate, easy to revert.
7. **WP5** — packaging, alongside whichever step needs it.

Hardware validation notes that apply to every step, from the H264 bring-up:

* **`kernel.now` chain-boot does not work with video** — the VideoCore keeps
  its previous VCHIQ state and ignores the new kernel's CONNECT. Every video
  test kernel must boot from SD and be reached with a full `POST /reboot`.
* Follow `PI-CONTROL.md`; flash only a settled Pi.
* The Domesday ROM plus a real VFS directory is the acceptance test. A
  `testsrc` clip proves decode, not integration.

---

## 7. Open decisions

1. **Ship the full `start.elf`** in `firmware/`, or document the download?
   (§5. Recommend: ship.)
2. **`gpu_mem=64` for every card**, or only for cards that want video? It is a
   `config.txt` edit either way, but the default matters for RAM headroom.
3. **Audio: player over Music 5000 by default?** (§5. Recommend: yes, when a
   video is open.)
4. **Chapters and time codes: implement or answer "not available"?** (§4.2.
   Recommend: not available.)
5. **Deferred F-code replies: worth the regression risk?** (§4.1. Recommend:
   yes, but gated and last.)
6. **`.pvf` selection rule** — header magic wins over extension, per §1.1. If
   the preprocessing tool is going to guarantee a name, say so and this gets
   simpler.
