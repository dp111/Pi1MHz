/* auds.c - firmware audio service ("AUDS") client. See auds.h. */

#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "rpi.h"
#include "systimer.h"
#include "vchiq.h"
#include "auds.h"

#define AUDS_FOURCC        VCHIQ_FOURCC('A','U','D','S')
#define AUDS_VER           2
#define AUDS_MIN_VER       1

#define REPLY_TIMEOUT_US   500000u

/* The two cookies every WRITE carries and every COMPLETE echoes */
#define AUDS_COOKIE1       VCHIQ_FOURCC('B','C','M','A')
#define AUDS_COOKIE2       VCHIQ_FOURCC('D','A','T','A')

enum {
    VC_AUDIO_MSG_TYPE_RESULT = 0,
    VC_AUDIO_MSG_TYPE_COMPLETE,
    VC_AUDIO_MSG_TYPE_CONFIG,
    VC_AUDIO_MSG_TYPE_CONTROL,
    VC_AUDIO_MSG_TYPE_OPEN,
    VC_AUDIO_MSG_TYPE_CLOSE,
    VC_AUDIO_MSG_TYPE_START,
    VC_AUDIO_MSG_TYPE_STOP,
    VC_AUDIO_MSG_TYPE_WRITE,
};

typedef struct {
    int32_t type;
    union {
        struct { uint32_t channels, samplerate, bps; }        config;
        struct { uint32_t volume, dest; }                      control;
        struct { uint32_t dummy; }                             open;
        struct { uint32_t dummy; }                             close;
        struct { uint32_t dummy; }                             start;
        struct { uint32_t draining; }                          stop;
        struct { uint32_t count, cookie1, cookie2;
                 int16_t silence, max_packet; }                write;
        struct { int32_t success; }                            result;
        struct { int32_t count; uint32_t cookie1, cookie2; }   complete;
    } u;
} vc_audio_msg_t;

/* PCM staging: the VideoCore DMAs from these, so they live in the VC heap
   (uncached on the ARM). One chunk per in-flight bulk; a chunk is free
   again when its BULK_TX_DONE arrives. */
#define AUDS_CHUNK_FRAMES  AUDS_CHUNK_FRAMES_MAX   /* ~10.7 ms at 48 kHz */
#define AUDS_CHUNK_BYTES   (AUDS_CHUNK_FRAMES * 4u)
#define AUDS_CHUNKS        VCHIQ_BULK_DEPTH

static struct {
    bool     open;                   /* service opened */
    bool     running;                /* START sent */
    int      service;
    uint32_t rate, channels;
    int      dest;

    uint32_t buf_phys;               /* AUDS_CHUNKS * AUDS_CHUNK_BYTES */
    uint32_t buf_handle;
    bool     busy[AUDS_CHUNKS];
    uint32_t next_chunk;

    bool     result_ready;
    int32_t  result;

    uint32_t chunks_sent;
    uint32_t completes;
    uint32_t bytes_sent;
    uint32_t bytes_completed;
} au;

/* The firmware accepts bulk as fast as we send it and queues internally,
   so WE pace: never more than this many bytes written-but-not-completed.
   COMPLETE arrives as the stream is consumed; this bound is therefore the
   output latency. */
#define AUDS_MAX_OUTSTANDING_BYTES 4096u          /* ~21 ms at 48 kHz */

static void on_data(const void *data, unsigned int size)
{
    if (size < sizeof(int32_t))
        return;
    const vc_audio_msg_t *m = (const vc_audio_msg_t *)data;
    switch (m->type) {
    case VC_AUDIO_MSG_TYPE_RESULT:
        au.result = (size >= 8) ? m->u.result.success : -1;
        au.result_ready = true;
        break;
    case VC_AUDIO_MSG_TYPE_COMPLETE:
        /* count carries flag bits above bit 29 (the reference driver
           masks them off); the low 30 bits are bytes consumed */
        if (size >= 16 && m->u.complete.cookie1 == AUDS_COOKIE1 &&
            m->u.complete.cookie2 == AUDS_COOKIE2) {
            au.completes++;
            au.bytes_completed += (uint32_t)m->u.complete.count & 0x3FFFFFFFu;
        }
        break;
    default:
        break;
    }
}

static void on_bulk_tx_done(void *user, int actual)
{
    (void)actual;
    uint32_t idx = (uint32_t)(uintptr_t)user;
    if (idx < AUDS_CHUNKS)
        au.busy[idx] = false;
}

static bool send(vc_audio_msg_t *m, bool wait)
{
    au.result_ready = false;
    uint32_t start = RPI_GetSystemTime();
    while (!vchiq_queue_message(au.service, m, sizeof(*m))) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US)
            return false;
    }
    if (!wait)
        return true;
    while (!au.result_ready) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US) {
            LOG_INFO("auds: no reply to msg %"PRId32"\r\n", m->type);
            return false;
        }
    }
    return au.result == 0;
}

static bool send_simple(int32_t type, bool wait)
{
    vc_audio_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    return send(&m, wait);
}

bool auds_start(uint32_t rate, uint32_t channels, int dest)
{
    if (au.running && au.rate == rate && au.channels == channels && au.dest == dest)
        return true;
    if (au.running)
        auds_stop();

    if (!au.open) {
        if (!vchiq_init())
            return false;
        static const vchiq_callbacks_t cbs = {
            .on_data = on_data,
            .on_bulk_tx_done = on_bulk_tx_done,
        };
        au.service = vchiq_open_service(AUDS_FOURCC, AUDS_VER, AUDS_MIN_VER, &cbs);
        if (au.service < 0) {
            LOG_INFO("auds: no AUDS service\r\n");
            return false;
        }
        if (!au.buf_phys) {
            au.buf_phys = vchiq_alloc_shared(AUDS_CHUNKS * AUDS_CHUNK_BYTES, &au.buf_handle);
            if (!au.buf_phys)
                return false;
        }
        au.open = true;
        /* OPEN gets no reply from the firmware */
        send_simple(VC_AUDIO_MSG_TYPE_OPEN, false);
    }

    vc_audio_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = VC_AUDIO_MSG_TYPE_CONFIG;
    m.u.config.channels   = channels;
    m.u.config.samplerate = rate;
    m.u.config.bps        = 16;
    if (!send(&m, true)) {
        LOG_INFO("auds: CONFIG %"PRIu32" Hz refused\r\n", rate);
        return false;
    }

    memset(&m, 0, sizeof(m));
    m.type = VC_AUDIO_MSG_TYPE_CONTROL;
    m.u.control.volume = 0;          /* 0 dB: units are -dB * 256 */
    m.u.control.dest   = (uint32_t)dest;
    if (!send(&m, true)) {
        LOG_INFO("auds: CONTROL dest %d refused\r\n", dest);
        return false;
    }

    send_simple(VC_AUDIO_MSG_TYPE_START, false);

    memset(au.busy, 0, sizeof(au.busy));
    au.next_chunk = 0;
    au.bytes_sent = au.bytes_completed = 0;
    au.rate = rate;
    au.channels = channels;
    au.dest = dest;
    au.running = true;
    LOG_INFO("auds: %"PRIu32" Hz x%"PRIu32" -> dest %d\r\n", rate, channels, dest);
    return true;
}

void auds_stop(void)
{
    if (!au.running)
        return;
    vc_audio_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = VC_AUDIO_MSG_TYPE_STOP;
    m.u.stop.draining = 0;
    send(&m, false);
    au.running = false;
}

bool auds_running(void)
{
    return au.running;
}

uint32_t auds_free_frames(void)
{
    if (!au.running)
        return 0;
    uint32_t outstanding = au.bytes_sent - au.bytes_completed;
    if (outstanding >= AUDS_MAX_OUTSTANDING_BYTES)
        return 0;
    uint32_t n = (AUDS_MAX_OUTSTANDING_BYTES - outstanding) / 4u;
    uint32_t chunks = 0;
    for (uint32_t i = 0; i < AUDS_CHUNKS; i++)
        if (!au.busy[i])
            chunks += AUDS_CHUNK_FRAMES;
    return n < chunks ? n : chunks;
}

uint32_t auds_write(const int16_t *pcm, uint32_t frames)
{
    if (!au.running || !frames || !vchiq_bulk_tx_space())
        return 0;
    uint32_t idx = au.next_chunk;
    if (au.busy[idx])
        return 0;
    if (frames > AUDS_CHUNK_FRAMES)
        frames = AUDS_CHUNK_FRAMES;
    uint32_t room = auds_free_frames();
    if (frames > room)
        frames = room;
    if (!frames)
        return 0;

    uint32_t bytes = frames * 4u;
    uint32_t phys = au.buf_phys + idx * AUDS_CHUNK_BYTES;
    memcpy((void *)(uintptr_t)phys, pcm, bytes);   /* VC heap: uncached */

    vc_audio_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = VC_AUDIO_MSG_TYPE_WRITE;
    m.u.write.count      = bytes;
    m.u.write.cookie1    = AUDS_COOKIE1;
    m.u.write.cookie2    = AUDS_COOKIE2;
    m.u.write.silence    = 0;
    m.u.write.max_packet = 0;        /* one bulk carries the whole chunk */
    if (!vchiq_queue_message(au.service, &m, sizeof(m)))
        return 0;
    if (!vchiq_bulk_transmit(au.service, vchiq_bus_addr(phys), bytes,
                             (void *)(uintptr_t)idx)) {
        /* The WRITE is already in: the firmware now expects 'bytes' of
           bulk. Nothing sensible to do but try again next time - the
           service resynchronises on the cookies. */
        return 0;
    }
    au.busy[idx] = true;
    au.next_chunk = (idx + 1u) % AUDS_CHUNKS;
    au.chunks_sent++;
    au.bytes_sent += bytes;
    return frames;
}

void auds_poll(void)
{
    if (au.open)
        vchiq_poll();
}

uint32_t auds_chunks_sent(void)
{
    return au.chunks_sent;
}

uint32_t auds_bytes_completed(void)
{
    return au.bytes_completed;
}

uint32_t auds_completes(void)
{
    return au.completes;
}

uint32_t auds_bytes_sent(void)
{
    return au.bytes_sent;
}
