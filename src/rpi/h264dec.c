/*
    h264dec.c - hardware H264 decoder via MMAL "ril.video_decode"

    Component lifecycle (mirrors what hello_video/omxplayer do over OMX):

      1. create + enable ril.video_decode
      2. input port: format = video/H264 768x576, enable, start feeding
         access units
      3. the component parses the SPS and raises MMAL_EVENT_FORMAT_CHANGED
         on the output port; we then set the output format to I420 at the
         advertised size, enable the port and hand it our display buffers
      4. every filled output buffer arrives back via BUFFER_TO_HOST +
         bulk; we pass its address up and re-arm the buffer when the
         caller recycles it

    The reconfigure in (3) must not run inside a VCHIQ callback (it makes
    synchronous MMAL calls of its own), so the event only sets a flag and
    h264dec_poll() does the work.
*/

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "rpi.h"
#include "systimer.h"
#include "vchiq.h"
#include "mmal_vc.h"
#include "h264dec.h"

typedef struct {
    mmal_vc_buffer_t buf;
    uint8_t *arm_ptr;                /* ARM view (input buffers only) */
    bool free;
} input_slot_t;

typedef struct {
    mmal_vc_buffer_t buf;
    bool registered;                 /* known to us */
    bool with_component;             /* currently submitted to the VC */
    bool with_caller;                /* frame delivered, awaiting recycle */
} output_slot_t;

static struct {
    bool running;                    /* component up, input port enabled */
    bool output_enabled;
    bool reconfigure_pending;        /* FORMAT_CHANGED seen, work deferred */
    bool eos_pending;                /* EOS marker owed but no slot was free */
    bool sent_input;                 /* input port has carried real payload */
    uint32_t width, height;
    uint32_t frame_bytes;

    uint32_t component;
    mmal_vc_port_t port_in;
    mmal_vc_port_t port_out;

    input_slot_t in[H264DEC_INPUT_BUFFERS];
    int in_borrowed;                 /* slot handed out by get_input_buffer */
    output_slot_t out[H264DEC_MAX_OUTPUT];

    h264dec_frame_cb frame_cb;
    uint32_t frames;
} dec;

/* ------------------------------------------------------------------ */
/* MMAL callbacks (called from inside mmal_vc_poll)                   */
/* ------------------------------------------------------------------ */

static void on_buffer_done(mmal_vc_buffer_t *buf)
{
    /* Input buffer released by the component? */
    for (int i = 0; i < H264DEC_INPUT_BUFFERS; i++) {
        if (buf == &dec.in[i].buf) {
            dec.in[i].free = true;
            return;
        }
    }

    /* Otherwise it is an output buffer */
    for (int i = 0; i < H264DEC_MAX_OUTPUT; i++) {
        output_slot_t *o = &dec.out[i];
        if (buf != &o->buf)
            continue;
        o->with_component = false;

        bool eos = (buf->flags & MMAL_BUFFER_HEADER_FLAG_EOS) != 0;
        if (buf->length >= dec.frame_bytes) {
            /* A real frame: it stays with the caller (likely on screen)
               until h264dec_recycle_output() hands it back */
            dec.frames++;
            o->with_caller = true;
            if (dec.frame_cb)
                dec.frame_cb(buf->busaddr & 0x3FFFFFFFu, buf->pts, eos);
        } else if (eos && buf->length == 0) {
            /* Bare EOS marker - nothing to display (phys 0 tells the
               caller so), and the poll loop re-arms the buffer */
            if (dec.frame_cb)
                dec.frame_cb(0, buf->pts, true);
        } else if (buf->length) {
            LOG_INFO("h264: short frame %"PRIu32"\r\n", buf->length);
        }
        return;
    }
}

static void on_event(uint32_t port_type, uint32_t port_num, uint32_t cmd,
                     const uint8_t *data, uint32_t length)
{
    (void)data;
    (void)length;
    (void)port_num;

    if (cmd == MMAL_EVENT_FORMAT_CHANGED && port_type == MMAL_PORT_TYPE_OUTPUT) {
        LOG_DEBUG("h264: output format changed\r\n");
        dec.reconfigure_pending = true;
    } else if (cmd == MMAL_EVENT_ERROR) {
        uint32_t status = length >= 4 ? *(const uint32_t *)(const void *)data : 0;
        LOG_INFO("h264: component error %"PRIu32"\r\n", status);
    }
}

/* ------------------------------------------------------------------ */
/* Output port management                                             */
/* ------------------------------------------------------------------ */

static void arm_output_buffers(void)
{
    if (!dec.output_enabled)
        return;
    for (int i = 0; i < H264DEC_MAX_OUTPUT; i++) {
        output_slot_t *o = &dec.out[i];
        if (!o->registered || o->with_component || o->with_caller ||
            o->buf.in_flight)
            continue;
        o->buf.length = 0;
        o->buf.offset = 0;
        o->buf.flags = 0;
        o->buf.pts = MMAL_TIME_UNKNOWN;
        if (mmal_vc_submit_buffer(&dec.port_out, &o->buf))
            o->with_component = true;
        else
            break;                   /* transient; retry next poll */
    }
}

/* FORMAT_CHANGED arrived: (re)program the output port for I420 and
   enable it. Runs from h264dec_poll(), never from a VCHIQ callback. */
static bool reconfigure_output(void)
{
    if (dec.output_enabled) {
        /* Resolution changes cannot happen with our fixed-format streams;
           be safe and disable first if the component insists. */
        mmal_vc_port_disable(&dec.port_out);
        dec.output_enabled = false;
        for (int i = 0; i < H264DEC_MAX_OUTPUT; i++)
            dec.out[i].with_component = false;
    }

    if (!mmal_vc_port_info_get(dec.component, MMAL_PORT_TYPE_OUTPUT, 0,
                               &dec.port_out))
        return false;

    dec.port_out.format.type = MMAL_ES_TYPE_VIDEO;
    dec.port_out.format.encoding = MMAL_ENCODING_I420;
    dec.port_out.format.encoding_variant = 0;
    dec.port_out.es.video.width = dec.width;
    dec.port_out.es.video.height = dec.height;
    dec.port_out.es.video.crop_x = 0;
    dec.port_out.es.video.crop_y = 0;
    dec.port_out.es.video.crop_width = (int32_t)dec.width;
    dec.port_out.es.video.crop_height = (int32_t)dec.height;
    dec.port_out.format.extradata_size = 0;
    dec.port_out.port.buffer_num = H264DEC_MAX_OUTPUT;
    dec.port_out.port.buffer_size = dec.frame_bytes;

    if (!mmal_vc_port_set_format(&dec.port_out)) {
        LOG_INFO("h264: output set_format failed\r\n");
        return false;
    }

    /* The component may bump its minima; honour them but keep our size */
    if (dec.port_out.port.buffer_size < dec.frame_bytes)
        dec.port_out.port.buffer_size = dec.frame_bytes;
    dec.port_out.port.buffer_num = H264DEC_MAX_OUTPUT;

    if (!mmal_vc_port_enable(&dec.port_out)) {
        LOG_INFO("h264: output enable failed\r\n");
        return false;
    }

    dec.output_enabled = true;
    LOG_DEBUG("h264: output enabled %"PRIu32"x%"PRIu32"\r\n",
              dec.port_out.es.video.width, dec.port_out.es.video.height);
    arm_output_buffers();
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool h264dec_init(uint32_t width, uint32_t height, h264dec_frame_cb cb)
{
    if (dec.running)
        return true;

    /* I420 needs mod-32 width / mod-16 height or the VC pads the planes
       and the HVS pointer arithmetic in the player goes wrong. 768x576
       fits exactly. */
    if ((width & 31u) || (height & 15u)) {
        LOG_INFO("h264: dimensions must be mod32 x mod16\r\n");
        return false;
    }

    memset(&dec, 0, sizeof(dec));
    dec.width = width;
    dec.height = height;
    dec.frame_bytes = width * height * 3u / 2u;
    dec.frame_cb = cb;
    dec.in_borrowed = -1;

    static const mmal_vc_client_callbacks_t cbs = {
        .buffer_done = on_buffer_done,
        .event = on_event,
    };
    if (!mmal_vc_init(&cbs))
        return false;

    uint32_t inputs, outputs;
    if (!mmal_vc_component_create("ril.video_decode", &dec.component,
                                  &inputs, &outputs))
        return false;
    LOG_DEBUG("h264: video_decode created (%"PRIu32" in, %"PRIu32" out)\r\n",
              inputs, outputs);

    /* Input staging buffers, uncached VC heap */
    for (int i = 0; i < H264DEC_INPUT_BUFFERS; i++) {
        uint32_t handle;
        uint32_t phys = vchiq_alloc_shared(H264DEC_INPUT_BUF_SIZE, &handle);
        if (!phys) {
            LOG_INFO("h264: no memory for input buffers\r\n");
            return false;
        }
        dec.in[i].arm_ptr = (uint8_t *)(uintptr_t)phys;
        dec.in[i].buf.busaddr = vchiq_bus_addr(phys);
        dec.in[i].buf.alloc_size = H264DEC_INPUT_BUF_SIZE;
        dec.in[i].free = true;
    }

    /* Input port: H264 at our fixed size */
    if (!mmal_vc_port_info_get(dec.component, MMAL_PORT_TYPE_INPUT, 0,
                               &dec.port_in))
        return false;

    dec.port_in.format.type = MMAL_ES_TYPE_VIDEO;
    dec.port_in.format.encoding = MMAL_ENCODING_H264;
    dec.port_in.format.encoding_variant = 0;
    dec.port_in.es.video.width = width;
    dec.port_in.es.video.height = height;
    dec.port_in.es.video.frame_rate.num = 25;
    dec.port_in.es.video.frame_rate.den = 1;
    dec.port_in.es.video.par.num = 1;
    dec.port_in.es.video.par.den = 1;
    dec.port_in.format.extradata_size = 0;   /* SPS/PPS are in-band */
    dec.port_in.port.buffer_num = H264DEC_INPUT_BUFFERS;
    dec.port_in.port.buffer_size = H264DEC_INPUT_BUF_SIZE;

    if (!mmal_vc_port_set_format(&dec.port_in)) {
        LOG_INFO("h264: input set_format failed\r\n");
        return false;
    }
    dec.port_in.port.buffer_num = H264DEC_INPUT_BUFFERS;
    dec.port_in.port.buffer_size = H264DEC_INPUT_BUF_SIZE;

    if (!mmal_vc_component_enable(dec.component)) {
        LOG_INFO("h264: component enable failed\r\n");
        return false;
    }

    if (!mmal_vc_port_enable(&dec.port_in)) {
        LOG_INFO("h264: input enable failed\r\n");
        return false;
    }

    dec.running = true;
    LOG_INFO("h264: hardware decoder ready (%"PRIu32"x%"PRIu32")\r\n",
             width, height);
    return true;
}

bool h264dec_add_output_buffer(uint32_t phys, uint32_t size)
{
    if (size < dec.frame_bytes)
        return false;
    for (int i = 0; i < H264DEC_MAX_OUTPUT; i++) {
        output_slot_t *o = &dec.out[i];
        if (!o->registered) {
            o->buf.busaddr = vchiq_bus_addr(phys);
            o->buf.alloc_size = dec.frame_bytes;
            o->registered = true;
            arm_output_buffers();
            return true;
        }
    }
    return false;
}

void h264dec_recycle_output(uint32_t phys)
{
    for (int i = 0; i < H264DEC_MAX_OUTPUT; i++) {
        output_slot_t *o = &dec.out[i];
        if (o->registered && (o->buf.busaddr & 0x3FFFFFFFu) == phys) {
            /* Only a buffer the caller actually holds may be recycled -
               anything else (e.g. a stale phys after a flush already
               returned the buffer) must not clobber with_component /
               in_flight while the VC still owns it. */
            if (o->with_caller) {
                o->with_caller = false;
                arm_output_buffers();
            }
            return;
        }
    }
}

uint8_t *h264dec_get_input_buffer(uint32_t *max_size)
{
    if (!dec.running || dec.in_borrowed >= 0)
        return NULL;
    for (int i = 0; i < H264DEC_INPUT_BUFFERS; i++) {
        if (dec.in[i].free) {
            dec.in_borrowed = i;
            if (max_size)
                *max_size = H264DEC_INPUT_BUF_SIZE;
            return dec.in[i].arm_ptr;
        }
    }
    return NULL;
}

void h264dec_cancel_input(void)
{
    dec.in_borrowed = -1;
}

/* Send the zero-length EOS marker that pushes the last picture out of the
   decoder (the freeze-frame trick). Needs a free input slot AND bulk
   space; when either is missing the debt is remembered in eos_pending
   and retried from h264dec_poll() as slots come back. */
static void try_send_eos(void)
{
    for (int i = 0; i < H264DEC_INPUT_BUFFERS; i++) {
        if (dec.in[i].free && i != dec.in_borrowed) {
            input_slot_t *e = &dec.in[i];
            e->buf.length = 0;
            e->buf.offset = 0;
            e->buf.pts = MMAL_TIME_UNKNOWN;
            e->buf.flags = MMAL_BUFFER_HEADER_FLAG_EOS;
            if (mmal_vc_submit_buffer(&dec.port_in, &e->buf)) {
                e->free = false;
                dec.eos_pending = false;
            }
            return;                  /* on failure eos_pending stays set */
        }
    }
}

bool h264dec_submit_input(uint32_t length, int64_t pts, bool eos)
{
    if (!dec.running || dec.in_borrowed < 0 || length > H264DEC_INPUT_BUF_SIZE)
        return false;

    input_slot_t *s = &dec.in[dec.in_borrowed];
    s->buf.length = length;
    s->buf.offset = 0;
    s->buf.pts = pts;
    s->buf.flags = MMAL_BUFFER_HEADER_FLAG_FRAME_END |
                   MMAL_BUFFER_HEADER_FLAG_KEYFRAME;

    if (!mmal_vc_submit_buffer(&dec.port_in, &s->buf)) {
        /* Transient (no VCHIQ TX slot / bulk slot). Drop the borrowed
           buffer so the caller can retry with a fresh read - leaving it
           borrowed would starve input forever. */
        h264dec_cancel_input();
        return false;
    }
    s->free = false;
    dec.in_borrowed = -1;
    dec.sent_input = true;

    if (eos) {
        dec.eos_pending = true;
        try_send_eos();              /* poll retries if nothing was free */
    }
    return true;
}

bool h264dec_resume(void)
{
    if (!dec.running)
        return false;
    dec.eos_pending = false;         /* a flush supersedes any owed EOS */
    /* Input: the bulk-synced flush, but only once real payload has been
       sent on the port (before that the VC side may not be set up to
       receive the dummy bulk - same gating as the reference client).
       Output: always the plain flush; that port never carries host->VC
       payload, so the dummy-bulk variant would desynchronise pairing. */
    bool ok = dec.sent_input ? mmal_vc_port_flush(&dec.port_in)
                             : mmal_vc_port_flush_normal(&dec.port_in);
    if (dec.output_enabled)
        ok = mmal_vc_port_flush_normal(&dec.port_out) && ok;
    /* Flushed output buffers come back with length 0; on_buffer_done has
       already cleared with_component, so just re-arm them. */
    arm_output_buffers();
    return ok;
}

/* Detach everything for a warm restart (Beeb reset re-runs the emulator
   inits): return all buffers to us, forget the output registrations so
   the caller can free/reallocate its frame buffers, and leave the
   component enabled with a fresh input port ready for the next AU. */
void h264dec_reset(void)
{
    if (!dec.running)
        return;

    bool was_enabled = dec.output_enabled;
    mmal_vc_port_disable(&dec.port_in);
    if (dec.output_enabled) {
        mmal_vc_port_disable(&dec.port_out);
        dec.output_enabled = false;
    }

    for (int i = 0; i < H264DEC_INPUT_BUFFERS; i++)
        dec.in[i].free = true;
    dec.in_borrowed = -1;
    dec.eos_pending = false;
    for (int i = 0; i < H264DEC_MAX_OUTPUT; i++) {
        dec.out[i].registered = false;
        dec.out[i].with_component = false;
        dec.out[i].with_caller = false;
        dec.out[i].buf.in_flight = false;
    }

    mmal_vc_port_enable(&dec.port_in);
    /* If the output was up before, its format is already known - re-enable
       it from the poll loop once the caller has registered new buffers,
       without waiting for another FORMAT_CHANGED. */
    dec.reconfigure_pending = was_enabled;
}

void h264dec_poll(void)
{
    if (!dec.running)
        return;

    mmal_vc_poll();

    if (dec.reconfigure_pending) {
        dec.reconfigure_pending = false;
        if (!reconfigure_output())
            dec.reconfigure_pending = true;   /* retry next poll */
    }

    if (dec.eos_pending)
        try_send_eos();

    arm_output_buffers();
}

bool h264dec_running(void)
{
    return dec.running;
}

uint32_t h264dec_frames_decoded(void)
{
    return dec.frames;
}
