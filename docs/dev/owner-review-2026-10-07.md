# Owner review list, 2026-10-07

STATUS: decided 2026-10-07 - see "Outcome" below; the tables keep the
original proposals.  Built from
`review-2026-10-06.md` (full detail by ID there) after the merge and the
hardware tests of 2026-10-07.  Mark each item **fix**, **reject** or
**later**.  "Rec" is Claude's recommendation; the review's own evidence
label is in brackets (INFERRED = read from the code, not seen happening).

## 1. Findings that look like real bugs

| ID | Where | What | Rec | Decision |
|---|---|---|---|---|
| S3 | `scsi.c:2586-2592` | BSFATREAD goes BUS FREE mid data-in on a read failure, so the Beeb waits on REQ for ever.  Return CHECK CONDITION as READ6 does. [INFERRED] | fix | |
| L6 | `rpi/sdcard.c:556-566` | The CMD12 response after a multi-block write is thrown away, so SD write errors reach FatFs as success. [INFERRED] | fix | |
| S5 | `scsi.c:2063-2081, 558-564` | INQUIRY on a never-started LUN, and unknown opcodes, return CHECK CONDITION without sense data, so REQUEST SENSE says NO ERROR. [INFERRED] | fix | |
| S6 | `filesystem.c:882-883` | ADFS images are always opened read/write; a read-only image can't start its LUN and the Beeb sees BAD FORMAT. [INFERRED] | fix | |
| F2 | `fat_service.c:750-755` | At BREAK, files open in the FAT service are forgotten unsynced: a file being written keeps its old size and loses clusters. [INFERRED] | fix | |
| U4 | `mtp_fs.c:1925-1927` | `reboot.now` resets from inside `tud_task`, unanswered - the pattern chainboot.c dropped.  Defer it the same way. [INFERRED] | fix | |
| W5 | `webserver.c:6725-6728` | A PUT/POST answered before its body is read keeps the connection alive, so the body is parsed as the next request. [INFERRED] | fix | |
| W6 | `webserver.c:4268` | The upload form deletes an empty folder of the same name (DAV PUT already refuses). [INFERRED] | fix | |
| C7 | `teletext_emulator.c:226-260` | After `tcp_close` fails in the recv callback it aborts but returns `ERR_OK`, so lwIP touches the freed pcb. [INFERRED] | fix | |
| F3 | `fujinet/fn_json.c:215` | cJSON nests 1000 deep with no guard against the 64 KB stack; build with `-DCJSON_NESTING_LIMIT=64`. [INFERRED] | fix | |
| A2 | `1mhzwifi.asm:111-117` | No word-boundary check: the WiFi ROM claims `*TIMER`, `*MODEM`, `*LAPSE`, `*PINGALL`... from lower-priority ROMs. [INFERRED] | fix | |
| A4 | `join.asm:57-62` | A 128-character password leaves no CR; ESCAPE is stored as a password character. [INFERRED] | fix | |
| A5 | `6502code.asm:947-960` | `serremove` never restores the 6850 setting, so RS423 receive stays dead after `*GO FD03`. [INFERRED] | fix | |
| D11 | `sdio.c:5132-5176` | QUERY_AMPDU sends 4 diagnostic GETs (~480 ms) on every WiFi bring-up in release.  Breaks the release-build rule; gate on `wifi_diag`. [INFERRED] | fix | |
| D12 | `sdio_host.c:668, 716, 735, 770` | A LOG_INFO register dump in release on every command error, including the expected KSO wake failures. [GUESSED frequency] | fix | |

## 2. Findings expected to be rejected (your earlier decisions)

| ID | What | Why reject | Decision |
|---|---|---|---|
| C8 | `hd_emulator_set_IRQ()` reads `HD_status` from the main loop with FIQ unmasked | Same family as HD_status volatile; the Beeb serialises SCSI | |
| S8 | WRITE F-code has no `bytesTransferred < 256` check | WRITE6-bounds reasoning: the Beeb sends the whole block | |
| P8 | `hd_wait_ack` reports ACK after SEL/reset (junk MODE SELECT after a BREAK) | Needs the Beeb to abandon a command part-way | |
| C5 | No VFP save in FIQ; IRQ saves d0-d7 only on kernel7 | You chose the d0-d7 fallback in August; FIQ uses no FP | |
| L5 | `_clean_cache_area` ends in DMB, not DSB | Cache primitives stay as they are | |
| F1 | No `/BeebVFS*` guard in fat_service (open/rename/unlink/mkdir) | The Beeb's VFS never writes; recorded as your call since 2026-09-07 | |
| S9 | `filesystemGetFatFileInfo` can `f_mkdir` under `/BeebVFS*` | As F1 | |
| F4 | `/BEEBVF~1/` (8.3 alias) gets past FujiNet's VFS guard | As F1 | |
| C3 | `JIM_Init.bin` reloaded / page 0 cleared on every BREAK | Probably intended | |
| A7 | ROM loaders skip a bank already holding a ROM | Probably intended | |

## 3. Lower risk - fix only if cheap

| ID | What |
|---|---|
| C2 | No `JIM_ram_size == 0` guard in services_emulator: with `Rampage_addr=-1`, a write to &FCA6-&FCAA dereferences NULL in FIQ |
| C6 | Teletext `ring_push` drops non-field-aligned amounts, so after a stall every field is misaligned |
| U5 | `h264dec_reset()` ignores a failed `port_disable` and frees GPU buffers the component may still own |
| U6 | vchiq corrupt-RX path clears `inited` without condemning; ports are reused |
| S2 | Every jukebox (incl. the Domesday disc flip) runs a full `filesystemReset()` |
| S10 | The VFS title cache survives a card swap |
| S11 | Geometry for a stopped LUN uses `f_size()` of a closed FIL |
| L4 | One mailbox property buffer: a late answer to a timed-out query is taken as the next query's |
| L7 | `RPI_MailboxInit` runs after two property calls; `get_cmdline()` caches an empty result if its first call fails |
| D4-D8 | SDIO: CMD52 fallback ungated; R5 error flags unchecked; `max_seq` unclamped; 512-block CMD53 encodes as 0; retry-ladder off-by-one |
| D9, D10 | SDIO host-open retries switch power domain 0 off [GUESSED]; the "lazy re-open" path can never run |
| N9-N11 | TNFS close while writing skips CLOSE; telnet WILL/DO share one bitmap [GUESSED]; COPY_PUBLIC scratch page is handle 1's [GUESSED] |
| F5-F9 | FujiNet O(n^2) listing; slot cache stale after a card swap; zip-in-gzip UEF streams as noise; UEF pair split; 0x20 length eaten as a space |
| A8, A9 | AUN learn mode overwrites a static map entry; type-5 immediate checks busy before the replay cache |
| V9 | Teletext HOLD re-render; `VDU 23,17,5` swaps colours but not gcol/tint; `VDU 23,19,0` drops spacing at scale 0 |

Tidy-ups with no behaviour change (comments, dead code, CI/test quality):
L8, L9, V10, D13, N12, F10, A10-A12, T3-T5, T7, T8.

## 4. Other decisions

| Item | Question | Rec |
|---|---|---|
| D-dev4 | CLAUDE.md says Beeb writes to VFS are "never refused with an error" while F1 is open - reword? | settle F1 first |
| D-dev15 | Dead prototypes in mailbox.h, framebuffer.h, teletext.h, rpi/block.h (BeebSCSI's stay) - remove? | remove the non-BeebSCSI ones |
| Licence | `beeb/1mhz-wifi/build-merged.sh` and `build.sh` still say the merged WiFi ROM "may not be redistributed"; Peter Clarke gave permission 2026-09-21 and the ROM ships | update the comments |
| ChangeLog V1.39 | 60ab935 added "*POINTER 0 not yet tried on a Beeb" - it now is tested and works | fix the line |
| usb_mode=auto | Picks host with a plain lead to a computer (old kernel too); cause unknown | low priority now host is the default |
| Explorer | After Escape at a prompt, "Put file:" stays on screen until the next redraw | cosmetic, clear the line |

## 5. Hardware tests only you can do

- **WiFi on a Pi 3B+**: bring-up, `*JOIN` during bring-up, rejoin (7b2fbd1, c2c5d1b, 4a6292c, 9d5250a).
- **LaserDisc VP modes on Domesday**: VP4 / VP5, then BREAK - the picture should come back at full brightness and the dim strips should follow a MODE change (ace9d69, 3584cb8).

## 6. Before v.1.40

- ChangeLog: USB is a host by default (MTP needs `usb_mode=device`); the
  first install after V1.39 must go on the SD card (kernel.now from V1.39
  silently keeps the old kernel); kernel.now works with the video player
  open.
- Fresh firmware images, then push: ADFS-multi-target `878e424` first,
  then Pi1MHz master, then the `V1.39` tag.

## Outcome (2026-10-07)

**Fixed** (branch review-fixes, each with a host test that fails on the old
code where one was possible): S2, S3, S5, S6, S8, S10, S11, P8, F2 (and the
Beeb's own f mount/unmount, found by the review of the fixes), L6, L7, W5,
W6, C6, C7, F3, F5-F9, A2, A4, A8, A9, U5, U6, D11, D12, D-dev15 (#if 0),
the licence comments, the V1.39 ChangeLog line, the usb_mode=auto docs and
the explorer's leftover prompt.

**Hardware-tested** (Zero 2 W + Master): cold boot and kernel.now of the
combined build; S2 with a control (a download across a VFS side flip is
cut short on V1.39, complete now); explorer Escape clears the prompt; the
menu's new sound option (both / left / right / off), the right mouse
button and the help screen on a second visit (dp111).

**V9** (2026-10-08): checked against the Master - MODE 7 held graphics,
the re-render after an overwrite and VDU 23,17,5 all match it (no change);
VDU 23,19,0 with a 0 scale fixed (80d0808, Pi-only command, host test).

**Accepted** (2026-10-08): D11 - without wifi_diag the "802.11n state" /
"AMPDU limits" /status rows are gone; that is fine (dp111).

**Rejected or closed**: C2 (JIM RAM is never 0), C3, C5 (no FP in the
IRQ), C8, F1, F4, S9 (only the Beeb's VFS can't write), A7 (a bank may
hold a different ROM), D-dev4 (the VFS ROM never writes), L5 (DMB is
enough: every caller follows the clean with a device write), A5 (the 6850
reads &52 before install and after remove - nothing to restore, MEASURED),
D9 (not real), D10, N9-N11 (real but harmless), R17, P7.

**Still open**:
- A2/A4 not yet run on a Beeb: the WiFi ROM would not load into sideways
  RAM after a power cycle (banks 4-7 read RAM FF after *SRROM).
- sdcard: no CMD13 after a write's programming; OUT_OF_RANGE on a write
  ending at the card's last block (GUESSED, rare).
- FujiNet: atomic file rewrite needs a new store primitive.
- BSFATPATH has no short-transfer check.
- U4 (reboot.now inside tud_task), L4, D4-D8, the tidy-ups.
- WiFi on a 3B+ and the VP modes on Domesday (owner's hardware).
