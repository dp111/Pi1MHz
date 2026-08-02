# HDMI Screen, Mouse Pointer and Video

Plug an HDMI monitor (or a cheap HDMI capture dongle) into the Pi and
Pi1MHz can mirror the Beeb's screen output onto it.

## The screen redirector

This is not a video digitiser - Pi1MHz renders the stream of VDU
characters and control codes the Beeb prints, using the BBC fonts, on
its own HDMI output (the renderer is adapted from PiTubeDirect).

Switch it on with helper 2:

```
X%=2 : CALL &FC88
```

From then on, everything printed through the OS (`PRINT`, `*CAT`,
listings, MODE changes and so on) also appears on the HDMI screen.

Under the hood the Beeb-side hook sends each VDU byte to `&FCA0`, so
software can also write to `&FCA0` directly to print on the HDMI
display without affecting the Beeb's own screen.

You can see the same picture from another computer: with
[WiFi](wifi.md) running, the [web interface](web-interface.md) shows a
live snapshot at `http://Pi1MHz/framebuffer`.

## Mouse pointer

Pi1MHz can overlay a mouse pointer on the HDMI picture - this is used
by VFS/Domesday software (`*MOUSE`), and your own programs can drive it
too. Four bytes at `&FCAC-&FCAF` control it:

| Address | Contents |
|---|---|
| `&FCAC` | X position, low byte |
| `&FCAD` | X position, high byte |
| `&FCAE` | Y position, low byte |
| `&FCAF` | bits 0-3: Y position high bits; bits 4-7: pointer shape 0-3 (4 or more = pointer off) |

## Video background (experimental)

Pi1MHz has the beginnings of LaserDisc player emulation for the
Domesday system: a 768x576 video plane behind the Beeb graphics,
intended to show the Domesday LaserDisc frames, driven by the same
player commands ("F-codes") the VFS software sends to a real Philips
player.

What works today: if a file called `frame.lz` (an LZ4-compressed
768x576 YUV frame) is present in the SD card root, it is displayed as
the background image at power-on. The moving-video and
still-frame-seeking side of the player emulation is **incomplete and
under development** - do not expect a full Domesday experience yet.

If there is no `frame.lz` the background is simply black, and none of
this affects anything else.
