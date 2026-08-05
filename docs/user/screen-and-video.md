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
too. It occupies `&FCAC-&FCB0`:

| Address | Contents |
|---|---|
| `&FCAC` | X position, low byte |
| `&FCAD` | X position, high byte |
| `&FCAE` | Y position, low byte |
| `&FCAF` | bits 0-3: Y position high bits; bits 4-7: pointer shape 0-3 (4 or more = pointer off) |
| `&FCB0` | pointer type select |

## Video background

Pi1MHz emulates a LaserDisc player for the Domesday system: a 768x576
video plane behind the Beeb graphics, showing the Domesday LaserDisc
frames, driven by the same player commands ("F-codes") the VFS software
sends to a real Philips player.

What works today: if a file called `frame.lz` (an LZ4-compressed
768x576 YUV frame) is present in the SD card root, it is displayed as
the background image at power-on.

If there is no `frame.lz` the background is simply black, and none of
this affects anything else.

## Hardware video player

Beyond the single still frame, Pi1MHz contains a full-motion video
player that uses the Pi's **hardware H264 decoder**: video with sound,
plus LaserDisc-style random access - goto picture, freeze frame,
step forward/back, play, reverse - driven by the same F-codes the
Domesday VFS software sends. The ARM stays almost idle: the VideoCore
does all decoding and pixel moving, straight into the buffers the
display hardware is scanning out.

To use it you need three things:

1. **The full GPU firmware, and three config.txt settings.** Copy
   `start.elf` and `fixup.dat` from the matching [Raspberry Pi firmware
   release](https://github.com/raspberrypi/firmware/tree/master/boot)
   to the card (the shipped `start_cd.elf` is a cut-down firmware with
   no video codec support), then uncomment these lines in `config.txt`:

   ```
   start_file=start.elf
   fixup_file=fixup.dat
   gpu_mem=64
   vd_use_vpu0=1
   vd_isp_disable=1
   ```

   `vd_use_vpu0=1` is **required**: Pi1MHz runs its 1MHz bus handler on
   VPU core 1, and without this key the firmware wants that core for
   decoding as well, so no frame is ever produced. With it set, video
   and the Beeb interface run together happily.

   `vd_isp_disable=1` is recommended rather than required - it makes the
   decoder convert its output format on the VPU instead of the ISP
   block, which is about 5% faster. The picture is effectively identical
   (1.3% of pixels differ by 1-4 levels out of 255, i.e. rounding in the
   colour conversion).

2. **A `video.pvf` file** in the SD card root, made from any video
   with the offline tool:

   ```
   tools/make_pvf.py mydisc.mkv video.pvf
   tools/verify_pvf.py video.pvf
   ```

   The tool re-encodes to 768x576 all-intra H264 (every frame
   individually seekable - that is what makes freeze frame and picture
   numbers work) and packs the audio ready-resampled for the Pi.
   A Domesday disc side comes to roughly 2 GB.

3. Nothing else - at boot the player shows picture 1 and then follows
   the F-codes (`Fxxxxx R/N/S`, `N`, `O`, `L`, `M`, `*`, `/`, `A1`,
   `B1`, `?F`, ...).

Without the full firmware or without `video.pvf`, everything quietly
falls back to the `frame.lz` behaviour above. A missing `vd_use_vpu0=1`
is the one setting that does *not* fall back: the decoder starts
normally but never produces a picture, so the screen stays black. If
that happens, check that line in `config.txt` first - `/status` will
show the H264 decoder running with 0 frames decoded.

### Performance

Measured on a Pi Zero W with a 768x576 all-intra test file, decoding
free-run with no display pacing and with the 1MHz bus handler running
as usual: **152 frames per second**. The player needs 25, so there is
about 6x headroom. If [WiFi](wifi.md) is enabled, the web interface's
`/status` page reports the decoder's frame count.

Developers: the full design - VCHIQ/MMAL protocol details, buffer
handling, the `.pvf` container - is in
[docs/dev/h264-hardware-decode.md](../dev/h264-hardware-decode.md).
