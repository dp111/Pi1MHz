# Pi1MHz — rules for Claude

Bare-metal Raspberry Pi firmware emulating BBC Micro 1MHz-bus peripherals
(SCSI hard disc / VP415 LaserDisc for Domesday, WiFi, framebuffer, sound).
No OS, no threads: one cooperative main poll loop + FIQ for bus events +
the VPU servicing the 1MHz bus. Read `PI-CONTROL.md` before touching
hardware; `docs/dev/*.md` are the design records (each carries a STATUS
header — trust the newest status over the body).

## Architecture — hard boundaries

- **Layers own their pixels and their state.** The F-code layer emulates a
  LaserDisc player: it may control only the player's own output (video
  plane, audio, transport). It must NEVER touch the Beeb's display — no
  CLS, no framebuffer writes. If an F-code handler seems to need a
  Beeb-screen side effect, the diagnosis is wrong.
- **`vidcore/Pi1MHzvc.s` (and generated `src/Pi1MHzvc.c`) are hands-off.**
  Report findings with line numbers; the owner decides.
- **VFS volumes (`/BeebVFS*`, LUN ≥ 8) are read-only media.** The firmware
  never creates, formats, or writes them. Beeb writes are silently
  accepted-and-discarded — never refused with an error, because the Beeb's
  FS handshake loops have no timeouts and an error can hang the machine.
- **Never do SD/FatFs work in FIQ context.** FIQ latches a request; the
  main loop services it (see `hd_juke_pending`).
- **State machines own their dispatch.** Work belonging to one state goes
  inside that state's `case`, not behind an `if` in the caller. Match the
  idiom of the code you are editing.
- **Low RAM below 0x8000 is VPU/Beeb-shared** (`Pi1MHz_STRUCT_VADDR`
  0x100 + shadow/JIM). ARM-side persistent state goes in `.noinit`
  (survives watchdog resets and the loader), never at fixed low addresses.
- The video plane is enabled only when the player has a real decoded frame
  (`videoplayer_active()`); data-only disc sides show black, never a stale
  buffer. The player is lazy: boot and BBC reset do no video/GPU/SD work.

## Release-build discipline

- **Release builds carry no diagnostic runtime cost.** Instrumentation is
  `#ifdef DEBUG` or config-gated with a zero-cost hot path when off.
  SCSI is reliable — do not re-add always-on SCSI instrumentation.
- Zero-cost forensics that stay in release: boot-stage breadcrumbs
  (boot-time), the crash record (fault path), the Reset-reason row.
- `LOG_DEBUG`/`wifi_debug_printf` are DEBUG-only; release serial prints
  banners/heartbeats via LOG_INFO. Don't add prints to fix release bugs.

## Build & artifacts

- Build: `bash src/scripts/build.sh rpi` (release) / `rpi debug` / `rpi3`.
  Both configs must compile warning-free (the vendored FatFs
  `-Wcast-qual` is the one pre-existing exception).
- Every build overwrites `firmware/kernel.img` — debug builds included.
  The committed artifact can be stale: never trust it; judge by source.
  Verify what you deploy: readback md5 (PUT) and, for behavioural changes,
  a marker string in the binary.
- `kernel7` (rpi3/A53) must be boot-tested on real hardware after any
  CP15/asm/timer change — a clean compile proves nothing there.
- BBC BASIC tokenising: use `basictool` (ROM-exact, builds in-tree from
  github.com/ZornsLemma/basictool). `tools/bbc_tokenise.py` exists but is
  not the tool of record.
- Vendored code (FatFs, lwIP, TinyUSB, VICE): disable with a named `#if`,
  never delete, so upstream diffs stay clean.

## Hardware sessions (see PI-CONTROL.md for the full runbook)

- Flash/reboot the target Pi without asking (standing authorization), but
  only a settled Pi; leave test state as-is afterwards and report it.
- Never BREAK the Beeb unless the Pi has just answered `/status` — a BREAK
  against a dead/rebooting Pi wedges the Master.
- Never persist an untested kernel to SD as the only copy; stage a
  known-good image for restore first.
- `pi-http.sh` pins the host and bakes in `--max-time 30` — pass your own
  for big transfers, use `-H 'Expect:'` on large PUTs, and verify sizes by
  HEAD (a 201/204 is not proof the bytes arrived).
- The Beeb screen is visible: HDMI capture via `ffmpeg.exe -f dshow -i
  video="USB Video"` (grab ~5 frames, read the png). Use eyes before
  blind probing. The Pico keyboard (COM9) drops characters — verify every
  typed command by its echo or its bus effect; BREAK resets CAPS
  (inverts case — F-code parameters are case-sensitive) and the redirect.
- Session hygiene (each of these cost a session hours at least once):
  `claude-tmp/pi-status.sh 'Boot|Display'` gives /status as text - never
  dump the HTML. `claude-tmp/beeb-cap.sh` grabs the HDMI capture, which is
  the **Pi's** output (help screen / video plane), not the Beeb's native
  screen. A `Boot time` row with a real pre-kernel figure after a
  kernel.now means the chain-boot fell back to the SD kernel - fingerprint
  before believing any symptom. Zero bytes on COM9 after F11 (1B 5B 57)
  means the Pico is wedged and needs a physical re-plug, not a firmware
  fault. `claude-tmp/build-all.sh` builds the three configs release-last
  and prints only warnings. Brief subagents narrowly with an output cap;
  a read-only Explore agent for a report, never an open-ended "study".

## Reviews and process

- Reviews must include an architecture pass: is each change in the right
  layer and the right function, matching surrounding idiom — not just
  "is it correct".
- For non-trivial changes, propose the diff and placement before applying.
- Experiments that need commits go on a worktree branch, never the main
  tree; the owner reviews and commits mainline changes manually and
  pushes manually — never push.
- Do not re-report findings the owner has rejected: HD_status volatile,
  WRITE6 LBA bounds, FatFs FIQ/main concurrency (the Beeb serializes SD
  ops), `_invalidate_cache_area` edge handling.
