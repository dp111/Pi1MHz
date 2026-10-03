# USB Mouse

Plug an ordinary USB mouse into the Pi and the Beeb can use it: the
VFS ROM's pointer (`*MOUSE`, Domesday and other VFS software) follows
it, and `ADVAL` reads its position and buttons. It works alongside an
AMX-style mouse or trackerball on the user port - both can be used at
the same time.

## Setting it up

1. **Make the Pi's USB port a host.** Add this to `/Pi1MHz/Pi1MHz.cfg`:

   ```
   usb_mode=host
   ```

   | `usb_mode` | The port is |
   |---|---|
   | `device` (default) | [USB file access (MTP)](usb-file-access.md) to a computer |
   | `host` | a host for a USB mouse |
   | `auto` | a host when an OTG adapter is plugged in, MTP otherwise |

   On a Pi whose USB sockets sit behind its own hub - the Pi 1, 2 and 3
   Model B, B+ and 3B+ - the port can only be a host, so it is always
   one, whatever `usb_mode` says.

2. **Plug the mouse in.** On a Pi Zero use the inner micro-USB socket
   (marked USB, not PWR) with an OTG adapter. A USB hub works too; the
   first mouse found is the one used, and keyboards and other devices
   are ignored. Any standard mouse works - the Pi asks it for the simple
   "boot" report every mouse supports.

3. **Use the Pi1MHz VFS ROM** - the `VFS.rom` shipped in `/Pi1MHz`
   (see [The VFS ROM](helpers-and-roms.md#the-vfs-rom)). An original
   Acorn VFS ROM knows nothing of the USB mouse and ignores it.

The status page (see [The web interface](web-interface.md)) has a
**USB** row: `device (MTP)`, or `host, mouse` with the mouse's USB
vendor:product ID, or `host, mouse none` when no mouse is plugged in.

In host mode there is no MTP. To copy files, or to try a firmware
image with `kernel.now`, use [the web interface](web-interface.md)
over WiFi.

## On the Beeb

After `*MOUSE` the USB mouse moves the same pointer as the user port
mouse:

- `ADVAL(7)` and `ADVAL(8)` give the X and Y position, in graphics
  units (0-1276 and 0-1020).
- `ADVAL(9)` gives the buttons: bit 0 the left button, bit 1 the
  middle, bit 2 the right.
- Pressing a button types the same key as the matching user port
  mouse button (left = button 1, middle = button 2, right = button 3).

One count from the mouse moves the pointer one graphics unit.

## For programmers: the registers

Reading `&FCAC-&FCAF` gives the mouse's movement since the previous
read (writing them still drives the [mouse pointer](screen-and-video.md#mouse-pointer)):

| Address | Read |
|---|---|
| `&FCAC` | X movement, bits 0-7 |
| `&FCAD` | bits 0-5: X movement, bits 8-13; bit 6: left button; bit 7: right button |
| `&FCAE` | Y movement, bits 0-7 |
| `&FCAF` | bits 0-5: Y movement, bits 8-13; bit 6: middle button; bit 7: a mouse is plugged in |

The movement is a 14-bit two's complement number (-8192 to +8191);
Y up is positive, as on the Beeb's screen. A set button bit means the
button is held.

Read the four in order, `&FCAC` first. Reading `&FCAF` hands over the
next movement, so the bytes read always belong together, and nothing
is lost between reads. Movement beyond what one read can carry is
dropped - a mouse left unread does not pile up a jump.

Only the VFS ROM should read them while `*MOUSE` is on: a second
reader would take movement from it.
