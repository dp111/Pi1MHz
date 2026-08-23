/*

    Take video from SDCARD and display it on the screen

    Hardware video player: a "video.pvf" file (built offline by
    tools/make_pvf.py: all-intra H264 + 46875 Hz PCM + frame index) is
    decoded with the VideoCore hardware H264 decoder (rpi/h264dec.c,
    MMAL-over-VCHIQ) into two GPU frame buffers that double as the HVS
    4:2:0 plane sources. The ARM never touches pixel data: it reads
    ~30 KB of bitstream per frame from SD into an uncached staging
    buffer and flips three HVS pointer registers per frame. Everything
    runs from a Pi1MHz poll task; the decoder needs the FULL start.elf
    and gpu_mem=64 (see docs/dev/h264-hardware-decode.md).

    Playback control mirrors a Philips LaserVision player: the F-code
    layer (BeebSCSI/fcode.c) calls the videoplayer_* functions below
    for goto/still/play/step, which is what the Domesday VFS ROM
    drives. Every frame is an IDR picture so any picture number can be
    shown with a single seek + single decode; freeze frame is simply
    "stop feeding the decoder".

    With no video file, or without the firmware to decode one, the
    video plane stays disabled and the Beeb display is unchanged.

*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "rpi/screen.h"
#include "BeebSCSI/fatfs/ff.h"
#include "rpi/rpi.h"
#include "rpi/systimer.h"
#include "rpi/audio.h"
#include "rpi/h264dec.h"
#include "Pi1MHz.h"
#include "pvf.h"
#include "videoplayer.h"

#define YUV_PLANE 0

#define PVF_FILENAME "video.pvf"

/* Where the GPU buffer handles are parked so the NEXT kernel can hand them
 * back over a kernel.now chain-boot - see the detailed rationale in git
 * history / docs: the VideoCore keeps allocations across an ARM warm
 * restart and the allocating and releasing kernels are different builds,
 * so the handles live at a fixed low-RAM address, not in .noinit.
 *
 * [0] magic 'VBUF', [1] still-frame buffer handle of a PRE-1.31 kernel
 * [2] magic 'VBF2', [3][4] the two H264 frame buffer handles
 *
 * Word [1] is only ever released here, never written: the 4:2:2 still
 * frame it belonged to is gone, but chain-booting from an older kernel
 * would otherwise leak its 864 KB out of the pool the decoder needs.
 */
#define VIDEOBUF_PERSIST_BASE 0x00007C20u
#define videobuf_magic    (((volatile uint32_t *)VIDEOBUF_PERSIST_BASE)[0])
#define videobuf_handle   (((volatile uint32_t *)VIDEOBUF_PERSIST_BASE)[1])
#define videobuf_magic2   (((volatile uint32_t *)VIDEOBUF_PERSIST_BASE)[2])
#define videobuf_handle2(n) (((volatile uint32_t *)VIDEOBUF_PERSIST_BASE)[3 + (n)])
#define VIDEOBUF_MAGIC    0x56425546u   /* 'VBUF' */
#define VIDEOBUF_MAGIC2   0x56424632u   /* 'VBF2' */

/* ------------------------------------------------------------------ */
/* Player state                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    VP_IDLE,                         /* no video open */
    VP_STILL,                        /* frozen on cur_picture */
    VP_PLAY,                         /* pipelined forward play */
    VP_PLAY_REV,                     /* reverse = paced backwards stills */
} vp_mode_t;

#define NUM_FRAME_BUFFERS 2

static struct {
    bool open;
    FIL file;
    pvf_header_t hdr;
    uint32_t *index;                 /* frame_count record offsets */

    uint32_t frame_bytes;            /* width*height*3/2 */
    uint32_t buf_phys[NUM_FRAME_BUFFERS];
    uint32_t displayed_phys;         /* buffer currently scanned out (0 = none) */
    uint32_t pending_phys;           /* decoded, waiting for display slot */
    int64_t  pending_pts;

    vp_mode_t mode;
    vp_mode_t prev_mode;             /* for the 'Q' goto op */
    uint32_t cur_picture;            /* 1-based, on screen */
    uint32_t next_frame;             /* 0-based, next to feed the decoder */
    uint32_t stop_picture;           /* stop register, 0 = none */
    uint32_t info_picture;           /* info register ('I' op) */
    int32_t  in_flight;              /* AUs submitted minus frames back */

    int32_t  seek_frame;             /* pending random access, -1 = none */
    char     seek_op;

    /* pacing */
    uint32_t frame_period_us;
    uint32_t next_frame_due;         /* systimer target for the next flip */

    /* audio */
    bool audio_present;
    bool audio_on[2];
    bool audio_inited;
    uint8_t *audio_ring;             /* s16le stereo, ring buffer */
    uint32_t audio_ring_size;
    uint32_t audio_wr, audio_rd;     /* byte positions (mod size) */
    int32_t  audio_err_l, audio_err_r;   /* rpi_audio_pack dither state */
} vp;

/* ------------------------------------------------------------------ */
/* Audio                                                              */
/* ------------------------------------------------------------------ */

#define AUDIO_RING_FRAMES 8          /* of video, ~320 ms */

static uint32_t audio_ring_level(void)
{
    /* wr/rd are monotonic, so the level is the plain difference - masking
       it would alias a completely full ring to "empty" */
    return vp.audio_wr - vp.audio_rd;
}

static void audio_ring_write(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        vp.audio_ring[vp.audio_wr & (vp.audio_ring_size - 1u)] = data[i];
        vp.audio_wr++;
    }
}

static void audio_ring_reset(void)
{
    vp.audio_rd = vp.audio_wr = 0;
}

/* Move samples from the ring into the PWM DMA buffer. Each DMA refill is
   DMA_BUFFER_SIZE words = DMA_BUFFER_SIZE/2 stereo sample pairs. */
static void audio_pump(void)
{
    if (!vp.audio_inited)
        return;

    /* When playing, ALWAYS consume the ring at the DMA rate - muting only
       silences the output. Consuming while muted keeps audio in sync with
       the pictures, so A1 after A0 resumes at the right place instead of
       replaying a third of a second of stale sound. */
    bool playing = (vp.mode == VP_PLAY);

    while (rpi_audio_buffer_free_space()) {
        uint32_t *dst = rpi_audio_buffer_pointer();
        for (uint32_t i = 0; i < DMA_BUFFER_SIZE / 2; i++) {
            int16_t l = 0, r = 0;
            if (playing && audio_ring_level() >= 4) {
                uint32_t rd = vp.audio_rd & (vp.audio_ring_size - 1u);
                l = (int16_t)(vp.audio_ring[rd] | (vp.audio_ring[rd + 1] << 8));
                r = (int16_t)(vp.audio_ring[rd + 2] | (vp.audio_ring[rd + 3] << 8));
                vp.audio_rd += 4;
            }
            /* Channel mutes emulate the LaserDisc A/B tracks */
            if (!vp.audio_on[0]) l = 0;
            if (!vp.audio_on[1]) r = 0;
            *dst++ = rpi_audio_pack(l, &vp.audio_err_l);
            *dst++ = rpi_audio_pack(r, &vp.audio_err_r);
        }
        rpi_audio_samples_written();
        if (!playing)
            break;
    }
}

/* ------------------------------------------------------------------ */
/* SD reading / decoder feeding                                       */
/* ------------------------------------------------------------------ */

/* Read record 'frame' (0-based) and submit its access unit. Audio PCM is
   pulled into the ring when 'with_audio'. 'eos' forces the decoder to
   emit the picture immediately (used for stills/steps). */
static bool feed_frame(uint32_t frame, bool with_audio, bool eos)
{
    if (frame >= vp.hdr.frame_count)
        return false;

    uint32_t max;
    uint8_t *staging = h264dec_get_input_buffer(&max);
    if (!staging)
        return false;                /* both input buffers in flight */

    pvf_record_t rec;
    UINT n;
    if (f_lseek(&vp.file, vp.index[frame]) != FR_OK ||
        f_read(&vp.file, &rec, sizeof(rec), &n) != FR_OK || n != sizeof(rec) ||
        rec.video_len > max) {
        LOG_INFO("videoplayer: bad record %"PRIu32"\r\n", frame);
        h264dec_cancel_input();
        return false;
    }

    uint32_t vlen = (rec.video_len + 3u) & ~3u;
    if (f_read(&vp.file, staging, vlen, &n) != FR_OK || n != vlen) {
        h264dec_cancel_input();
        return false;
    }

    if (with_audio && vp.audio_present && rec.audio_len &&
        rec.audio_len <= vp.audio_ring_size - audio_ring_level()) {
        /* Reuse the tail of the staging buffer for the audio hop - it is
           big enough (max_video_len + audio << buffer size) */
        uint8_t *ah = staging + vlen;
        if (rec.audio_len <= max - vlen &&
            f_read(&vp.file, ah, rec.audio_len, &n) == FR_OK && n == rec.audio_len)
            audio_ring_write(ah, rec.audio_len);
    }

    /* pts carries the frame number so the display side knows what it is
       looking at without any other bookkeeping */
    if (!h264dec_submit_input(rec.video_len, (int64_t)frame, eos))
        return false;

    vp.in_flight++;
    return true;
}

/* Decoded frame callback - runs inside h264dec_poll(); record only. */
static void frame_decoded(uint32_t phys, int64_t pts, bool eos)
{
    if (eos && !phys)
        return;
    if (vp.in_flight > 0)
        vp.in_flight--;
    if (vp.pending_phys && vp.pending_phys != vp.displayed_phys) {
        /* Overrun: a frame was never displayed (e.g. seek burst). Give
           its buffer back so the decoder is not starved. */
        h264dec_recycle_output(vp.pending_phys);
    }
    vp.pending_phys = phys;
    vp.pending_pts = pts;
}

/* Put the pending decoded frame on screen and recycle the old one. */
static void flip_pending(void)
{
    uint32_t phys = vp.pending_phys;
    if (!phys)
        return;
    vp.pending_phys = 0;

    uint32_t w = vp.hdr.width, h = vp.hdr.height;
    screen_set_YUV_pointers(YUV_PLANE,
                            phys,
                            phys + w * h,               /* Cb (U) */
                            phys + w * h + (w / 2) * (h / 2)); /* Cr (V) */

    if (vp.displayed_phys && vp.displayed_phys != phys)
        h264dec_recycle_output(vp.displayed_phys);
    vp.displayed_phys = phys;
    vp.cur_picture = (uint32_t)vp.pending_pts + 1u;
}

/* ------------------------------------------------------------------ */
/* Poll task                                                          */
/* ------------------------------------------------------------------ */

static void videoplayer_poll(void)
{
    if (!vp.open)
        return;

    h264dec_poll();

    /* Feed the PWM before any SD work: the DMA runway is only ~9.5 ms
       and a seek-burst of AU reads below can exceed that */
    audio_pump();

    /* Pending random access? Flush whatever is mid-pipeline first. */
    if (vp.seek_frame >= 0) {
        uint32_t target = (uint32_t)vp.seek_frame;
        if (vp.in_flight)
            h264dec_resume();        /* discard stale pictures */
        vp.in_flight = 0;
        audio_ring_reset();

        bool play = (vp.seek_op == 'N') ||
                    (vp.seek_op == 'Q' && vp.prev_mode == VP_PLAY);
        if (feed_frame(target, play, !play)) {
            vp.seek_frame = -1;
            vp.next_frame = target + 1u;
            vp.mode = play ? VP_PLAY : VP_STILL;
            vp.next_frame_due = RPI_GetSystemTime() + vp.frame_period_us;
        }
        /* else: staging busy, retry next poll */
    }

    switch (vp.mode) {
    case VP_PLAY: {
        /* Keep the decoder pipeline primed (up to 2 AUs deep) */
        uint32_t limit = vp.stop_picture ? vp.stop_picture : vp.hdr.frame_count;
        while (vp.in_flight < 2 && vp.next_frame < limit) {
            if (!feed_frame(vp.next_frame, true, false))
                break;
            vp.next_frame++;
        }

        /* Display at the frame rate */
        if (vp.pending_phys &&
            (int32_t)(RPI_GetSystemTime() - vp.next_frame_due) >= 0) {
            flip_pending();
            vp.next_frame_due += vp.frame_period_us;
        }

        /* End of play range -> still on the last picture. The stop
           register is one-shot, as on the real player: reaching it clears
           it, so a later bare 'N' plays on rather than instantly
           re-stilling. */
        if (vp.cur_picture >= limit && !vp.in_flight && !vp.pending_phys) {
            vp.mode = VP_STILL;
            if (vp.stop_picture && vp.cur_picture >= vp.stop_picture)
                vp.stop_picture = 0;
            h264dec_resume();
        }
        break;
    }

    case VP_PLAY_REV:
        /* Reverse play = random-access stills marched backwards. Each is
           an independent IDR decode, so this sustains full rate. */
        if (vp.pending_phys)
            flip_pending();
        if ((int32_t)(RPI_GetSystemTime() - vp.next_frame_due) >= 0 &&
            vp.in_flight == 0) {
            if (vp.cur_picture > 1) {
                if (feed_frame(vp.cur_picture - 2u, false, true))
                    vp.next_frame_due += vp.frame_period_us;
            } else {
                vp.mode = VP_STILL;
            }
        }
        break;

    case VP_STILL:
    case VP_IDLE:
    default:
        if (vp.pending_phys)
            flip_pending();          /* show seek/step results at once */
        break;
    }

    audio_pump();
}

/* ------------------------------------------------------------------ */
/* F-code control surface                                             */
/* ------------------------------------------------------------------ */

bool videoplayer_active(void)
{
    return vp.open;
}

uint32_t videoplayer_picture_number(void)
{
    return vp.cur_picture;
}

void videoplayer_goto(uint32_t picture, char op)
{
    if (!vp.open || picture == 0)
        return;
    if (picture > vp.hdr.frame_count)
        picture = vp.hdr.frame_count;

    switch (op) {
    case 'S':                        /* stop register */
        vp.stop_picture = picture;
        return;
    case 'I':                        /* info register */
        vp.info_picture = picture;
        return;
    case 'R':                        /* goto & still */
    case 'N':                        /* goto & play */
    case 'Q':                        /* goto & previous mode */
        vp.prev_mode = (vp.mode == VP_PLAY) ? VP_PLAY : VP_STILL;
        vp.seek_frame = (int32_t)(picture - 1u);
        vp.seek_op = op;
        return;
    default:
        return;
    }
}

void videoplayer_play_fwd(void)
{
    if (!vp.open)
        return;
    if (vp.mode != VP_PLAY) {
        vp.next_frame = vp.cur_picture;      /* picture is 1-based */
        vp.in_flight = 0;
        h264dec_resume();
        audio_ring_reset();
        vp.mode = VP_PLAY;
        vp.next_frame_due = RPI_GetSystemTime() + vp.frame_period_us;
    }
}

void videoplayer_play_rev(void)
{
    if (!vp.open)
        return;
    if (vp.in_flight)
        h264dec_resume();
    vp.in_flight = 0;
    vp.mode = VP_PLAY_REV;
    vp.next_frame_due = RPI_GetSystemTime() + vp.frame_period_us;
}

void videoplayer_halt(void)
{
    if (!vp.open)
        return;
    vp.mode = VP_STILL;
}

void videoplayer_pause(void)
{
    if (!vp.open)
        return;
    vp.mode = VP_STILL;
}

void videoplayer_step(int delta)
{
    if (!vp.open || vp.cur_picture == 0)
        return;
    int32_t target = (int32_t)vp.cur_picture + delta;   /* new picture, 1-based */
    if (target < 1)
        target = 1;
    videoplayer_goto((uint32_t)target, 'R');
}

void videoplayer_audio_enable(int channel, bool on)
{
    if (channel >= 0 && channel < 2)
        vp.audio_on[channel] = on;
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

static bool pvf_open_file(void)
{
    UINT n;
    if (f_open(&vp.file, PVF_FILENAME, FA_READ) != FR_OK)
        return false;

    /* Random access seeks constantly; give FatFS a cluster link map so
       f_lseek is O(1) instead of walking the FAT chain from the start
       for every backwards seek (FF_USE_FASTSEEK is enabled). 256 entries
       cover 127 fragments - plenty for a file copied onto the card in
       one go; if the file is more fragmented fall back to slow seeks. */
    static DWORD clmt[256];
    clmt[0] = sizeof(clmt) / sizeof(clmt[0]);
    vp.file.cltbl = clmt;
    if (f_lseek(&vp.file, CREATE_LINKMAP) != FR_OK) {
        LOG_INFO("videoplayer: " PVF_FILENAME " heavily fragmented; seeks will be slow\r\n");
        vp.file.cltbl = NULL;
    }

    if (f_read(&vp.file, &vp.hdr, sizeof(vp.hdr), &n) != FR_OK ||
        n != sizeof(vp.hdr) ||
        vp.hdr.magic != PVF_MAGIC || vp.hdr.version != PVF_VERSION ||
        vp.hdr.frame_count == 0 ||
        vp.hdr.frame_count > 400000 ||   /* bounds the index malloc too */
        vp.hdr.fps_num == 0 || vp.hdr.fps_den == 0 ||
        vp.hdr.audio_bytes_per_frame > 65536 ||
        vp.hdr.width == 0 || vp.hdr.width > 1920 ||
        vp.hdr.height == 0 || vp.hdr.height > 1088) {
        LOG_INFO("videoplayer: bad " PVF_FILENAME " header\r\n");
        f_close(&vp.file);
        return false;
    }

    vp.index = malloc(vp.hdr.frame_count * sizeof(uint32_t));
    if (!vp.index) {
        f_close(&vp.file);
        return false;
    }
    if (f_lseek(&vp.file, vp.hdr.index_offset) != FR_OK ||
        f_read(&vp.file, vp.index, vp.hdr.frame_count * sizeof(uint32_t), &n) != FR_OK ||
        n != vp.hdr.frame_count * sizeof(uint32_t)) {
        LOG_INFO("videoplayer: bad " PVF_FILENAME " index\r\n");
        free(vp.index);
        vp.index = NULL;
        f_close(&vp.file);
        return false;
    }
    return true;
}

/* Survives the memset of vp below: whether THIS module claimed the PWM
   audio path on a previous init (a Beeb reset re-runs every emulator
   init, and we must recognise our own claim as ours). */
static bool audio_owned_by_player;

/* Abandon the hardware-decode path part way through bring-up: give
   everything back and leave the video plane disabled. The order matters:
   h264dec_reset() first, so the decoder drops its SMEM imports and
   disables the output port, before any frame buffer goes back to the GPU
   pool - otherwise the VideoCore could still be armed to decode into
   memory we have just returned. */
static void video_give_up(uint32_t handles[NUM_FRAME_BUFFERS])
{
    h264dec_reset();
    for (int i = 0; i < NUM_FRAME_BUFFERS; i++)
        if (handles[i]) {
            screen_release_buffer(handles[i]);
            handles[i] = 0;
        }
    free(vp.index);
    vp.index = NULL;
    f_close(&vp.file);
    vp.open = false;
    screen_plane_enable(YUV_PLANE, false);
}

void videoplayer_init(uint8_t instance, uint8_t address)
{
    (void)instance;
    (void)address;

    screen_plane_enable(YUV_PLANE, false);

    /* Warm re-init (Beeb reset re-runs emulator inits): tear the previous
       instance down FIRST. h264dec_reset() detaches the frame buffers
       from the still-running decoder - without it, releasing the GPU
       buffers below would leave the VideoCore free to DMA into freed
       memory. Then release the heap the old instance held. */
    if (vp.open) {
        h264dec_reset();
        f_close(&vp.file);
    }
    free(vp.index);
    free(vp.audio_ring);

    memset(&vp, 0, sizeof(vp));
    vp.seek_frame = -1;

    /* An older kernel's 4:2:2 still-frame buffer, if we chain-booted from
       one: the still is gone, so just give the memory back. */
    if (videobuf_magic == VIDEOBUF_MAGIC && videobuf_handle != 0u) {
        screen_release_buffer(videobuf_handle);
        videobuf_handle = 0u;
        videobuf_magic = 0u;
    }

    /* Release H264 frame buffers a previous chain-booted kernel leaked */
    if (videobuf_magic2 == VIDEOBUF_MAGIC2) {
        for (int i = 0; i < NUM_FRAME_BUFFERS; i++)
            if (videobuf_handle2(i)) {
                screen_release_buffer(videobuf_handle2(i));
                videobuf_handle2(i) = 0;
            }
        videobuf_magic2 = 0;
    }

    if (!pvf_open_file()) {
        /* No video file: leave the plane off, the Beeb display is all
           there is to show. */
        LOG_DEBUG("videoplayer_init: no " PVF_FILENAME "\r\n");
        return;
    }

    vp.frame_bytes = vp.hdr.width * vp.hdr.height * 3u / 2u;
    uint64_t period = (uint64_t)1000000 * vp.hdr.fps_den / vp.hdr.fps_num;
    vp.frame_period_us = (uint32_t)period;

    /* Start the hardware decoder first - if the firmware is start_cd.elf
       this fails cleanly and the video plane simply stays off. */
    if (!h264dec_init(vp.hdr.width, vp.hdr.height, frame_decoded)) {
        LOG_INFO("videoplayer: no hardware decoder - check start.elf/gpu_mem\r\n");
        free(vp.index);
        vp.index = NULL;
        f_close(&vp.file);
        return;
    }

    /* Zeroed: screen_allocate_buffer does not write the handle when the
       allocation tag itself fails, and the cleanup below tests them */
    uint32_t handles[NUM_FRAME_BUFFERS] = {0};
    for (int i = 0; i < NUM_FRAME_BUFFERS; i++) {
        vp.buf_phys[i] = screen_allocate_buffer(vp.frame_bytes, &handles[i]);
        if (!vp.buf_phys[i]) {
            LOG_INFO("videoplayer: no GPU memory for frames (gpu_mem too small?)\r\n");
            video_give_up(handles);
            return;
        }
        /* black I420 */
        uint8_t *p = (uint8_t *)(uintptr_t)vp.buf_phys[i];
        memset(p, 0x10, vp.hdr.width * vp.hdr.height);
        memset(p + vp.hdr.width * vp.hdr.height, 0x80,
               vp.hdr.width * vp.hdr.height / 2u);
        if (!h264dec_add_output_buffer(vp.buf_phys[i], vp.frame_bytes)) {
            /* Without every output buffer the decoder can never produce a
               frame, so carrying on would leave the screen permanently
               black - give everything back instead. */
            LOG_INFO("videoplayer: output buffer %d not registered\r\n", i);
            video_give_up(handles);
            return;
        }
    }

    videobuf_magic2 = VIDEOBUF_MAGIC2;
    for (int i = 0; i < NUM_FRAME_BUFFERS; i++)
        videobuf_handle2(i) = handles[i];

    screen_create_YUV420_plane(YUV_PLANE, vp.hdr.width, vp.hdr.height,
                               vp.buf_phys[0]);
    vp.displayed_phys = 0;           /* nothing decoded on it yet */
    screen_plane_enable(YUV_PLANE, true);

    vp.audio_present = (vp.hdr.audio_rate != 0);
    if (vp.audio_present && rpi_audio_active() && !audio_owned_by_player) {
        /* The Music 5000 (enabled by default) or BeebSID already owns the
           PWM/DMA path, and its poll fills the same buffers - two writers
           means garbled audio, so stand down. Disable the other emulator
           (M5000_addr=-1 in Pi1MHz.cfg) to give the video its sound. */
        LOG_INFO("videoplayer: audio disabled - PWM in use (set M5000_addr=-1 for video sound)\r\n");
        vp.audio_present = false;
    }
    if (vp.audio_present) {
        vp.audio_ring_size = 1;
        while (vp.audio_ring_size < vp.hdr.audio_bytes_per_frame * AUDIO_RING_FRAMES)
            vp.audio_ring_size <<= 1;             /* power of two for the masks */
        vp.audio_ring = malloc(vp.audio_ring_size);
        if (vp.audio_ring) {
            rpi_audio_init(vp.hdr.audio_rate);
            audio_owned_by_player = true;
            vp.audio_inited = true;
            vp.audio_on[0] = vp.audio_on[1] = true;
        } else {
            vp.audio_present = false;
        }
    }

    vp.open = true;
    vp.mode = VP_STILL;
    vp.cur_picture = 1;

    /* Show picture 1 so there is something on screen immediately */
    videoplayer_goto(1, 'R');

    Pi1MHz_Register_Poll(videoplayer_poll);

    LOG_INFO("videoplayer: %s %"PRIu32"x%"PRIu32" %"PRIu32" frames%s\r\n",
             PVF_FILENAME, vp.hdr.width, vp.hdr.height, vp.hdr.frame_count,
             vp.audio_present ? " + audio" : "");
}
