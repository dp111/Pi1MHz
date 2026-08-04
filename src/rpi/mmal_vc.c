/*
    mmal_vc.c - minimal MMAL-over-VCHIQ client for bare metal

    Protocol behaviour is copied from raspberrypi/userland
    interface/mmal/vc/mmal_vc_client.c + mmal_vc_api.c (BSD-3-Clause):

      - control messages are VCHIQ DATA messages on the 'mmal' service;
        each starts with mmal_worker_msg_header_t. Synchronous calls set
        header.context to a cookie; the reply echoes it back.
      - a buffer with payload is a BUFFER_FROM_HOST message IMMEDIATELY
        followed by a VCHIQ bulk transmit of the payload; the VC matches
        the two by arrival order per service, so the pair must never be
        split (single-threaded here, so it cannot be).
      - the VC returns buffers with BUFFER_TO_HOST; if there is payload
        the host must queue a bulk receive, and the buffer completes when
        the matching BULK_RX_DONE arrives.
      - a zero-length EOS buffer uses BUFFER_FROM_HOST_ZEROLEN + a dummy
        8-byte bulk transmit (VC versions >= 12.2, i.e. everything now).
      - PORT_FLUSH (msgid 25) with a dummy bulk keeps a flush ordered
        against in-flight bulk data (VC versions >= 15).
*/

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "rpi.h"
#include "systimer.h"
#include "vchiq.h"
#include "mmal_vc.h"

#define MMAL_CONTROL_FOURCC VCHIQ_FOURCC('m','m','a','l')

#define REPLY_TIMEOUT_US 2000000u

static struct {
    bool inited;
    mmal_vc_client_callbacks_t cb;

    /* one synchronous call at a time */
    uint32_t wait_context;           /* cookie of the outstanding call, 0 = none */
    bool     reply_ready;
    uint8_t  reply[MMAL_WORKER_MAX_MSG_LEN];
    uint32_t reply_size;
    uint32_t next_context;

    /* scratch for the dummy bulk transfers (EOS / flush) in the VC heap */
    uint32_t dummy_bus;
} client;

/* ------------------------------------------------------------------ */
/* VCHIQ callbacks                                                    */
/* ------------------------------------------------------------------ */

static void complete_buffer(mmal_vc_buffer_t *buf)
{
    buf->in_flight = false;
    if (client.cb.buffer_done)
        client.cb.buffer_done(buf);
}

static void handle_buffer_to_host(const mmal_worker_buffer_from_host_t *msg)
{
    mmal_vc_buffer_t *buf = (mmal_vc_buffer_t *)(uintptr_t)msg->drvbuf.client_context;
    if (!buf) {
        LOG_INFO("mmal: buffer_to_host with no context\r\n");
        return;
    }

    buf->cmd    = msg->buffer_header.cmd;
    buf->offset = msg->buffer_header.offset;
    buf->length = msg->buffer_header.length;
    buf->flags  = msg->buffer_header.flags;
    buf->pts    = msg->buffer_header.pts;

    if (msg->buffer_header.offset + msg->buffer_header.length > buf->alloc_size) {
        LOG_INFO("mmal: returned buffer too big (%"PRIu32")\r\n",
                 msg->buffer_header.length);
        buf->length = 0;
        buf->flags |= MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED;
        complete_buffer(buf);
        return;
    }

    if (!msg->is_zero_copy &&
        (msg->buffer_header.length != 0 ||
         (msg->buffer_header.flags & MMAL_BUFFER_HEADER_FLAG_EOS))) {
        uint32_t len = (msg->buffer_header.length + 3u) & ~3u;
        if (!len)
            len = 8;                 /* dummy transfer accompanies bare EOS */

        if (msg->payload_in_message == 0) {
            /* Data follows by bulk into our buffer; completion arrives as
               BULK_RX_DONE (bulk_rx_done() below) with 'buf' as the tag. */
            if (!vchiq_bulk_receive(buf->busaddr + msg->buffer_header.offset,
                                    len, buf)) {
                LOG_INFO("mmal: bulk receive queue full\r\n");
                buf->length = 0;
                buf->flags |= MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED;
                complete_buffer(buf);
            }
        } else if (msg->payload_in_message <= MMAL_VC_SHORT_DATA) {
            /* Tiny payload came inside the message itself */
            memcpy((void *)(uintptr_t)(buf->busaddr & 0x3FFFFFFFu),
                   msg->short_data, msg->payload_in_message);
            buf->offset = 0;
            buf->length = msg->payload_in_message;
            complete_buffer(buf);
        } else {
            /* "impossible" per the reference - but never leave the buffer
               stranded in flight */
            LOG_INFO("mmal: invalid short payload %"PRIu32"\r\n",
                     msg->payload_in_message);
            buf->length = 0;
            buf->flags |= MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED;
            complete_buffer(buf);
        }
    } else {
        /* No payload - e.g. an input buffer being released */
        complete_buffer(buf);
    }
}

static void on_data(const void *data, unsigned int size)
{
    const mmal_worker_msg_header_t *h = (const mmal_worker_msg_header_t *)data;

    if (size < sizeof(*h) || h->magic != MMAL_MAGIC) {
        LOG_INFO("mmal: bad message (size %u)\r\n", size);
        return;
    }

    if (h->msgid == MMAL_WORKER_BUFFER_TO_HOST) {
        handle_buffer_to_host((const mmal_worker_buffer_from_host_t *)data);
        return;
    }

    if (h->msgid == MMAL_WORKER_EVENT_TO_HOST) {
        const mmal_worker_event_to_host_t *ev =
            (const mmal_worker_event_to_host_t *)data;
        if (ev->length > MMAL_WORKER_EVENT_SPACE) {
            /* Larger events arrive by bulk - and the VC has ALREADY queued
               its side of that transfer, so it must be received or every
               later bulk pairs one-off. Sink it into the scratch page
               (the VC transfers min(sizes), so a short receive is fine);
               nothing video_decode emits is this big. */
            LOG_INFO("mmal: oversize event 0x%"PRIx32" sunk\r\n", ev->cmd);
            uint32_t len = (ev->length + 3u) & ~3u;
            vchiq_bulk_receive(client.dummy_bus, len > 4096u ? 4096u : len,
                               NULL);
            return;
        }
        if (client.cb.event)
            client.cb.event(ev->port_type, ev->port_num, ev->cmd,
                            ev->data, ev->length);
        return;
    }

    /* Everything else is the reply to the outstanding synchronous call */
    if (client.wait_context && h->context == client.wait_context) {
        client.reply_size = (size <= sizeof(client.reply)) ? size : (uint32_t)sizeof(client.reply);
        memcpy(client.reply, data, client.reply_size);
        client.reply_ready = true;
    } else {
        LOG_DEBUG("mmal: unmatched reply msgid %"PRIu32"\r\n", h->msgid);
    }
}

static void bulk_rx_done(void *user, int actual)
{
    mmal_vc_buffer_t *buf = (mmal_vc_buffer_t *)user;
    if (!buf)
        return;                      /* sunk transfer (oversize event) */
    if (actual < 0) {
        buf->length = 0;
        buf->flags |= MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED;
    }
    complete_buffer(buf);
}

static void bulk_tx_done(void *user, int actual)
{
    /* Input payload has been fetched by the VC. The buffer itself is
       released later via BUFFER_TO_HOST, so nothing to do here. */
    (void)user;
    (void)actual;
}

/* ------------------------------------------------------------------ */
/* Synchronous call helper                                            */
/* ------------------------------------------------------------------ */

static bool sendwait(mmal_worker_msg_header_t *msg, uint32_t size, uint32_t msgid,
                     void *reply, uint32_t reply_size)
{
    if (!client.inited)
        return false;

    msg->magic = MMAL_MAGIC;
    msg->msgid = msgid;
    msg->control_service = 0;
    msg->status = 0;
    msg->dummy = 0;
    if (++client.next_context == 0)
        client.next_context = 1;
    msg->context = client.next_context;

    client.wait_context = msg->context;
    client.reply_ready = false;

    /* TX slots can transiently run out while the VC is busy - retry. */
    uint32_t start = RPI_GetSystemTime();
    while (!vchiq_queue_message(msg, size)) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US) {
            client.wait_context = 0;
            return false;
        }
    }

    while (!client.reply_ready) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US) {
            LOG_INFO("mmal: reply timeout (msgid %"PRIu32")\r\n", msgid);
            client.wait_context = 0;
            return false;
        }
    }
    client.wait_context = 0;

    if (reply && reply_size) {
        uint32_t n = (client.reply_size < reply_size) ? client.reply_size : reply_size;
        memcpy(reply, client.reply, n);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void mmal_vc_poll(void)
{
    vchiq_poll();
}

bool mmal_vc_init(const mmal_vc_client_callbacks_t *callbacks)
{
    if (client.inited)
        return true;

    if (!vchiq_init())
        return false;

    memset(&client, 0, sizeof(client));
    client.cb = *callbacks;

    static const vchiq_callbacks_t vcb = {
        .on_data = on_data,
        .on_bulk_tx_done = bulk_tx_done,
        .on_bulk_rx_done = bulk_rx_done,
    };
    /* on_data captures 'client' statically; safe to pass const struct */
    if (!vchiq_open_service(MMAL_CONTROL_FOURCC,
                            MMAL_WORKER_VER_MAJOR, MMAL_WORKER_VER_MINIMUM,
                            &vcb))
        return false;

    uint32_t handle;
    uint32_t dummy_phys = vchiq_alloc_shared(4096, &handle);
    if (!dummy_phys)
        return false;
    client.dummy_bus = vchiq_bus_addr(dummy_phys);

    client.inited = true;

    /* Version handshake - purely a sanity check */
    mmal_worker_version_t ver;
    memset(&ver, 0, sizeof(ver));
    if (sendwait(&ver.header, sizeof(ver),
                 MMAL_WORKER_GET_VERSION, &ver, sizeof(ver))) {
        LOG_DEBUG("mmal: VC version %"PRIu32".%"PRIu32" (min %"PRIu32")\r\n",
                  ver.major, ver.minor, ver.minimum);
        if (ver.major < 12) {
            LOG_INFO("mmal: VC MMAL too old (%"PRIu32")\r\n", ver.major);
            client.inited = false;
            return false;
        }
    } else {
        client.inited = false;
        return false;
    }

    return true;
}

bool mmal_vc_component_create(const char *name, uint32_t *handle,
                              uint32_t *inputs, uint32_t *outputs)
{
    mmal_worker_component_create_t msg;
    mmal_worker_component_create_reply_t reply;
    memset(&msg, 0, sizeof(msg));
    msg.client_component = 0x50314D48;   /* 'P1MH' - echoed in events */
    msg.pid = 1;
    strncpy(msg.name, name, sizeof(msg.name) - 1);

    if (!sendwait(&msg.header, sizeof(msg), MMAL_WORKER_COMPONENT_CREATE,
                  &reply, sizeof(reply)))
        return false;
    if (reply.status != MMAL_STATUS_SUCCESS) {
        LOG_INFO("mmal: create '%s' failed (%"PRIu32")\r\n", name, reply.status);
        return false;
    }
    *handle = reply.component_handle;
    if (inputs)  *inputs  = reply.input_num;
    if (outputs) *outputs = reply.output_num;
    return true;
}

bool mmal_vc_component_enable(uint32_t handle)
{
    mmal_worker_component_handle_msg_t msg;
    mmal_worker_reply_t reply;
    memset(&msg, 0, sizeof(msg));
    msg.component_handle = handle;
    if (!sendwait(&msg.header, sizeof(msg), MMAL_WORKER_COMPONENT_ENABLE,
                  &reply, sizeof(reply)))
        return false;
    return reply.status == MMAL_STATUS_SUCCESS;
}

bool mmal_vc_port_info_get(uint32_t component, uint32_t port_type,
                           uint32_t index, mmal_vc_port_t *port)
{
    mmal_worker_port_info_get_t msg;
    mmal_worker_port_info_t reply;
    memset(&msg, 0, sizeof(msg));
    msg.component_handle = component;
    msg.port_type = port_type;
    msg.index = index;

    if (!sendwait(&msg.header, sizeof(msg), MMAL_WORKER_PORT_INFO_GET,
                  &reply, sizeof(reply)))
        return false;
    if (reply.status != MMAL_STATUS_SUCCESS || !reply.found)
        return false;

    port->component = component;
    port->type = port_type;
    port->index = index;
    port->handle = reply.port_handle;
    port->port = reply.port;
    port->format = reply.format;
    port->es = reply.es;
    memcpy(port->extradata, reply.extradata, sizeof(port->extradata));
    return true;
}

/* Push our copies of port+format back to the VC and re-read the result
   (the component may adjust buffer requirements in response). */
bool mmal_vc_port_set_format(mmal_vc_port_t *port)
{
    mmal_worker_port_info_set_t msg;
    mmal_worker_port_info_t reply;
    memset(&msg, 0, sizeof(msg));
    msg.component_handle = port->component;
    msg.port_type = port->type;
    msg.index = port->index;
    msg.port = port->port;
    msg.format = port->format;
    msg.es = port->es;
    memcpy(msg.extradata, port->extradata,
           (port->format.extradata_size <= sizeof(msg.extradata)) ?
            port->format.extradata_size : sizeof(msg.extradata));

    if (!sendwait(&msg.header, sizeof(msg), MMAL_WORKER_PORT_INFO_SET,
                  &reply, sizeof(reply)))
        return false;
    if (reply.status != MMAL_STATUS_SUCCESS)
        return false;

    /* Keep the port handle from PORT_INFO_GET - the reference client never
       reads it from a SET reply, so do not trust that field here. */
    port->port = reply.port;
    port->format = reply.format;
    port->es = reply.es;
    memcpy(port->extradata, reply.extradata, sizeof(port->extradata));
    return true;
}

static bool port_action(mmal_vc_port_t *port, uint32_t action, bool with_port_copy)
{
    mmal_worker_port_action_t msg;
    mmal_worker_reply_t reply;
    memset(&msg, 0, sizeof(msg));
    msg.component_handle = port->component;
    msg.port_handle = port->handle;
    msg.action = action;
    if (with_port_copy)
        msg.param.enable.port = port->port;

    if (!sendwait(&msg.header, sizeof(msg), MMAL_WORKER_PORT_ACTION,
                  &reply, sizeof(reply)))
        return false;
    if (reply.status != MMAL_STATUS_SUCCESS) {
        LOG_INFO("mmal: port action %"PRIu32" failed (%"PRIu32")\r\n",
                 action, reply.status);
        return false;
    }
    return true;
}

bool mmal_vc_port_enable(mmal_vc_port_t *port)
{
    return port_action(port, MMAL_WORKER_PORT_ACTION_ENABLE, true);
}

bool mmal_vc_port_disable(mmal_vc_port_t *port)
{
    return port_action(port, MMAL_WORKER_PORT_ACTION_DISABLE, false);
}

/* Plain flush via PORT_ACTION. Correct for ports that have never carried
   host->VC payload (e.g. video_decode's output port) - the reference
   warns the other end may not even be set up for bulk on such ports. */
bool mmal_vc_port_flush_normal(mmal_vc_port_t *port)
{
    return port_action(port, MMAL_WORKER_PORT_ACTION_FLUSH, false);
}

/* Flush with the dummy-bulk handshake so the flush cannot overtake
   payload that is still crossing as a bulk transfer. Use ONLY on ports
   that have sent payload (input ports); see mmal_vc_port_flush_normal. */
bool mmal_vc_port_flush(mmal_vc_port_t *port)
{
    mmal_worker_buffer_from_host_t msg;
    mmal_worker_reply_t reply;
    memset(&msg, 0, sizeof(msg));
    msg.drvbuf.magic = MMAL_MAGIC;
    msg.drvbuf.component_handle = port->component;
    msg.drvbuf.port_handle = port->handle;
    msg.drvbuf.client_context = 0;

    msg.header.magic = MMAL_MAGIC;
    msg.header.msgid = MMAL_WORKER_PORT_FLUSH;
    if (++client.next_context == 0)
        client.next_context = 1;
    msg.header.context = client.next_context;
    client.wait_context = msg.header.context;
    client.reply_ready = false;

    /* Secure the bulk slot BEFORE the message goes out: once the message
       is sent the VC expects the dummy bulk, and failing to deliver it
       would desynchronise the pairing for every later transfer. */
    uint32_t start = RPI_GetSystemTime();
    while (!vchiq_bulk_tx_space()) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US)
            goto fail;
    }
    while (!vchiq_queue_message(&msg, sizeof(msg))) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US)
            goto fail;
    }
    /* Single-threaded: the slot reserved above is still free, so this
       cannot fail unless the service died - in which case pairing is
       moot anyway. */
    vchiq_bulk_transmit(client.dummy_bus, 8, NULL);

    while (!client.reply_ready) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US) {
            LOG_INFO("mmal: flush timeout\r\n");
            goto fail;
        }
    }
    client.wait_context = 0;
    memcpy(&reply, client.reply,
           (client.reply_size < sizeof(reply)) ? client.reply_size : sizeof(reply));
    return reply.status == MMAL_STATUS_SUCCESS;

fail:
    client.wait_context = 0;
    return false;
}

bool mmal_vc_port_parameter_set(mmal_vc_port_t *port,
                                const void *param, uint32_t size)
{
    mmal_worker_port_param_set_t msg;
    mmal_worker_reply_t reply;
    if (size < sizeof(mmal_parameter_header_t) ||
        size > sizeof(mmal_parameter_header_t) + sizeof(msg.space))
        return false;
    memset(&msg, 0, sizeof(msg.header) + 8u);
    msg.component_handle = port->component;
    msg.port_handle = port->handle;
    memcpy(&msg.param, param, size);

    uint32_t msg_len = (uint32_t)(sizeof(msg) - sizeof(msg.space)) + size
                       - (uint32_t)sizeof(mmal_parameter_header_t);
    if (!sendwait(&msg.header, msg_len, MMAL_WORKER_PORT_PARAMETER_SET,
                  &reply, sizeof(reply)))
        return false;
    return reply.status == MMAL_STATUS_SUCCESS;
}

bool mmal_vc_submit_buffer(mmal_vc_port_t *port, mmal_vc_buffer_t *buf)
{
    mmal_worker_buffer_from_host_t msg;
    uint32_t msgid = MMAL_WORKER_BUFFER_FROM_HOST;
    uint32_t bulk_len = 0;

    if (!client.inited)
        return false;

    memset(&msg, 0, sizeof(msg));
    msg.header.magic = MMAL_MAGIC;
    msg.drvbuf.magic = MMAL_MAGIC;
    msg.drvbuf.component_handle = port->component;
    msg.drvbuf.port_handle = port->handle;
    msg.drvbuf.client_context = (uint32_t)(uintptr_t)buf;

    msg.buffer_header.cmd = 0;
    msg.buffer_header.data = buf->busaddr;
    msg.buffer_header.alloc_size = buf->alloc_size;
    msg.buffer_header.length = buf->length;
    msg.buffer_header.offset = 0;
    msg.buffer_header.flags = buf->flags;
    msg.buffer_header.pts = buf->pts;
    msg.buffer_header.dts = MMAL_TIME_UNKNOWN;
    msg.payload_in_message = 0;

    if (buf->length) {
        bulk_len = buf->length;
    } else if (buf->flags & MMAL_BUFFER_HEADER_FLAG_EOS) {
        /* zero-length EOS: special message + 8-byte dummy bulk */
        msgid = MMAL_WORKER_BUFFER_FROM_HOST_ZEROLEN;
        bulk_len = 0;
    }

    msg.header.msgid = msgid;
    msg.header.context = 0;

    /* Never split the message/bulk pair: secure the bulk slot BEFORE the
       message goes out (single-threaded, so it cannot be stolen between
       the check and the transmit). If there is no room the caller simply
       retries - unlike an unbounded drain loop here, that cannot wedge
       the whole poll loop when the channel dies. */
    bool needs_bulk = (bulk_len != 0) ||
                      (msgid == MMAL_WORKER_BUFFER_FROM_HOST_ZEROLEN);
    if (needs_bulk && !vchiq_bulk_tx_space()) {
        vchiq_poll();
        return false;
    }

    if (!vchiq_queue_message(&msg, sizeof(msg)))
        return false;

    if (bulk_len)
        vchiq_bulk_transmit(buf->busaddr + buf->offset, bulk_len, buf);
    else if (msgid == MMAL_WORKER_BUFFER_FROM_HOST_ZEROLEN)
        vchiq_bulk_transmit(client.dummy_bus, 8, NULL);

    buf->port = port;
    buf->in_flight = true;
    return true;
}
