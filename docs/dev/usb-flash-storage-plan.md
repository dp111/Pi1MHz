STATUS (2026-10-09): built on branch usb-msc; hardware-tested on a Zero 2 W
+ Master with a FAT32 Kingston DataTraveler (0951:1666) - see "Hardware
results" at the end.  exFAT is built but NOT tested on hardware (dp111: left
for now).  Stages 1-4 were done as one (dp111: ADFS LUNs and MMFS move too).
Written from a read-only design study of master 0992048.  Claims are marked
MEASURED, INFERRED or GUESSED.

- `storage=usb`: every path the Beeb's storage uses is built from
  `filesystemStorageRoot()` ("" or "1:"): BeebSCSI and BeebVFS
  (filesystem.c), the FAT transfer directory, the FAT service (names get
  "1:"; MMFS's raw sectors on drive 0 go to drive 1; getcwd asks the
  drive's directory with it made current for the call), FujiNet's store,
  Music 5000 recordings, the video.  WebDAV, MTP, Pi1MHz.cfg, ROMs, the
  WiFi profile and cacert.pem stay on the card.
- Decided at the first use after each BBC reset; until 5 s after the first
  reset it waits for the drive (`usb_storage_wait_for_drive`, running USB
  through `usb_service`, the boot half included).  GUESSED: 5 s covers a
  hub plus a slow drive - the USB drive row shows the wait taken.
- Host-side interlocks (`beeb_path_busy`, the LUN lock) compare card
  paths: with the storage on the drive nothing of the Beeb's is on the card.
- Writes: `WRITE(10)` through the bounce buffer, same timeout.  No SCSI
  SYNCHRONIZE CACHE (CTRL_SYNC is a no-op, as for the SD card).
- The eject and remount hooks were NOT split by volume: an SD card eject
  (kernel.now, a card swap) also closes the drive's files - which is what
  kernel.now needs, and USB host keeps running through it.
- `watchdog_feed()`: the 1.5 s command wait and the power-on wait re-arm a
  configured watchdog.
- With two FatFs volumes `f_getcwd` prefixes "0:"; the FAT service strips it
  (the stage 2 build gave the Beeb "0:/dir").
- Not done: read-ahead for the drive; a WebDAV `/usb` folder; a split of
  the eject hooks.

## Hardware results (2026-10-09, Zero 2 W + Master, FAT32 drive)

All MEASURED, each against the SD card as the control:
- ADFS `*CAT`, and `*SAVE` onto the drive's image (the card's image md5
  unchanged; the file absent on the card's disc).
- MMFS (helper 4) `*DCAT` and `*SAVE` into BEEB.MMB on the drive (the card's
  BEEB.MMB md5 unchanged).
- VFS `*MOUNT 0` and Domesday video from `1:/BeebVFS0/video.pvf`: ~26
  pictures/s over 20 s, no drops, no audio underruns added - as the card.
- Power-on (Beeb and Pi cycled together): decided on the drive after a
  254 ms wait; ADFS from the drive with no BREAK.
- Drive pulled out: Disc error 1C, Beeb and Pi alive; CTRL-BREAK - the card.
  Plugged back in: the card until the next CTRL-BREAK, then the drive.
- Writes stall: one 16 KB write took 1665 ms (22 of 42521 over 100 ms;
  reads 3 ms at most) - the command limit is 10 s, not 1.5 s.
- Found and fixed: the helpers' ROMs ("Pi1MHz/<n>.rom" via the FAT
  service) went to the drive - /Pi1MHz now stays on the card.
- Not tested: exFAT, two drives, a drive behind a hub with a mouse.
