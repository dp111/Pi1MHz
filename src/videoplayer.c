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
#include "BeebSCSI/filesystem.h"
#include "rpi/rpi.h"
#include "rpi/systimer.h"
#include "rpi/audio.h"
#include "rpi/h264dec.h"
#include "Pi1MHz.h"
#include "pvf.h"
#include "videoplayer.h"

#define YUV_PLANE 0

/* The video for the current VFS jukebox directory, same convention as
   the scsi0.dat LUN images: /BeebVFS<n>/video.pvf. The card root is
   tried second so a standalone demo card (no VFS volume) still plays. */
#define PVF_FILENAME "video.pvf"
static char pvf_path[32];

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
    uint32_t speed_fast;             /* SxxxF: speed = value/2 x normal, 2..40 */
    uint32_t speed_slow;             /* SxxxS: speed = 2/value x normal, 2..250 */
    uint32_t stride;                 /* pictures per displayed frame (fast play) */
    bool play_audio;                 /* slow/fast motion runs silent */
    bool show_picture;               /* D1: draw the picture number overlay */
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

    /* audio: the PCM goes straight from the .pvf record into the audio
       core's ring; channel mutes (A/B soundtracks) are applied at the
       output by the core so they stay in sync */
    bool audio_present;
    bool audio_on[2];                /* F-codes A0/A1, B0/B1 */
    audio_producer_t producer;

    bool reopen;                     /* the VFS directory changed / card remounted */
    uint32_t dup_owed;               /* still's duplicate AU not yet sent (record+1) */
    bool tail_pushed;                /* end of play range: last picture's pusher sent */

    /* diagnostics */
    uint32_t feeds, feed_fail, frames_back, flips, resume_max_us, feed_max_us;
    uint32_t clmt_entries;           /* cluster link map size in use (0 = none) */
} vp;

/* ------------------------------------------------------------------ */
/* SD reading / decoder feeding                                       */
/* ------------------------------------------------------------------ */

/* Read record 'frame' (0-based) and submit its access unit. Audio PCM is
   pulled into the ring when 'with_audio'. 'eos' forces the decoder to
   emit the picture immediately (used for stills/steps). */
/* Read 'len' bytes of the current record into 'dst' in pieces, keeping the
   audio DMA fed between them: an 80 KB access unit after a random seek
   was measured at ~54 ms (two reads, ~1.5 MB/s on this path) against a
   9.5 ms runway, so even 16 KB pieces starved it - 4 KB keeps each gap
   under 3 ms. This is the one long read in the loop. */
static bool read_chunked(void *dst, uint32_t len)
{
    uint8_t *p = dst;
    UINT n;
    while (len) {
        uint32_t piece = len < 4096u ? len : 4096u;
        if (f_read(&vp.file, p, piece, &n) != FR_OK || n != piece)
            return false;
        p += piece;
        len -= piece;
        audio_pump();
    }
    return true;
}

/* Submit the access unit of record 'frame' (0-based) as one input buffer;
   the audio PCM goes into the ring when 'with_audio'. Returns false when
   no staging buffer is free or the read failed. */
static bool submit_au(uint32_t frame, bool with_audio, bool count)
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
    if (!read_chunked(staging, vlen)) {
        h264dec_cancel_input();
        vp.feed_fail++;
        return false;
    }

    if (with_audio && vp.audio_present && rec.audio_len) {
        /* Straight from SD into the audio ring, in one or two pieces if
           it wraps. A frame that does not fit (ring full) is skipped -
           the pacing loop never lets that happen in practice. */
        uint32_t frames = rec.audio_len / 4u;
        if (frames <= audio_free_frames()) {
            while (frames) {
                uint32_t contig;
                int16_t *dst = audio_write_ptr(&vp.producer, &contig);
                if (!dst || !contig)
                    break;
                if (contig > frames)
                    contig = frames;
                if (f_read(&vp.file, dst, contig * 4u, &n) != FR_OK || n != contig * 4u)
                    break;
                audio_commit(contig);
                frames -= contig;
            }
        }
    }

    /* pts carries the frame number so the display side knows what it is
       looking at without any other bookkeeping. Never an EOS: after one
       the component takes nothing until it is flushed, and a flush was
       found not to be enough to get it decoding again. */
    if (!h264dec_submit_input(rec.video_len, (int64_t)frame, false)) {
        vp.feed_fail++;
        return false;
    }
    if (count)
        vp.in_flight++;
    vp.feeds++;
    return true;
}

/* Feed record 'frame'. A still ('eos' - the name is historical) is made
   the way the decoder naturally works: it emits picture N when picture
   N+1 arrives, so the same access unit is submitted twice. The duplicate
   is not counted in in_flight - it stays inside the decoder until the
   next flush (a seek, play, or reverse step), which is what h264dec_
   resume() in those paths is for. If both staging buffers are not free
   the duplicate is owed and the poll task sends it. */
static bool feed_frame(uint32_t frame, bool with_audio, bool eos)
{
    uint32_t t0 = RPI_GetSystemTime();
    if (!submit_au(frame, with_audio, true))
        return false;
    if (eos) {
        if (!submit_au(frame, false, false))
            vp.dup_owed = frame + 1u;   /* 1-based so 0 = none */
    }
    uint32_t dt = RPI_GetSystemTime() - t0;
    if (dt > vp.feed_max_us) vp.feed_max_us = dt;
    return true;
}

/* Decoded frame callback - runs inside h264dec_poll(); record only. */
static void frame_decoded(uint32_t phys, int64_t pts, bool eos)
{
    if (eos && !phys)
        return;
    vp.frames_back++;
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

/* The Beeb's own 8x8 glyphs (framebuffer/fonts/bbc.fnt.h, 8 bytes per
   character, MSB = leftmost pixel) render the VP415-style on-screen
   picture number - one font in the image, not two. */
extern const uint8_t fontbbc[];

/* Draw the 5-digit picture number into a frame's Y plane, 2x scaled, at
   the top left - the frame buffers are in the VC heap (uncached), so the
   HVS sees the pixels immediately. Chroma is left alone: white text. */
static void draw_picture_number(uint32_t phys, uint32_t picture)
{
    uint8_t *y = (uint8_t *)(uintptr_t)phys;
    uint32_t w = vp.hdr.width;
    uint32_t x0 = 32, y0 = 28;

    for (int digit = 4; digit >= 0; digit--) {
        uint32_t d = picture % 10u;
        picture /= 10u;
        uint32_t dx = x0 + (uint32_t)digit * 20u;
        const uint8_t *glyph = &fontbbc[(uint32_t)('0' + d) * 8u];
        for (uint32_t row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (uint32_t col = 0; col < 8; col++) {
                uint8_t v = (bits & (0x80u >> col)) ? 0xEB : 0x10;
                uint8_t *p = y + (y0 + row * 2u) * w + dx + col * 2u;
                p[0] = p[1] = v;
                p[w] = p[w + 1] = v;
            }
        }
    }
}

/* Put the pending decoded frame on screen and recycle the old one. */
static void flip_pending(void)
{
    uint32_t phys = vp.pending_phys;
    if (!phys)
        return;
    vp.pending_phys = 0;

    if (vp.show_picture)
        draw_picture_number(phys, (uint32_t)vp.pending_pts + 1u);

    uint32_t w = vp.hdr.width, h = vp.hdr.height;
    screen_set_YUV_pointers(YUV_PLANE,
                            phys,
                            phys + w * h,               /* Cb (U) */
                            phys + w * h + (w / 2) * (h / 2)); /* Cr (V) */

    if (vp.displayed_phys && vp.displayed_phys != phys)
        h264dec_recycle_output(vp.displayed_phys);
    vp.displayed_phys = phys;
    vp.flips++;
    vp.cur_picture = (uint32_t)vp.pending_pts + 1u;
}

/* ------------------------------------------------------------------ */
/* Poll task                                                          */
/* ------------------------------------------------------------------ */

static void pvf_reopen(void);
static bool pvf_open_file(void);
static void reset_speed(void);

static void videoplayer_poll(void)
{
    if (vp.reopen) {
        vp.reopen = false;
        pvf_reopen();
    }
    if (!vp.open)
        return;

    h264dec_poll();

    if (vp.dup_owed) {
        if (submit_au(vp.dup_owed - 1u, false, false))
            vp.dup_owed = 0;
    }

    /* Feed the PWM before any SD work: the DMA runway is short and a
       seek-burst of AU reads below can exceed it (the core also pumps
       from its own poll; this is the early call) */
    audio_pump();

    /* Pending random access? Flush whatever is mid-pipeline first. */
    if (vp.seek_frame >= 0) {
        uint32_t target = (uint32_t)vp.seek_frame;
        uint32_t t0 = RPI_GetSystemTime();
        h264dec_resume();            /* discard stale pictures and a held still duplicate */
        uint32_t dt = RPI_GetSystemTime() - t0;
        if (dt > vp.resume_max_us) vp.resume_max_us = dt;
        vp.in_flight = 0;
        vp.dup_owed = 0;
        vp.tail_pushed = false;
        audio_flush();

        bool play = (vp.seek_op == 'N') ||
                    (vp.seek_op == 'Q' && vp.prev_mode == VP_PLAY);
        if (play)
            reset_speed();           /* goto-and-play is normal speed with sound */
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
            if (!feed_frame(vp.next_frame, vp.play_audio, false))
                break;
            vp.next_frame += vp.stride ? vp.stride : 1u;
        }
        /* The decoder emits a picture only when the next one arrives, so
           the last picture of the range needs a pusher: submit it again
           (uncounted, like a still's duplicate) once everything else is
           in. The flush below discards the duplicate. */
        if (vp.next_frame >= limit && vp.in_flight == 1 && !vp.tail_pushed) {
            if (submit_au(limit - 1u, false, false))
                vp.tail_pushed = true;
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
        /* With a stride the last displayed picture can fall short of the
           limit by up to stride-1 - the range still ended */
        if (vp.cur_picture + (vp.stride ? vp.stride : 1u) > limit &&
            !vp.in_flight && !vp.pending_phys) {
            vp.mode = VP_STILL;
            vp.tail_pushed = false;
            audio_flush();
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
            {
                uint32_t back = 1u + (vp.stride ? vp.stride : 1u);
                /* record cur_picture-back is valid exactly when
                   cur_picture >= back (records are 0-based, pictures
                   1-based): '>' stopped reverse play on picture 2 */
                if (vp.cur_picture >= back) {
                    if (feed_frame(vp.cur_picture - back, false, true))
                        vp.next_frame_due += vp.frame_period_us;
                } else {
                    vp.mode = VP_STILL;
                }
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

void videoplayer_media_changed(void)
{
    /* Called from the jukebox/remount paths, possibly in FIQ context: just
       note it; the poll task does the file work. */
    vp.reopen = true;
}

/* The VFS jukebox directory changed, or the card was remounted (which
   invalidates every open FIL): drop the file and index, reopen whatever
   video the new directory holds, and show its picture 1. The decoder and
   the frame buffers stay up. */
static void pvf_reopen(void)
{
    if (!h264dec_running())
        return;                      /* never came up: nothing to reopen into */

    if (vp.open) {
        h264dec_resume();            /* discard anything in flight */
        vp.in_flight = 0;
        audio_flush();
        f_close(&vp.file);
        free(vp.index);
        vp.index = NULL;
        vp.open = false;
    }
    vp.mode = VP_STILL;
    vp.seek_frame = -1;
    vp.pending_phys = 0;
    vp.stop_picture = 0;

    uint32_t old_w = vp.hdr.width, old_h = vp.hdr.height;
    if (!pvf_open_file()) {
        /* nothing in the new directory: hold the last picture */
        return;
    }
    if (vp.hdr.width != old_w || vp.hdr.height != old_h) {
        /* The decoder and the GPU frame buffers were sized at boot;
           decoding a different geometry into them corrupts GPU memory.
           A dimension change needs a Beeb reset (full re-init). */
        LOG_INFO("videoplayer: %s is %"PRIu32"x%"PRIu32", player is %"PRIu32"x%"PRIu32" - reset to switch\r\n",
                 pvf_path, vp.hdr.width, vp.hdr.height, old_w, old_h);
        f_close(&vp.file);
        free(vp.index);
        vp.index = NULL;
        vp.hdr.width = old_w;
        vp.hdr.height = old_h;
        if (vp.audio_present)
            audio_release(&vp.producer);   /* the synths get the sound back */
        vp.audio_present = false;
        return;
    }
    vp.frame_period_us = (uint32_t)((uint64_t)1000000 * vp.hdr.fps_den / vp.hdr.fps_num);
    vp.audio_present = (vp.hdr.audio_rate != 0);
    if (vp.audio_present) {
        vp.producer.rate = vp.hdr.audio_rate;
        vp.producer.latency_frames = (vp.hdr.audio_bytes_per_frame / 4u) * 8u;
        vp.producer.name = "video";
        vp.producer.dma_frames = AUDIO_DMA_FRAMES_MAX;
        audio_claim(&vp.producer);
        vp.audio_on[0] = vp.audio_on[1] = true;
    }
    vp.open = true;
    vp.cur_picture = 1;
    videoplayer_goto(1, 'R');
}

uint32_t videoplayer_picture_number(void)
{
    return vp.cur_picture;
}

const char *videoplayer_status(void)
{
    static char buf[160];
    snprintf(buf, sizeof buf, "%s mode %d pic %lu seek %ld inflight %ld feeds %lu fail %lu back %lu flips %lu resume_max %luus feed_max %luus clmt %lu%s",
             vp.open ? pvf_path : "closed", (int)vp.mode, (unsigned long)vp.cur_picture,
             (long)vp.seek_frame, (long)vp.in_flight, (unsigned long)vp.feeds,
             (unsigned long)vp.feed_fail, (unsigned long)vp.frames_back,
             (unsigned long)vp.flips, (unsigned long)vp.resume_max_us,
             (unsigned long)vp.feed_max_us, (unsigned long)vp.clmt_entries,
             vp.file.cltbl ? "" : " (slow seeks)");
    return buf;
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

/* The .pvf's own frame period; only meaningful with a file open */
static uint32_t nominal_period_us(void)
{
    if (!vp.hdr.fps_num)
        return 40000;                /* no file: any sane value */
    return (uint32_t)((uint64_t)1000000 * vp.hdr.fps_den / vp.hdr.fps_num);
}

/* Normal-speed transport: back to nominal pacing with sound */
static void reset_speed(void)
{
    vp.frame_period_us = nominal_period_us();
    vp.stride = 1;
    vp.play_audio = true;
}

void videoplayer_play_fwd(void)
{
    if (!vp.open)
        return;
    reset_speed();
    if (vp.mode != VP_PLAY) {
        vp.next_frame = vp.cur_picture;      /* picture is 1-based */
        vp.in_flight = 0;
        vp.dup_owed = 0;
        vp.tail_pushed = false;
        h264dec_resume();
        audio_flush();
        vp.mode = VP_PLAY;
        vp.next_frame_due = RPI_GetSystemTime() + vp.frame_period_us;
    }
}

void videoplayer_play_rev(void)
{
    if (!vp.open)
        return;
    reset_speed();
    h264dec_resume();
    vp.in_flight = 0;
    vp.dup_owed = 0;
    audio_flush();
    vp.mode = VP_PLAY_REV;
    vp.next_frame_due = RPI_GetSystemTime() + vp.frame_period_us;
}

void videoplayer_halt(void)
{
    if (!vp.open)
        return;
    vp.mode = VP_STILL;
    audio_flush();                   /* sound stops with the picture */
}

void videoplayer_pause(void)
{
    if (!vp.open)
        return;
    vp.mode = VP_STILL;
    audio_flush();
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

void videoplayer_speed(uint32_t value, bool fast)
{
    if (value < 2u)
        value = 2u;
    if (fast) {
        if (value > 40u)
            value = 40u;
        vp.speed_fast = value;
    } else {
        if (value > 250u)
            value = 250u;
        vp.speed_slow = value;
    }
}

/* Slow motion: every picture held for 'speed' fields, silent */
static void start_motion(vp_mode_t mode, uint32_t period_us, uint32_t stride)
{
    if (!vp.open)
        return;
    if (!period_us)
        period_us = nominal_period_us();
    h264dec_resume();
    vp.in_flight = 0;
    vp.dup_owed = 0;
    vp.tail_pushed = false;
    audio_flush();
    vp.frame_period_us = period_us;
    vp.stride = stride;
    vp.play_audio = false;
    vp.next_frame = vp.cur_picture;  /* picture is 1-based -> next record */
    vp.mode = mode;
    vp.next_frame_due = RPI_GetSystemTime() + period_us;
}

/* Slow: 2/xxx x normal -> each picture held xxx/2 frame times, i.e. a
   period of 20000*xxx us (xxx=2 is the nominal 40 ms). */
void videoplayer_slow_fwd(void)
{
    start_motion(VP_PLAY, 20000u * (vp.speed_slow ? vp.speed_slow : 6u), 1u);
}

void videoplayer_slow_rev(void)
{
    start_motion(VP_PLAY_REV, 20000u * (vp.speed_slow ? vp.speed_slow : 6u), 1u);
}

/* Fast: xxx/2 x normal. The SD card sustains barely more than 25 AUs/s,
   so speed comes from skipping pictures (every one is an IDR): stride =
   xxx/2 at the nominal frame rate. Fractional factors round down, so
   S3F (1.5x) plays at normal speed - a documented approximation. */
static void fast_motion(vp_mode_t mode)
{
    uint32_t stride = (vp.speed_fast ? vp.speed_fast : 6u) / 2u;
    if (stride < 1u)
        stride = 1u;
    start_motion(mode, 0, stride);   /* 0 = nominal period */
}

void videoplayer_fast_fwd(void)  { fast_motion(VP_PLAY); }
void videoplayer_fast_rev(void)  { fast_motion(VP_PLAY_REV); }

void videoplayer_clear(void)
{
    vp.stop_picture = 0;
    vp.info_picture = 0;
}

void videoplayer_show_picture_number(bool on)
{
    if (vp.show_picture == on)
        return;
    vp.show_picture = on;
    /* On a still the digits are already burnt into the displayed buffer
       (or missing from it): re-decode the current picture to repaint. */
    if (vp.open && vp.mode == VP_STILL && vp.cur_picture)
        videoplayer_goto(vp.cur_picture, 'R');
}

void videoplayer_audio_enable(int channel, bool on)
{
    /* audio_set_channel_mute is a core-wide control: unless THIS player
       claimed the audio (open, with a sound track), the core belongs to
       a synth and an A0 sent at a bare VFS LUN must not silence it. */
    if (!vp.open || !vp.audio_present)
        return;
    if (channel >= 0 && channel < 2)
        vp.audio_on[channel] = on;
    audio_set_channel_mute(!vp.audio_on[0], !vp.audio_on[1]);
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

static bool pvf_open_file(void)
{
    UINT n;
    snprintf(pvf_path, sizeof(pvf_path), "/BeebVFS%d/" PVF_FILENAME,
             filesystemGetLunDirectoryVFS());
    if (f_open(&vp.file, pvf_path, FA_READ) != FR_OK) {
        snprintf(pvf_path, sizeof(pvf_path), "/" PVF_FILENAME);
        if (f_open(&vp.file, pvf_path, FA_READ) != FR_OK)
            return false;
    }

    /* Random access seeks constantly: a fast-seek link map makes f_lseek
       O(1) instead of a FAT-chain walk (50-80 ms per *FRAME on a 2.9 GB
       side, enough to starve the audio DMA). Sized by the filesystem
       helper to the file's real fragmentation. */
    static DWORD *clmt;
    static uint32_t clmt_entries;
    if (!filesystemAttachLinkMap(&vp.file, &clmt, &clmt_entries))
        LOG_INFO("videoplayer: %s: no cluster map; seeks will be slow\r\n", pvf_path);
    vp.clmt_entries = clmt_entries;

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
        audio_release(&vp.producer);
    }
    free(vp.index);

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
    if (vp.audio_present) {
        /* Take the audio output. Supersedes the Music 5000 (which cannot
           run alongside VFS anyway) or BeebSID; they get it back on the
           next Beeb reset if the video is gone. ~8 video frames of lead
           rides out SD seek bursts. */
        vp.producer.rate = vp.hdr.audio_rate;
        vp.producer.latency_frames = (vp.hdr.audio_bytes_per_frame / 4u) * 8u;
        vp.producer.name = "video";
        vp.producer.dma_frames = AUDIO_DMA_FRAMES_MAX;   /* ride out SD latency spikes */
        audio_claim(&vp.producer);
        vp.audio_on[0] = vp.audio_on[1] = true;
    }

    vp.open = true;
    vp.mode = VP_STILL;
    vp.cur_picture = 1;
    vp.speed_fast = 6;               /* 3x, the VP415 default */
    vp.speed_slow = 6;               /* 1/3x, the VP415 default */
    vp.stride = 1;
    vp.play_audio = true;

    /* Show picture 1 so there is something on screen immediately */
    videoplayer_goto(1, 'R');

    Pi1MHz_Register_Poll(videoplayer_poll);

    LOG_INFO("videoplayer: %s %"PRIu32"x%"PRIu32" %"PRIu32" frames%s\r\n",
             pvf_path, vp.hdr.width, vp.hdr.height, vp.hdr.frame_count,
             vp.audio_present ? " + audio" : "");
}
