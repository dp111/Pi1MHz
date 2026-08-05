# Hardware H264 decode on the Pi Zero (bare metal)

How Pi1MHz uses the VideoCore IV's hardware H264 decoder from bare metal:
what the hardware actually is, why the only viable route is speaking the
firmware's VCHIQ/MMAL protocols, exactly how those protocols work, and how
the code in this repo implements them.

## TL;DR

* The H264 codec block on BCM2835 has **no public register documentation**
  and is driven by code inside `start.elf`. Bare metal cannot poke it
  directly; you must ask the firmware to do it, over **VCHIQ** (a
  shared-memory mailbox channel) speaking **MMAL** (the media component
  RPC). MMAL "the library" is Linux userspace, but the *wire protocol*
  underneath is simple and fully reimplemented here in ~1500 lines.
* Requires the **full `start.elf`** (the shipped `start_cd.elf` cut-down
  firmware has no codec/VCHIQ support), **`gpu_mem=64`** and
  **`vd_use_vpu0=1`**. See the commented block in `firmware/config.txt`
  and §1 below.
* Source material is prepared **offline** by `tools/make_pvf.py` into a
  `.pvf` file: all-intra Annex-B H264 (every frame = SPS+PPS+IDR) plus
  46875 Hz PCM audio plus a frame index. All-intra makes random access,
  freeze frame, step and reverse trivial: any frame decodes on its own.
* Frames are delivered **zero-copy**: the decoder writes each picture
  straight into the buffer the HVS is scanning out, and only a memory
  handle crosses VCHIQ. No pixel ever travels over the channel, in
  either direction.
* Steady-state ARM cost per frame is tiny: one SD read of ~20-60 KB into
  an uncached staging buffer, ~300 bytes of message writes, and three HVS
  register pokes to flip the displayed frame. The VideoCore does all
  pixel work. No ARM cache maintenance anywhere (all shared buffers live
  in the VC heap, which the ARM maps uncached).
* Measured 152 fps on a Pi Zero W decoding a 768x576 all-intra clip with
  the 1MHz bus handler running as usual, against the 25 fps the player
  needs.

## File map

| file | role |
|---|---|
| `src/rpi/vchiq.c/.h` | bare-metal VCHIQ client: slot memory, doorbell, messages |
| `src/rpi/mmal_vc.c/.h` | MMAL wire structs + client: components, ports, formats, buffer exchange |
| `src/rpi/vcsm.c/.h` | SMEM service client: registers our buffers so MMAL will accept them |
| `src/rpi/h264dec.c/.h` | `ril.video_decode` wrapper: format-changed handshake, buffer plumbing |
| `src/pvf.h` | container format shared with the offline tool |
| `src/videoplayer.c` | the player: index, goto/still/play/step/reverse, audio, HVS flips |
| `src/BeebSCSI/fcode.c` | LaserVision F-codes now call into `videoplayer_*` |
| `src/rpi/screen.c` | new: 4:2:0 3-plane HVS support + `screen_set_YUV_pointers()` page flip |
| `tools/make_pvf.py` | offline encoder/muxer |
| `firmware/config.txt` | commented lines to switch to full start.elf + gpu_mem=64 + vd_use_vpu0=1 |

## 1. Why it has to be VCHIQ + MMAL

On VideoCore IV the H264 engine ("VCE" + hardware CABAC/deblock etc.) is
controlled by firmware running on the VPU. Broadcom never released its
register interface. The three theoretical routes:

1. **Drive the codec registers directly** - not possible, undocumented.
2. **Run our own code on the VPU that drives it** - Pi1MHz *does* run its
   own VPU code (`vidcore/Pi1MHzvc.s` on VPU core 1 for the 1MHz bus),
   but the codec driver is hundreds of KB of unreleased firmware logic;
   reimplementing it is not realistic.
3. **Ask start.elf to do it** - this is what Linux does. The kernel talks
   VCHIQ; userland's MMAL client library encodes component operations
   over it. Both protocols are open (kernel driver is GPL-2.0 OR
   BSD-3-Clause; userland is BSD-3-Clause) and stable for a decade
   (VCHIQ protocol version 8, MMAL worker protocol major 16).

So option 3, minus Linux: this repo carries a polled, single-service
VCHIQ client and a minimal MMAL client. Same approach as the Circle
bare-metal environment uses for its accelerated graphics, but far
smaller because we need exactly one component.

### Firmware prerequisites (IMPORTANT)

* `start_cd.elf` (currently shipped) has **no VCHIQ, no MMAL, no codecs**.
  The full `start.elf` + `fixup.dat` from the same firmware release as
  the shipped `bootcode.bin` must be copied to the card, and
  `start_file=start.elf`, `fixup_file=fixup.dat`, `gpu_mem=64` and
  `vd_use_vpu0=1` set (prepared, commented, in `firmware/config.txt`).
  Everything degrades gracefully without it: `vchiq_init()`'s mailbox
  call fails, and `videoplayer.c` falls back to the old `frame.lz`
  still.
* H264 needs **no licence key** (unlike MPEG-2/VC-1).
* `gpu_mem=64`: the decoder needs VC-side memory for its reference frame,
  bitstream FIFOs and its own copies of the buffer pools (~10-15 MB for
  768x576), plus our shared allocations (~2.8 MB, listed in §5).

### VPU core 1 and `vd_use_vpu0=1`

Pi1MHz launches its 1MHz bus handler onto VPU core 1 with mailbox tag
`0x30013` (`TAG_LAUNCH_VPU1` in `Pi1MHz.c`), where it spins with
interrupts off and never yields. With `start_cd.elf` the firmware never
wants that core; the full `start.elf` does, and by default it schedules
its H264 decode work there. The two cannot share it: with the bus
handler loaded the component accepts access units and reports no error,
but never issues the decode.

`vd_use_vpu0=1` in `config.txt` tells the firmware to decode on VPU core
0 instead. That is all it takes - **it is required for hardware video**,
and with it set the decoder and the 1MHz bus run together at full speed
(the 152 fps in the TL;DR was measured with the bus handler running).

## 2. VCHIQ: the transport (src/rpi/vchiq.c)

### Shared memory ("slots")

One block of memory, allocated from the VC heap and told to the firmware
via mailbox property tag `0x00048010` (bus address of the block; the tag
returns 0 on success - a nonzero/failed reply means "no VCHIQ in this
firmware", our clean fallback signal).

Layout (`NUM_SLOTS = 33` x 4 KB in our build):

```
slot 0            vchiq_slot_zero: magic 'VCHI', version 8/min 3,
                  sizes, fragment pool location, then TWO
                  vchiq_shared_state blocks: master (=VideoCore),
                  slave (=ARM), then per-slot use counters
slot 1            master sync slot        (synchronous msgs - unused)
slots 2..16       master data slots       (VC -> ARM messages)
slot 17           slave sync slot         (unused)
slots 18..32      slave data slots        (ARM -> VC messages)
+ 4 KB            fragment pool (64 x 64B, for the VC's cache-line
                  workaround on bulk transfers - unused, since this
                  client makes none, but the VC expects it to exist)
```

Each `vchiq_shared_state` carries: the slot range it owns, `tx_pos` (a
monotonically increasing byte offset into its message stream), a
`slot_queue[64]` ring of slot indexes, `slot_queue_recycle` (write index
into the ring), and `remote_event` triples {armed, fired, pad} for
trigger/recycle/sync events.

### Message flow

* A message is `{s32 msgid; u32 size; payload}` padded to 8 bytes.
  `msgid = type<<24 | srcport<<12 | dstport`. Messages never straddle a
  slot: the writer pads the remainder with a PADDING message.
* **TX**: write payload into `own slot_queue[(tx_pos/4096) & 63]` at
  `tx_pos & 4095`, publish by storing the new `tx_pos`, then *signal*:
  set `remote->trigger.fired = 1` and, if `armed`, write the doorbell
  register `PERIPHERAL_BASE + 0xB848` (BELL2) to interrupt the VC.
* **RX**: compare our `rx_pos` against `remote->tx_pos`; parse headers
  out of the remote's slots. When `rx_pos` crosses a slot boundary the
  consumed slot is **recycled**: append its index at
  `remote->slot_queue[remote->slot_queue_recycle & 63]`, increment
  `slot_queue_recycle`, signal `remote->recycle`. The VC replenishes our
  TX pool the same way in the other direction.
* We never enable the VC->ARM doorbell interrupt; the poll loop reads
  the shared counters directly (the `fired` flags exist to let a sleeping
  side be woken - a poller does not need them).

Message types used: `CONNECT`(1) handshake, `OPEN`(2)/`OPENACK`(3) for
the `'mmal'` and `'SMEM'` services, `DATA`(5) for everything else, and
`PAUSE`(10)/`RESUME`(11) (answered, never initiated). The protocol's
bulk-transfer messages (6-9) are never used - see below.

### No bulk transfers: everything is zero-copy

The protocol also defines *bulk* transfers, where the ARM posts a
pagelist describing a buffer and the VideoCore (the bus master - the ARM
never DMAs here) copies the payload with its own DMA. This client makes
none. Every MMAL port is put into zero-copy mode instead, so a buffer is
identified on the wire by a VideoCore memory handle and the codec reads
and writes that memory directly: the access unit is fetched out of our
staging buffer, and the decoded picture is written into the buffer the
HVS is already scanning out. That removes ~650 KB of DMA per frame, and
with it the pagelist machinery, the bulk queues and the message/bulk
pairing rules that go with them.

The handles come from the firmware's **SMEM** service (`src/rpi/vcsm.c`,
VCHIQ fourcc `SMEM`), because MMAL will not accept a handle from the
mailbox allocator. We use its `IMPORT` operation rather than `ALLOC`:
the memory stays ours, allocated exactly as before, and the import only
tells the VideoCore it may decode into it. The display plane layout is
therefore unchanged - the Y/Cb/Cr pointers the HVS uses are still the
ones `screen.c` set up.

### Memory & caches - why there is no cache maintenance anywhere

`cache.c` maps ARM RAM write-back cacheable but maps the region between
the ARM/VC memory split and the peripherals (i.e. the VC heap, where
`screen_allocate_buffer()`-style allocations land, descriptor `0x11C06`)
effectively uncached. Every buffer VCHIQ touches - slot memory,
bitstream staging, decoded frames - is allocated from the VC heap
(`vchiq_alloc_shared()`, same mailbox flags as the screen code) and
accessed by the ARM through that uncached window. Bus addresses handed
to the VC use `vchiq_bus_addr()`: the `0x40000000` **L2-coherent** alias
on BCM2835 (Pi 1 / Zero), where the ARM's memory port is routed through
the VideoCore L2 and the `0xC` alias gives the two sides torn views of
the slot ring; the `0xC0000000` uncached alias on Pi 2/3, where the ARM
bypasses that cache.
Result: full coherency by construction and zero clean/invalidate calls,
on both sides of every buffer the decoder touches.

## 3. MMAL: the RPC layer (src/rpi/mmal_vc.c)

Open VCHIQ service fourcc `'mmal'`, version 16 min 10. Every message
begins:

```c
struct mmal_worker_msg_header { u32 magic='mmal'; u32 msgid;
    u32 control_service; u32 context; u32 status; u32 pad; };
```

Synchronous calls (create/enable/port-info/actions/parameters) set
`context` to a cookie; the reply echoes it. Only two message kinds
arrive unsolicited: `BUFFER_TO_HOST` and `EVENT_TO_HOST`. The wire
structs in `mmal_vc.h` are byte-exact copies
of userland's `mmal_vc_msgs.h` with pointers as `u32` (they are opaque
cookies on the wire); `_Static_assert`s pin the layouts.

Buffer traffic, the part worth understanding. Both ports are switched to
zero-copy first, with a `MMAL_PARAMETER_ZERO_COPY` parameter set between
`PORT_INFO_GET` and `PORT_ACTION ENABLE`. That has to be re-asserted
after every `PORT_INFO_GET`, because re-reading the port rebuilds our
copy of its state and the VideoCore forgets the mode with it.

* **Host -> VC** (`mmal_vc_submit_buffer`): send `BUFFER_FROM_HOST`
  carrying `drvbuf {magic, component_handle, port_handle,
  client_context}` plus a serialized buffer header, with
  `is_zero_copy = 1` and the header's `data` field holding the buffer's
  SMEM handle instead of an address. Nothing follows it; the component
  reads the access unit out of that memory itself.
* **VC -> Host**: a `BUFFER_TO_HOST` message arrives with our
  `client_context` cookie and the updated header (length, flags, pts).
  The picture is already in our buffer, so the buffer is complete the
  moment the message lands. Empty returns - an input buffer being
  released, flush residue - look exactly the same.
* A zero-length **EOS** marker is an ordinary `BUFFER_FROM_HOST`. The
  `BUFFER_FROM_HOST_ZEROLEN` msgid exists only to carry a dummy bulk
  transfer, and the reference client excludes zero-copy ports from it.
* **Port flush** is the plain `PORT_ACTION_FLUSH`. The separate
  `PORT_FLUSH` message exists to order a flush against payload still
  crossing as a bulk transfer, which a zero-copy port never has.

## 4. The decoder component (src/rpi/h264dec.c)

`ril.video_decode` has 1 input, 1 output port. Bring-up sequence:

1. `COMPONENT_CREATE "ril.video_decode"` -> component handle.
2. `PORT_INFO_GET` input; zero-copy; set format `video/H264` 768x576,
   buffer_num=2, buffer_size=512K; `PORT_INFO_SET`; `COMPONENT_ENABLE`;
   `PORT_ACTION ENABLE` (echoing the port struct).
3. **Configure and enable the output port immediately**, at the format we
   already know (these streams are always 768x576 I420): `PORT_INFO_GET`
   output, zero-copy, force encoding `I420`, `PORT_INFO_SET`,
   `PORT_ACTION ENABLE`, then submit the display buffers as output
   buffers.

   Doing this up front is not an optimisation, it is required. MMAL
   delivers `MMAL_EVENT_FORMAT_CHANGED` *as a buffer on the output port*,
   so waiting for that event with the output port disabled deadlocks: the
   component has no buffer in which to tell us anything. (The ffmpeg MMAL
   decoder enables its output up front for the same reason.)
4. Feed access units. The component parses the SPS and raises
   `MMAL_EVENT_FORMAT_CHANGED`; the handler re-runs step 3 - deferred to
   `h264dec_poll()`, because MMAL calls must not run inside a VCHIQ
   callback.
5. Every decoded frame comes back as a filled output buffer =
   **I420 written by the VC directly into the buffer the HVS scans out**.

Sizing rules: I420 output requires width % 32 == 0 and height % 16 == 0
or the VC pads the stride and the plane pointer arithmetic changes.
768x576 fits exactly (Y 442368 + U 110592 + V 110592 = 663552 bytes).

Freeze frame / stills: submit the one AU with `FRAME_END|KEYFRAME`, then
a zero-length `EOS` buffer. The EOS forces the decoder to emit the
picture without waiting for a successor. `h264dec_resume()` (flush both
ports) clears the EOS state before the next feed.

`pts` is (ab)used to carry the frame number through the decoder, so the
display side knows which picture it is showing with no other bookkeeping.

## 5. Memory budget

| allocation | size | where |
|---|---|---|
| VCHIQ slots + fragment pool | 136 KB | VC heap, uncached |
| input staging x2 | 1 MB | VC heap |
| frame buffers x2 (display + decode) | 1.3 MB | VC heap |
| frame index (54000 frames) | 216 KB | ARM heap |
| audio ring (8 frames) | 64 KB | ARM heap |
| VC-internal codec + its buffer pools | ~10-15 MB | inside gpu_mem |

GPU buffer handles for the frame buffers are parked at `0x7C20` (words
2-4, magic `'VBF2'`) so a `kernel.now` chain-boot can release them - the
same leak-avoidance mechanism the still-frame buffer already used.

## 6. The .pvf container and offline preparation

See `src/pvf.h` for the exact layout (64 B header, u32-per-frame index,
then per-frame records of `{video_len, audio_len} + AU + PCM`). Design
drivers:

* **one seek + one or two sequential reads per random access** (record
  header+AU, optionally audio);
* every AU self-contained (SPS+PPS+IDR) => decode any frame cold;
* audio pre-resampled to **46875 Hz** = the PWM DMA rate in
  `rpi/audio.c`, so 25 fps gives exactly 1875 stereo samples per frame
  and playback does no rate conversion;
* 32-bit offsets: 4 GB cap = the FAT32 file limit anyway. A Domesday
  side (~54000 frames) at crf 17-18 lands ~1.5-2.5 GB.

Encoding choices in `make_pvf.py`: libx264, `keyint=1` (all-intra),
`repeat-headers=1` (SPS/PPS per IDR), `bframes=0`, `ref=1`, High
profile/CABAC (VC4 decodes 1080p CABAC; 768x576 is easy). All-intra
costs roughly 3-5x the bitrate of normal H264 but buys O(1) random
access, no reference-chain state, and byte-identical stills - the right
trade for a LaserDisc emulator. If capacity ever matters more than
simplicity, a GOP-based variant would index only IDR frames and decode
forward from the preceding IDR on seeks; nothing in the transport would
change.

## 7. Player integration (src/videoplayer.c, fcode.c)

* `videoplayer_init()` (runs after the filesystem, before the frame
  buffer, as before): if `video.pvf` opens and `h264dec_init()`
  succeeds, it creates the **4:2:0** HVS plane (new
  `screen_create_YUV420_plane()`, HVS pixel format 8 - the existing
  4:2:2 path uses format 0xA), registers a Pi1MHz poll task and shows
  picture 1. Otherwise: the classic `frame.lz` 4:2:2 still, unchanged.
* The poll task runs the whole pipeline: pending-seek handling (flush,
  feed, still-or-play), keeping <=2 AUs in flight during play, pacing
  flips with the system timer (40 ms), reverse play as backwards-stepped
  stills (each an independent IDR decode - sustains full rate), audio
  ring -> PWM DMA, and end-of-range/stop-register halts.
* A "page flip" is `screen_set_YUV_pointers()` writing the three plane
  pointers in HVS context memory; the old buffer is recycled to the
  decoder afterwards.
* F-codes now wired through (`fcode.c`): `Fxxxxx R/N/Q/S/I`, bare `N`
  (play), `O` (play reverse), `L`/`M` (step), `*` (halt), `/` (pause),
  `+yy`/`-yy` (instant jump), `A0/A1`, `B0/B1` (audio channels), and
  `?F` now answers with the real picture number `Fxxxxx`.
* Audio shares the PWM path with Music 5000/BeebSID. Ownership is
  enforced: whoever calls `rpi_audio_init()` first owns it
  (`rpi_audio_active()`), and the player stands down with a log message
  when the path is taken. The M5000 is enabled by default, so video
  sound needs `M5000_addr=-1` in Pi1MHz.cfg. (A mixer would be the
  proper fix if ever needed.)
* A Beeb reset re-runs every emulator init; `videoplayer_init` detects
  the warm restart, calls `h264dec_reset()` to detach the frame buffers
  from the live component BEFORE releasing them, and frees the previous
  index/audio-ring allocations - Break during playback is safe.

## 8. Troubleshooting

Symptoms and where to look, if a card ever comes up without video:

* **`vchiq: firmware rejected init`** - the card is still booting
  `start_cd.elf`, or `start.elf`/`fixup.dat` do not match each other.
* **Decoder starts but the frame count on `/status` stays 0** -
  `vd_use_vpu0=1` is missing from `config.txt` (§1).
* **`h264: no SMEM service`** - the firmware predates the `SMEM` service;
  zero-copy is the only frame path, so the player falls back to the
  still frame.
* **No `FORMAT_CHANGED`, nothing decodes** - suspect the stream: every
  access unit must be a complete SPS+PPS+IDR. `tools/verify_pvf.py`
  checks the container; `ffmpeg -f lavfi -i testsrc` makes a known-good
  clip.
* **Green picture** - Cb/Cr plane pointers swapped in `flip_pending`.
  **Stripes** - stride, i.e. a width that is not a multiple of 32.
* **Video works but the Beeb interface is dead** - something other than
  `vd_use_vpu0` took VPU core 1; check `TAG_LAUNCH_VPU1` succeeded.

Note that `kernel.now` chain-booting does not work with video: it is an
ARM-only warm restart, so the VideoCore keeps the previous connection
state and ignores the new kernel's CONNECT. Video kernels must boot from
SD.

## 9. Performance budget (Pi Zero, 1 GHz ARM1176)

Per frame at 25 fps (40 ms):

| step | cost | who |
|---|---|---|
| SD read ~30 KB AU (+7.5 KB audio) | ~1-3 ms | ARM (FatFS, fastseek cltbl) |
| VCHIQ msgs (~300 B uncached writes) | ~10 us | ARM |
| decode 768x576 intra into our buffer | ~6.6 ms | VC hardware |
| HVS pointer flip + poll overhead | ~10 us | ARM |

Measured end to end: 152 fps free-run (6.6 ms/frame) with the 1MHz bus
handler running, against the 40 ms budget of 25 fps. ARM load is a few
percent, dominated by the SD read - the "Pi has lots of other tasks"
requirement is met by construction. If SD contention
with BeebSCSI ever bites, raise the pipeline depth (in_flight limit and
`H264DEC_INPUT_BUFFERS`) to ride through longer stalls.

## 10. Alternatives considered

* **Software decode on ARM**: ARM1176 manages roughly 5-10 fps at
  768x576 intra - not viable, and it would eat the CPU the bus emulation
  needs.
* **Keep LZ4 YUV stills** (status quo): ~200-400 KB/frame -> a Domesday
  side would need >15 GB and play at SD-read speed. H264 intra is
  ~10-20x smaller.
* **MJPEG via the VC image decoder**: same VCHIQ/MMAL plumbing needed,
  worse quality/bitrate than H264 intra - no win.
* **OpenMAX IL instead of MMAL**: same transport, clunkier RPC (it is
  what hello_video uses); MMAL's worker protocol is the simpler wire
  format, which is why it was chosen.
* **Bulk frame transfers** (the obvious first implementation): copy each
  decoded picture from the VideoCore into our buffers with a per-frame
  VCHIQ bulk DMA. It works and is bit-identical, but costs ~650 KB of
  DMA per frame and about 5% of the total decode rate, and it needs the
  pagelist/bulk-queue half of the VCHIQ client plus the message/bulk
  pairing rules throughout MMAL. Zero-copy via `SMEM` replaced it.
* **Zero-copy by letting the decoder allocate** (`SMEM` `ALLOC` rather
  than `IMPORT`): simpler on the face of it, but the frame buffers would
  then be the firmware's, with its choice of plane layout, and the HVS
  scan-out setup in `screen.c` would have to follow it. Importing our own
  buffers keeps the display side unchanged.

## 11. Protocol references

Vendored nothing; layouts were transcribed (and pinned by static
asserts) from:

* Linux `drivers/staging/vc04_services/interface/vchiq_arm/`
  (`vchiq_core.c/h`, `vchiq_arm.c`, `vchiq_pagelist.h`), rpi-6.1.y -
  slot/message/pagelist formats, init tag `0x48010`, doorbell offsets.
* raspberrypi/userland `interface/mmal/vc/mmal_vc_msgs.h`,
  `mmal_vc_client.c`, `mmal_vc_api.c` - MMAL worker messages and the
  message/bulk pairing rules; `interface/mmal/*.h` - public structs.
* `hello_pi/hello_video` + omxplayer - component usage patterns
  (port-settings-changed handshake, EOS/flush semantics).
