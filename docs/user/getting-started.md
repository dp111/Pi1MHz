# Getting Started

## What you need

- A BBC Micro (B or B+) or BBC Master with a free 1MHz bus connector.
- A Pi1MHz interface board (the level shifter that sits between the Pi
  and the 1MHz bus).
- A Raspberry Pi Zero W, Zero 2 W or 3B+. A plain Pi Zero (no W) works
  for everything except WiFi. The Pi 3A+ is expected to work but is not
  formally tested.
- A micro SD card, formatted FAT32. A few gigabytes is plenty unless you
  plan to store lots of disc images.
- 5V power for the Pi. Most boards take this from the BBC itself (for
  example from the User Port or Tube connector) so no separate power
  supply is needed - check the instructions for your particular board.

If you want the WiFi features you need a WiFi-equipped Pi (Zero W,
Zero 2 W or 3B+) and a 2.4GHz WPA2 network.

## Prepare the SD card

1. Download the latest Pi1MHz release from
   <https://github.com/dp111/Pi1MHz> (or use the `firmware/` folder if
   you have a copy of the source tree).
2. Copy the **entire contents** of the release's firmware folder to the
   root of the SD card. When you are done the card root should contain,
   among other things:

   ```
   bootcode.bin
   config.txt
   start_cd.elf
   fixup_cd.dat
   kernel.img          (used by Pi Zero / Zero W)
   kernel7.img         (used by Pi Zero 2 W / Pi 3)
   Pi1MHz/             (ROMs, configuration, WiFi firmware)
   ```

   The right kernel for your Pi model is picked automatically - leave
   both on the card.

3. Optionally, add hard disc images. A ready-made starter set is
   available from the BeebSCSI project:
   <https://www.domesday86.com/wp-content/uploads/2019/03/BeebSCSI_Quickstart_LUN_2_5.zip>
   Unzip it so that the card gains a `/BeebSCSI0` directory containing
   files such as `scsi0.dat`. See [Hard discs](hard-discs.md) for the
   full layout.

4. Optionally, edit `/Pi1MHz/Pi1MHz.cfg` in a text editor to set up
   WiFi and other options. Every line in the shipped file is a
   commented-out example. See the
   [configuration reference](configuration.md).

## Connect the hardware

With everything switched off:

1. Fit the Pi to the Pi1MHz interface board.
2. Insert the SD card into the Pi.
3. Plug the interface board into the 1MHz bus connector under the BBC
   (the two-row IDC connector towards the left, between the Econet and
   analogue ports). Take care with orientation - follow your board's
   instructions.

## First boot

Switch the BBC on. The Pi boots from the SD card in a few seconds.

If the BBC starts up faster than the Pi (fast machine, slow SD card),
the machine may not see the hard disc first time - just press
**CTRL-BREAK** once the Pi has had a couple of seconds to catch up.

## Check it is working

Type this at the BASIC prompt:

```
X%=0 : CALL &FC88
```

You should get a help screen showing the Pi1MHz version, build date,
your Pi model and its temperature, followed by a list of the helper
functions. That screen coming up proves the Pi is alive and the bus
interface is working.

Two equivalent ways to reach the same screen:

```
*FX147,136,0
*GO FD00
```

or on a Tube:

```
*FX147,136,0
*GOIO FD00
```

(`*FX147,136,0` writes 0 to `&FC88`; `*GO FD00` runs the helper code
that Pi1MHz pages into the JIM window.)

Another quick check: `PRINT $&FD00` displays a short message that
Pi1MHz leaves in the first page of its expansion RAM.

## Use the hard disc

- **BBC Master**: ADFS is already in ROM. Put a disc image at
  `/BeebSCSI0/scsi0.dat` on the SD card, then `*ADFS` (or CTRL-A-BREAK)
  and `*CAT` as usual.
- **BBC B/B+**: you need ADFS. If you have sideways RAM, Pi1MHz can
  load it for you: `X%=3 : CALL &FC88` loads the ADFS ROM shipped on
  the SD card into sideways RAM. Then press CTRL-BREAK and `*ADFS`.

See [Hard discs](hard-discs.md) for images, drives and jukeboxes, and
[Loading ROMs](helpers-and-roms.md) for the other ROMs Pi1MHz can load
(MMFS, VFS support, Econet filing system, teletext software and your
own).

## Next steps

- Set up [WiFi](wifi.md) so you can manage the SD card from another
  computer through the [web interface](web-interface.md).
- Plug an HDMI monitor into the Pi and try the
  [screen redirector](screen-and-video.md).
- Try the [Music 5000 emulation](sound.md) with Hybrid Music System
  software.
