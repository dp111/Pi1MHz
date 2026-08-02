# Troubleshooting

## Nothing works at all

- **Check the SD card.** The card root must contain `bootcode.bin`,
  `start_cd.elf`, `fixup_cd.dat`, `config.txt`, `kernel.img`,
  `kernel7.img` and the `Pi1MHz` folder. A card with only some of
  these will not boot. The card must be FAT formatted.
- **Check the Pi's power/activity LED.** If it never flickers after
  power-on, the Pi is not booting - re-copy the firmware files, try
  another card.
- **Reseat everything**: the Pi on the interface board, the board on
  the 1MHz bus connector. Check the connector orientation against your
  board's instructions.
- The 1MHz bus connector on the BBC sees little use for decades at a
  time - dirty contacts are a classic cause of flaky behaviour.

## The help screen doesn't appear

`X%=0 : CALL &FC88` should print the Pi1MHz help screen.

- If the machine just hangs or beeps, the Pi is probably not being
  seen on the bus at all - see above.
- If you get `Bad command` or similar, check you typed it exactly.
- The helpers need `/Pi1MHz/6502code.bin` on the SD card. If that file
  is missing the helper interface is silently absent, though other
  features still work.

## The hard disc isn't found

- **Press CTRL-BREAK again.** A fast machine with a slow SD card can
  finish booting before the Pi is ready. One extra CTRL-BREAK after a
  couple of seconds cures it.
- Check the image really is at `/BeebSCSI0/scsi0.dat` (directory
  `BeebSCSI0`, file `scsi0.dat` - a common mistake is an extra folder
  level from unzipping, e.g. `/BeebSCSI_Quickstart/BeebSCSI0/...`).
- On a BBC B, make sure ADFS is actually present (`*HELP` lists ROMs).
  Load it with `X%=3 : CALL &FC88` then CTRL-BREAK, and start it with
  `*ADFS`.
- If you changed `SCSIJUKE`, the drive images are being looked for in
  `/BeebSCSI<that number>` instead of `/BeebSCSI0`.
- `*FX147,65,0` switches back to disc set 0 if something has jukeboxed
  away from it - but jukeboxing only works while every drive is
  stopped, so `*BYE` first if a disc is in use.

## Discs behave strangely / errors after heavy use

- Make a habit of backing up your `.dat` images - they are ordinary
  files and easy to copy off the card.
- Try the image in another emulator or a fresh copy of it, to
  establish whether the image or the setup is at fault.

## WiFi

WiFi trouble is almost always one of these:

1. **No `wifi_ssid` set.** WiFi does not even start without it.
2. **Wrong band or security.** The network must be 2.4GHz with WPA2
   personal. A 5GHz-only network, WPA3-only setting, or a network with
   a browser login page will not work.
3. **Password wrong.** 8-63 characters, exactly as on the router.
4. **Country code.** Set `wifi_country=` to where you are. The default
   is `GB`; with the wrong country set, the radio may not be allowed to
   use the channel your router is on (channels 12/13 are the usual
   culprits).
5. **Not waited long enough.** Joining plus getting an address can
   take up to ~30 seconds after power-on.
6. **Firmware files missing** - only if you built the card by hand:
   the `brcmfmac*` files must be in `/Pi1MHz/wifi/`. The standard
   firmware set includes them.

Then, to locate the Pi: try `http://pi1mhz.local/`, `http://Pi1MHz/`,
and your router's device list. If name lookup is unreliable, give the
Pi a [fixed address](wifi.md#fixed-address).

If the Pi answers sometimes and not others, suspect WiFi signal
quality before anything else - a Pi Zero's antenna inside/behind a
metal-lined case can be marginal. Moving the machine or the access
point a metre often transforms it.

## Web pages load but uploads fail

- The server refuses to overwrite, delete or move any file the Beeb
  currently has open - a started hard-disc image, or a file (or
  `BEEB.MMB`) that [MMFS](mmfs.md) has open through the FAT service.
  WebDAV clients see "423 Locked"; MTP reports the device busy.
- For a **hard disc**, `*BYE` in ADFS stops the drive and releases it.
  (CTRL-BREAK does *not* - the Beeb re-accesses the disc on reset and
  restarts the drive.) For **MMFS**, see
  [changing images from another computer](mmfs.md#changing-images-from-another-computer).
- Very large uploads over marginal WiFi can simply take a long time -
  try `/bench.bin` to gauge your actual speed.

## Econet (AUN) doesn't talk

- Check `http://pi1mhz.local/aun` - it shows your station number, the
  address map and traffic counters. Counters that never move mean
  nothing is arriving: check the other end's address and your
  `aun_map`.
- Every station needs a unique station number; the fileserver must
  know your station's IP address (on most bridges/fileservers that
  means a static entry for it - and on PiEconetBridge, one configured
  with AUTOACK).
- Use fixed IP addresses at both ends.

## Music 5000 sound is distorted or too quiet

- Distortion: lower `M5000_Gain`, or leave auto-scaling on (values
  under 1000).
- Too quiet: raise `M5000_Gain`.
- On a Pi 3B+, `BeebAudio_Off=1` plus headphones/amplifier on the Pi's
  jack gives much better quality than the Beeb speaker, in stereo.
- Brief interruptions during disc access are a known limitation.

## The Pi locks up occasionally

Add `watchdog=10` to `/Pi1MHz/Pi1MHz.cfg`. If the
firmware ever stops responding for 10 seconds, the Pi reboots itself
rather than sitting dead until you power-cycle. (The Beeb side will
see the hard disc pause during the reboot.)

## Getting more information out of it

The release firmware prints nothing at all - that is normal. For real
diagnosis you need a debug build of the firmware (built from source
with `DEBUG=1`, or included with some releases), placed on the card as
`debug/kernel.img` (and/or `debug/kernel7.img`):

1. Edit `config.txt` in the SD root and un-comment the
   `kernel=debug/kernel.img` line (for a Pi Zero; the equivalent line
   in the `[pi3]` section for a Pi 3/Zero 2 W).
2. Connect a 3.3V serial lead to the Pi's GPIO 14/15 (TX/RX) pins and
   ground, at 115200 baud, 8N1.
3. Boot; the debug build reports everything it does, including WiFi
   progress and hard disc commands.

Options like `wifi_debug=1`, `aun_debug=1` and `teletext_debug=1` in
`Pi1MHz.cfg` add yet more detail - they only do anything on a debug
build.

Remember to switch `config.txt` back afterwards; debug builds are
slower.

## Asking for help

The Pi1MHz thread on the StarDot forums (<https://stardot.org.uk>) is
the place. Include your machine, Pi model, firmware version (top line
of the help screen, `X%=0 : CALL &FC88`) and what you have tried.
