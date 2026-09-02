# Audio tidy-up + HDMI output — plan

> **STATUS 2026-08-26: substantially implemented.** `src/rpi/audio.c` now has
> the single-owner core this plan called for: one producer at a time writes
> int16 stereo PCM into a ring (`audio_producer_t` with per-producer rate /
> latency / dma_frames), `audio_claim`/`audio_release` ownership, and both
> sinks (PWM pin and HDMI via MAI - `Audio_out=hdmi` in Pi1MHz.cfg) with
> M5000/BeebSID at 48 kHz over HDMI hardware-verified. Recent additions:
> same-rate producer handovers restart only the DMA (no CM clock KILL, which
> blanked FRED/JIM reads), and an ownerless core is auto-claimed by the
> first writer so synths return after the video player releases. Sections
> below are the original design record; consult the code for current truth.


Make the audio path a small, single-owner core with one canonical sample
format, pluggable producers (Music 5000, BeebSID, video player) that each
choose their own sample rate, and selectable sinks: the Beeb's audio pin
(PWM, as today) and/or HDMI. Keep the ARM cost at or below today's.

Companion to `docs/dev/laserdisc-video-integration-plan.md` (WP2/WP5 there
assume this exists) and `docs/dev/h264-hardware-decode.md`.

---

## 0. What is there today

`src/rpi/audio.c` is a PWM DMA driver with a producer API bolted on:

* Two DMA control blocks, each with a `DMA_BUFFER_SIZE = 448` word buffer
  (`rpi/audio.h:22`) = 224 stereo frames, **9.5 ms of runway in total**.
  Chained in a ring on DMA channel 5 into the PWM FIFO.
* `rpi_audio_init(rate)` sets the PWM clock (PLLD 500 MHz / 2 / `range`),
  so the rate is whatever the *first* caller asks for. Returns `range`,
  the full-scale PWM count — ~5333 at 46875 Hz, i.e. ~12.4 bits.
* Producers poll `rpi_audio_buffer_free_space()`, get a raw pointer to the
  next DMA buffer, and write **PWM words** into it. Each producer does its
  own conversion: M5000 has its own fixed-point scale/clip/dither in
  `music5000_store_sample()` (`M5000_emulator.c:292`); BeebSID and the
  video player use `rpi_audio_pack()` (`audio.c`, error-feedback dither).
* Ownership is first-come: `rpi_audio_active()` is "someone called init".
  `Pi1MHz.c:444` disables M5000 at config time if BeebSID is on; the video
  player stands down at init if anyone else claimed the path
  (`videoplayer.c`, "audio disabled - PWM in use").
* The Beeb's own audio and the Pi's PWM share **GPIO13 = PWM1**
  (`AUDIO_PIN`, `Pi1MHz.h:106`). `BeebAudio_Off=1` sets the pin hi-Z, which
  M5000 also reads as "go stereo" (`M5000_emulator.c:209`) — although PWM0
  is on GPIO18 = the bus's A2, so the "left" word never reaches anything.
  Every producer writes two words per frame; one of them is discarded.
* 46875 Hz everywhere. It is the Music 5000's *hardware* rate (6 MHz/128)
  and the only rate that makes sense for M5000. BeebSID adopted it for
  compatibility (`BEEBSID_SAMPLE_RATE`, `BeebSid.c:11`); `make_pvf.py`
  resamples video audio to it (`AUDIO_RATE = 46875`).
* There is **no HDMI audio code anywhere**. `screen.c` touches the HDMI
  block only to read the hotplug bit.

What is wrong with it, in order of how much it matters:

1. Producers write sink-specific words. Adding a second sink means either
   every producer learns two formats or the core converts — and the core
   can't, because it never sees PCM.
2. The rate is global and implicit. Whoever inits first wins, and nothing
   re-inits if ownership changes.
3. Ownership is a set of special cases in three files (`Pi1MHz.c`,
   `videoplayer.c`, the M5000/BeebSID polls) instead of one rule.
4. 9.5 ms of runway against a main loop that blocks for tens of ms during
   SCSI transfers (the video plan's WP2 problem). M5000 survives because
   its poll is cheap and frequent *when nothing else is happening*.
5. `M5000_store_sample` is the third copy of "scale to PWM range with
   dither".

---

## 1. Target shape

```
producers                core (rpi/audio.c)              sinks
---------                ------------------              -----
M5000   46875 Hz ─┐      int16 stereo ring        ┌─► PWM  (Beeb pin, GPIO13)
BeebSID 48000 Hz ─┼─►    one owner at a time    ──┤
video   48000 Hz ─┘      rate = owner's rate      └─► HDMI (MAI or AUDS, §3)
```

### 1.1 One canonical format: `int16_t` interleaved stereo

The core owns a ring of `int16_t` L/R pairs in ARM memory. Producers write
PCM; sinks pull PCM and convert on the way out. This is the one structural
change everything else hangs off.

Cost of the extra pass for PWM: ~10 instructions per sample pair for
`rpi_audio_pack()` on two channels. At 48 kHz that is ~1 M instructions/s,
**0.1% of one ARM1176 core** — cheaper than the M5000 synth's per-sample
`update_channels()` by two orders of magnitude. For an HDMI sink fed by
DMA the canonical format *is* the wire format (§3), so zero conversion.

### 1.2 API

```c
/* producer side */
typedef struct {
    uint32_t rate;                 /* Hz, the producer's native rate */
    uint32_t latency_frames;       /* how far ahead it wants to run */
    const char *name;              /* for /status */
} audio_producer_t;

bool     audio_claim(const audio_producer_t *p);   /* become THE producer */
void     audio_release(void);
bool     audio_owner_is(const audio_producer_t *p);
uint32_t audio_free_frames(void);                  /* ring space          */
int16_t *audio_write_ptr(uint32_t *contig_frames); /* up to wrap          */
void     audio_commit(uint32_t frames);

/* core side */
void     audio_pump(void);         /* ring -> enabled sinks; poll task AND
                                      the SCSI-wait hook (video plan WP2) */

/* sinks, config-time */
void     audio_route(bool pwm, bool hdmi);
void     audio_mute_beeb(bool);    /* today's rpi_audio_mute_beeb */
```

`audio_claim()` by a new producer supersedes the old one: the old one's
`audio_write_ptr()` returns `NULL` and its poll does nothing. That single
rule replaces `rpi_audio_active()`, the `Pi1MHz.c` BeebSID/M5000 loop, and
the video player's stand-down — see §4 for who wins.

`audio_write_ptr`/`commit` is the zero-copy producer path: M5000 and
BeebSID render straight into the ring exactly as they render into the DMA
buffer now, just as `int16_t` instead of PWM words. The video player's
`audio_ring_write()` (byte copy loop, `videoplayer.c`) goes away — the
PCM from the `.pvf` record is read from SD into the staging buffer and
`memcpy`'d into the ring in one go, or read directly into it if the
contiguous run allows.

### 1.3 Sinks

Each sink is `{ start(rate), stop(), free_frames(), write(const int16_t*,
n) }`. **Exactly one sink is active** (`Audio_out` is `beeb` or `hdmi`,
decided 2026-08-23), so `audio_pump()` drains the ring into that one
sink with no cross-sink pacing.

* **PWM sink** = today's `audio.c` minus the producer API, plus the pack
  pass. Keep the two-CB DMA ring, but size it in *time* not words:
  `PWM_DMA_MS` ≈ 10 ms per buffer → 20 ms runway (2× today, §5).
* **HDMI sink** — §3.

**The Beeb pin is always the sum of L and R** (decided 2026-08-23). The
PWM block has two outputs: PWM1 goes to the Beeb pin (GPIO13); on boards
with a headphone jack (Pi 3B+) the firmware also routes PWM0/PWM1 to the
jack as a stereo pair. M5000 already handles this correctly today
(`M5000_emulator.c:209`, `:300`): with the Beeb pin live it writes L+R
into both words; with `BeebAudio_Off=1` the pin is hi-Z and it writes L
and R separately for the jack. That logic moves into the PWM sink so
every producer gets it - which fixes a real bug: the video player writes
L/R separately regardless, so on a live Beeb pin it currently delivers
the **right channel only**, and `A1 B0` (Domesday soundtrack 1 alone)
gives silence instead of track 1.

Sink rule: `beeb_muted ? (L, R) : (L+R, L+R)`, the sum saturated to
int16 and unhalved, exactly as M5000 does it now - so M5000's mono level
is unchanged. Each producer's clip detection runs per channel before the
sum, so M5000's autorange sees the same signal it sees now. HDMI is
always stereo.

---

## 2. Sample rates

Rule: **the owner's native rate, never resample on the ARM.** The PWM sink
runs at any rate (it just sets `range`); the HDMI sink wants a standard
one.

| producer | native rate | why | HDMI? |
|---|---|---|---|
| Music 5000 | 46875 | hardware: 6 MHz / 128, the synth steps its 24-bit phase accumulators once per sample | non-standard — see below |
| BeebSID | free (fastsid resamples from 1 MHz internally, `fastsid_init(speed)`) | pick **48000** | yes |
| video | whatever `make_pvf.py` wrote | change the tool to **48000** | yes |

46875 Hz over HDMI: the HDMI audio clock regeneration (N/CTS) can express
any rate, and most sinks lock to whatever arrives, but the audio infoframe
has no code for it and some receivers refuse. Two honest options for M5000
→ HDMI:

* **Try it** with infoframe rate = "refer to stream header" and measure on
  the TVs/capture dongle we have. Zero code.
* **Run the synth at 48000**: `46875/48000 = 125/128 exactly`, so scaling
  each channel's `FREQ(c)` by 125/128 (`M5000_emulator.c:223`) reproduces
  the pitch with ≤1 LSB of 24-bit error — well under a cent. ~3 lines,
  but it changes the M5000's output sample-for-sample, so behind a config
  key.

Recommend: try it first; fall back to the 125/128 trick only if a real
display refuses 46875. PWM is unaffected either way.

`DMA_BUFFER_SIZE` stops being a public constant; `BeebSid.c` sizes a
local array off it (`samples[DMA_BUFFER_SIZE/2]`) and should use the
ring's contiguous-run count instead.

---

## 3. HDMI: two routes

Both routes end at the same hardware — the VC4 HDMI block's **MAI**
(Multi-channel Audio Interface), a FIFO the encoder packs into HDMI audio
sample packets. The question is who programs it.

### Route A — the firmware's `AUDS` VCHIQ service

This is what Raspberry Pi OS's `bcm2835-audio` ALSA driver uses: open the
`AUDS` service, send `CONFIG {channels, rate, bits}`, `CONTROL {dest,
volume}` (dest 0 = auto, 1 = jack, 2 = HDMI), `START`, then `WRITE`
messages each followed by a bulk transfer of the PCM; the firmware answers
with `COMPLETE {bytes consumed}` so the client knows its FIFO level.

* **Pro:** the firmware does the MAI, N/CTS, infoframes, hotplug
  re-init, and the dest switch. We already have a multi-service VCHIQ
  client (`vchiq_open_service`, bulk transmit) — bump `VCHIQ_MAX_SERVICES`
  from 2 to 3 and the transport is done. The wire protocol is a handful of
  small structs; ~250 lines.
* **Pro:** the PCM crosses by bulk DMA that the VC performs; ARM cost is a
  ~64 B message + a cache clean per block, same as the PWM path's clean.
* **Con:** needs the **full `start.elf`** — `start_cd.elf` has no VCHIQ.
  Video needs it anyway, so for the Domesday card it is free; for a
  BeebSID-only card it means `gpu_mem=64` and the firmware swap.
* **Con:** the firmware's own FIFO adds latency we do not control. The
  ALSA driver gets down to a few ms of period, so it should be fine for
  M5000's 10 ms update cadence, but it has to be measured.
* **Licence:** the protocol is learnt from a GPL-2.0 driver but the
  reimplementation is ours, same position as VCHIQ/MMAL already are.

### Route B — drive the MAI directly

Program the HDMI/HD register blocks (`0x7e902000` / `0x7e808000`:
`MAI_CTL`, `MAI_FMT`, `MAI_THR`, `MAI_SMP` for N/CTS, the audio infoframe
via the packet RAM) and DMA into `MAI_DATA` with DREQ 17, exactly like the
PWM sink does into the PWM FIFO.

* **Pro:** works with `start_cd.elf`; no VCHIQ; lowest possible latency;
  ARM cost identical to PWM (one DMA ring, one cache clean per block);
  canonical `int16` is (probably) the DMA format — **to verify**: the MAI
  takes 32-bit words, and whether S16 packs two samples per word or wants
  left-justified 32-bit must be checked against the hardware.
* **Con:** the firmware owns the HDMI block and re-initialises it on
  hotplug/mode change; our MAI setup gets clobbered and we have to notice
  (poll the hotplug bit we already read) and redo it. The packet RAM is
  shared with the firmware's AVI infoframe — write the wrong slot and the
  picture goes.
* **Con:** no documentation beyond Linux's `vc4_hdmi.c`/`vc4_regs.h`
  (GPL-2.0-only: facts and register offsets are fine to learn, code is
  not copyable into a GPLv3 project).
* **Con:** Pi 3 / Zero 2 share the block (same code); Pi 4 does not (not
  a target today).

### Result (2026-08-23): Route B works; Route A was misjudged

**Route B (direct MAI) is implemented and verified**: `rpi/hdmi_audio.c`
after Circle's `CHDMISoundBaseDevice` (GPLv3) - MAI registers at HD
`0x808000` / HDMI core `0x902000`, MAI sample clock as a rational of the
HSM clock (reads back 163682864 Hz, the value Linux uses), N/CTS from
the pixel clock read out of PLLH (74.25 MHz = 720p here), audio
infoframe through the packet RAM, IEC958 consumer subframes with the
B preamble every 192 frames, DMA channel 4 on DREQ 17 into
`HD_MAI_DATA` through the same two-buffer ring as the PWM sink. Needs
`hdmi_drive=2` so the firmware brings the link up as HDMI (the capture
dongle's EDID does not claim audio). A 1 kHz / 2 kHz M5000 test pair
came off the HDMI capture dongle **bit-exact in level** (±840 in the
ring, ±840 captured), left and right separate.

**The "AUDS kills the bus" verdict was wrong.** Bisected properly: with
`Audio_out=hdmi` the core never called `pwm_start()`, and with the PWM
block never started every FRED/JIM read from the Beeb returns &7F and
ADFS hangs - on the AUDS build, on the direct-MAI build, and on a build
that wrote NO HDMI registers at all. Stage-1 of `pwm_start()` alone
(clock manager PWM clock enable, PLLD source) is enough to restore the
bus. The bus handler's shared window evidently sits behind that clock;
`rpi_audio_init()` had been enabling it as a side effect since the
Music 5000 was made default-on. The core now always starts the PWM
block whatever sink is selected. The AUDS client (`rpi/auds.c`, not
built) worked protocol-wise and might be usable after all; it stays as
a record, Route B stays the implementation.

**Non-standard rates over HDMI are NOT acceptable**: the dongle treats
the 46875 Hz stream as 48000 and plays it 2.4% sharp (2000 Hz -> 2048
Hz measured). So HDMI means 48000 for everything: BeebSID and the
video player already, M5000 via the exact 125/128 phase-increment
scaling (§2) - the "try 46875 first" option is closed.

### Original recommendation (superseded)

**Route A first.** It is the lower-risk way to find out whether HDMI audio
is worth having at all, the transport is already in the tree, and the
Domesday card has the firmware it needs regardless. If the measured
latency or the `start.elf` requirement turns out to be a real problem for
the BeebSID-only case, Route B is a drop-in replacement behind the same
sink interface — nothing above the sink changes.

---

## 4. Who owns the audio

One rule, in one place (`audio_claim()`), replacing the three special
cases:

1. **Video player** (when a `.pvf` is open — i.e. we are in VFS mode).
   M5000 and VFS never run together, so the player simply claims on open
   and releases on close; M5000's poll finds `audio_owner_is()` false and
   does nothing — **including skipping `update_channels()`**, which is
   where its CPU goes. Today M5000 keeps synthesising 46875 × 16 channels
   a second whether or not anything is listening.
2. **BeebSID** when enabled by config (`BeebSID_addr`). Today's config-time
   "disable M5000" stays, because it also frees M5000's JIM/FRED
   registrations; the audio claim is just no longer what enforces it.
3. **Music 5000** otherwise (enabled by default, as now).

Claims re-init the sinks at the new rate. The PWM sink restarts its clock
and DMA (what `rpi_audio_init` does today, made idempotent); the HDMI
sink sends a new `CONFIG`. A Beeb reset re-runs every emulator init and
so re-claims in init order — the same result as today, just derived.

---

## 5. Processing time

The budget, per second, on the Pi Zero (1 GHz ARM1176):

| item | today | after | notes |
|---|---|---|---|
| M5000 synth (16 ch × 46875) | ~15-20 M instr | same — or **0** when video owns audio | the only big number, untouched |
| producer → ring | in-place | in-place | M5000/BeebSID render into the ring as they render into DMA now |
| ring → PWM (sum L+R or L/R, pack) | done by producers | ~1 M instr | moved, not added; M5000 gains one multiply per sample for its mix→int16 stage; one pack per frame when the pin is live (both words equal), two when the jack is in stereo |
| ring → HDMI | — | A: ~1 message + 1 cache clean per 10 ms; B: 1 cache clean | negligible either way |
| cache maintenance | 1 clean per 448-word block | 1 clean per block per DMA sink | same order |

Net: flat for the existing producers, a few percent *lower* on a Domesday
card because M5000 stops synthesising when the video owns the output.

The thing that actually improves behaviour is not CPU: it is
`audio_pump()` being callable from the SCSI-wait spin (video plan WP2) and
the PWM runway going to ~20 ms, so Beeb disc traffic stops starving the
DMA. That is the same change for M5000 — it gets the deeper runway and
the in-wait pumping for free.

---

## 6. Configuration

Pi1MHz.cfg keys — all optional, defaults reproduce today:

| key | values | default |
|---|---|---|
| `Audio_out` | `beeb`, `hdmi` | `beeb` |
| `BeebAudio_Off` | 0/1 | unchanged (pin hi-Z) |
| `BeebSID_rate` | Hz | `48000` when `hdmi` is in `Audio_out`, else `46875` |
| `M5000_hdmi_rate` | `native` / `48000` | `native` (the 125/128 option, §2) |

`M5000_addr=-1` is no longer needed to get video sound — the claim rule
handles it — but keeps working.

`/status` gains an audio line: owner, rate, sinks, ring level, underruns
per sink. Underrun counters are how we find out whether Route A's latency
is acceptable; build them in from the start.

---

## 6a. Measured 2026-08-23 (step 1 built, on hardware)

`/status` now shows `Audio: <owner>, <rate>, <queued>, <blocks>,
<underruns>, <pin mode>`. `blocks` counts DMA buffer handovers (proves
the sink is alive - it caught the pump not being polled at all);
`underruns` counts a buffer the DMA entered without it having been
refilled, plus whole missed cycles (SRC_ADR went backwards in the same
buffer).

M5000 synthesising, 100 MB HTTP download as the main-loop stressor:

| `AUDIO_DMA_FRAMES` | delay | idle 20 s | during download |
|---|---|---|---|
| 224 (4.8 ms) | 4.8-9.5 ms | 0 | 7 / 150 s |
| 112 (2.4 ms) | 2.4-4.8 ms | 0 | 528 / 80 s |
| 64 (1.4 ms) | 1.4-2.7 ms | 0 | 1137 / 80 s |

The main loop stalls for 2.5-5 ms routinely while the webserver serves
a file (SD reads, SDIO), and >5 ms occasionally. **224 stays the block
size** until those paths pump the audio from inside their waits (the
WP2 idea, applied to the webserver file loop as well as the SCSI ACK
spin). Register-write "smear" is gone regardless: M5000 renders per
idle-loop pass into the ring, not per DMA block. The ring lead is ONE
block (`latency_frames = AUDIO_DMA_FRAMES`) - two stacked on the DMA's
two doubled the delay in the first cut.

**SCSI transfers, measured with OSGBPB re-reading a 16 K ADFS file 300
times from BASIC:** 301 underruns - one click per 16 K read - with no
servicing inside the transfer; **4 -> 5** with the owner's synth AND the
pump called between sectors in `scsi.c`. Two placements that HANG ADFS,
both tried: inside `hd_wait_ack()` and every 32 bytes inside the byte
loop. The 6502 transfer loop assumes a controller that always keeps up
and never waits for REQ to drop, so any pause between the bytes of a
sector desyncs it. Between sectors is safe by existing evidence (the SD
card is read there today). Worst service call 360 us.

**Decision (2026-08-23): M5000/BeebSID glitches during disc or WiFi
access are acceptable.** So the synth-inside-the-transfer plumbing was
removed again; what stays is `audio_pump()` between sectors
(`hd_audio_service()`), which is what the video player needs - its ring
holds hundreds of ms, and Domesday reads the disc constantly while
playing. An IRQ-driven DMA refill (many small CBs, ~25 us per IRQ) was
considered and would make the sink immune to every main-loop stall, but
is not needed for that decision; revisit only if video audio proves to
glitch, and gate any IRQ length against the 300 x 16 K ADFS loop, since
a pause of hundreds of us between bytes hangs the Beeb.

Still to measure: BeebSID, the video player with `.pvf` audio at 48 kHz.

## 7. Steps

**Status 2026-08-23: steps 1-4 DONE** (core refactor + ownership on
hardware; HDMI via the direct MAI sink, not AUDS; measured under load and
soak-tested 36 min with the video player). Step 5 partially: `make_pvf.py`
defaults to 48000 and the synths run at 48 kHz on HDMI (M5000 by the exact
125/128 phase-increment scaling - 1 kHz verified as 1000 Hz off the
capture); `docs/user/sound.md` still needs its update. Two /code-review
passes reshaped the sink: FULL-block DMA commits only (a stalled loop then
replays a stutter, never a buzz), one shared `pack_frame()`, mono-producer
flag for BeebSID, autorange watching the L+R sum only when the pin sums.


1. **Core refactor, PWM only.** `int16` ring, `audio_claim()`, PWM sink
   with pack pass, time-sized DMA buffers, `audio_pump()` split from the
   poll. Convert M5000, BeebSID, video player. No behaviour change is the
   acceptance test — BeebSID and the video player must produce identical
   PWM words. M5000 is **decided (2026-08-23): unified**, i.e. it emits
   int16 with its own error-feedback stage and the sink packs it. Two
   dither stages in series instead of one means ±1 PWM LSB differences on
   a fraction of samples (~-74 dB); level, clip point and autorange are
   preserved by scaling mix→int16 with a constant `K = 2^21 / range@46875`
   rather than a shift, which also makes `M5000_Gain` rate-independent.
   Verify by diffing PWM words old vs new on a recorded register trace.
2. **Ownership.** Delete `rpi_audio_active()` and the stand-down in
   `videoplayer.c`; M5000 poll skips synthesis when not owner.
3. **HDMI sink, Route A.** `rpi/auds.c`, `VCHIQ_MAX_SERVICES` 3,
   `Audio_out` key. Test BeebSID at 48000 and video at 48000 (after the
   `make_pvf.py` change), then M5000 at 46875 on every display we have.
4. **Measure** latency and underruns on the Domesday card with the Beeb
   hammering the disc. Decide on Route B from numbers, not taste.
5. `make_pvf.py` default `AUDIO_RATE` → 48000; `pvf.h` comment and
   `docs/user/sound.md` updated.

Hardware notes: HDMI audio needs the display actually negotiated
(`hdmi_force_hotplug=1` is already in the recommended `config.txt` for the
capture dongle); the USB capture path that verified video can verify
audio too (VLC `:dshow-adev=` instead of `none`).

---

## 8. Decisions

Taken 2026-08-23:

1. **Route A** (firmware `AUDS`) first; Route B (direct MAI) only if
   measurements demand it.
2. Needing the full `start.elf` for HDMI audio is **acceptable**, including
   on a BeebSID-only card.
3. `Audio_out` is **`beeb` or `hdmi`**, never both. One active sink.
   The Beeb pin always carries **L+R summed** (as M5000 already does);
   the Pi 3B+ jack gets stereo when `BeebAudio_Off=1`; HDMI is stereo.
4. M5000 **unified** into the common int16 → sink pack pass (see step 1 for
   the exact scheme and the expected LSB-level dither difference).

Still open:

5. ~~M5000 → HDMI at 46875~~ — closed: the capture dongle plays 46875 as
   48000 (2.4% sharp). M5000 on HDMI needs the 125/128 synth-rate scaling.
