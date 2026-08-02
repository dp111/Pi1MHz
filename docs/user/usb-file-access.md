# USB File Access (MTP)

Connect the Pi's USB data port to a PC with a USB cable and the Pi
shows up as a portable device called **Pi1MHz MTP** - the same way a
phone or camera does. You can then browse the SD card and copy files
both ways without removing the card and without WiFi.

## Using it

1. Connect a data-capable USB cable (not a charge-only one) from the
   computer to the Pi's USB **data** port (on a Pi Zero, the inner
   micro-USB socket marked USB, not the one marked PWR).
2. On Windows, the device appears in File Explorer under "This PC";
   on other systems use any MTP-capable file browser.
3. Drag files to and from the SD card as usual - disc images into
   `/BeebSCSI0`, ROMs into `/Pi1MHz`, and so on.

MTP is a "one side at a time" protocol: the Pi stays in control of the
card, so this is safe to use while the system is running.

Like the [web interface](web-interface.md), MTP refuses to overwrite,
delete or move a file the Beeb currently has open - a started hard-disc
image, or an [MMFS](mmfs.md) disc image / `BEEB.MMB` - reporting the
device as busy. Release it on the Beeb first (`*BYE` for a hard disc).

## A special file: kernel.now

Copying a firmware image to the device under the name `kernel.now`
makes the Pi immediately restart into that image - **temporarily**. It
runs from memory; the `kernel.img` on the SD card is untouched, and the
next power-cycle goes back to the version on the card. This exists for
safely test-driving a new firmware build before committing to it. To
make a new firmware permanent, copy it over `kernel.img` in the SD card
root instead (over MTP, WebDAV, or in a card reader).

If you never touch firmware updates you can ignore this entirely -
just avoid naming any of your own files `kernel.now`.
