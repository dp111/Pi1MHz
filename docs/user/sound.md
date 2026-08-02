# Sound: Music 5000/3000 and BeebSID

## Music 5000 / Music 3000

Pi1MHz emulates the Hybrid Music System's Music 5000 synthesiser and
the Music 3000 expander (giving 16 channels each, 32 in total). To the
Beeb it looks like the real hardware, so the Hybrid AMPLE software and
other Music 5000 programs work as-is.

It is enabled by default - just run your Music 5000 software.

### Where the sound comes out

- By default the sound is played through the **BBC's own internal
  speaker** (the audio is fed back over the interface board).
- On a **Pi 3B+** you can instead use the Pi's headphone jack for far
  better quality. Setting `BeebAudio_Off=1` in `/Pi1MHz/Pi1MHz.cfg`
  mutes the feed to the Beeb speaker and switches the headphone jack to
  proper stereo.

Note: a hard disc access while music is playing can briefly interrupt
the sound.

### Volume / gain

```
M5000_Gain=3
```

The default gain is 3. Automatic scaling normally pulls the gain down
if the output clips; to fix the gain and switch auto-scaling off, add
1000 to the value - for example `M5000_Gain=1016` means gain 16 with
auto-scaling disabled.

### Recording to a WAV file

Pi1MHz can capture Music 5000 output straight to a file on the SD
card:

```
*FX147,202,4 : *FX147,203,1     start recording
*FX147,202,4 : *FX147,203,0     stop recording
```

On stopping, the capture is written to the SD card root as
`Musics000.wav` (then `Musics001.wav` and so on). The exact numbers to
type are shown on the Pi1MHz help screen (`X%=0 : CALL &FC88`), which
is worth checking in case a firmware update moves them. The recording
buffer uses the Pi's spare RAM; recording stops by itself if it fills
up.

## BeebSID

Pi1MHz can emulate a SID chip (the Commodore 64 sound chip) appearing
at `&FC20-&FC3F`, compatible with BeebSID software.

It is **off by default**. Enable it in `/Pi1MHz/Pi1MHz.cfg`:

```
BeebSID_addr=0x20
```

Enabling BeebSID automatically disables the Music 5000 emulation - the
two share the Pi's single audio output, so only one can run at a time.
`BeebAudio_Off=1` works for BeebSID too, muting the Beeb-speaker feed.
