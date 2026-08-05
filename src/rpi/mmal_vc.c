/*
    mmal_vc.c - minimal MMAL-over-VCHIQ client for bare metal

    Protocol behaviour is copied from raspberrypi/userland
    interface/mmal/vc/mmal_vc_client.c + mmal_vc_api.c (BSD-3-Clause):

      - control messages are VCHIQ DATA messages on the 'mmal' service;
        each starts with mmal_worker_msg_header_t. Synchronous calls set
        header.context to a cookie; the reply echoes it back.
      - every port here runs in zero-copy mode, so payload never crosses
        the wire. A buffer is submitted as a BUFFER_FROM_HOST message
        whose 'data' field carries the buffer's VideoCore memory handle
        (minted by the SMEM service, see vcsm.c); the component reads or
        writes that memory itself and returns the buffer, with its length
        and flags updated, in a BUFFER_TO_HOST message.
      - a zero-length EOS buffer is an ordinary BUFFER_FROM_HOST on a
        zero-copy port (the reference client excludes such ports from the
        ZEROLEN variant).
*/

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "rpi.h"
#include "systimer.h"
#include "vchiq.h"
#include "mmal_vc.h"

#define MMAL_CONTROL_FOURCC VCHIQ_FOURCC('m','m','a','l')

/* A healthy VideoCore replies in well under a millisecond, so half a second
   is still an enormous margin - but it bounds how long the main poll loop
   can be held up by a VC that has stopped answering. */
#define REPLY_TIMEOUT_US 500000u

static struct {
    bool inited;
    bool dead;                       /* latched on the first timeout */
    int  service;                    /* VCHIQ service id */
    mmal_vc_client_callbacks_t cb;

    /* one synchronous call at a time */
    uint32_t wait_context;           /* cookie of the outstanding call, 0 = none */
    bool     reply_ready;
    uint8_t  reply[MMAL_WORKER_MAX_MSG_LEN];
    uint32_t reply_size;
    uint32_t next_context;
} client;

/* Buffers currently submitted to the VideoCore. A BUFFER_TO_HOST message
   names its buffer by the host pointer we put in client_context, and that
   pointer is dereferenced and written through - so it is checked against
   this registry rather than trusted. The decoder never has more than five
   buffers out (2 input + 3 output); the spare entries are headroom. */
#define MMAL_MAX_INFLIGHT 8
static mmal_vc_buffer_t *inflight[MMAL_MAX_INFLIGHT];

/* Latched on a failure that leaves a VCHIQ service open: the service
   cannot be taken back (the protocol's CLOSE handshake is VC-initiated
   here), so a retry would burn the last free service slot. */
static bool mmal_failed;

/* ------------------------------------------------------------------ */
/* VCHIQ callbacks                                                    */
/* ------------------------------------------------------------------ */

static bool inflight_add(mmal_vc_buffer_t *buf)
{
    int free_slot = -1;
    if (!buf)
        return false;
    for (int i = 0; i < MMAL_MAX_INFLIGHT; i++) {
        if (inflight[i] == buf)
            return true;                 /* already registered */
        if (!inflight[i] && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return false;
    inflight[free_slot] = buf;
    return true;
}

static void inflight_remove(mmal_vc_buffer_t *buf)
{
    for (int i = 0; i < MMAL_MAX_INFLIGHT; i++)
        if (inflight[i] == buf)
            inflight[i] = NULL;
}

static bool inflight_known(const mmal_vc_buffer_t *buf)
{
    if (!buf)                            /* an empty slot is also NULL */
        return false;
    for (int i = 0; i < MMAL_MAX_INFLIGHT; i++)
        if (inflight[i] == buf)
            return true;
    return false;
}

static void complete_buffer(mmal_vc_buffer_t *buf)
{
    buf->in_flight = false;
    if (client.cb.buffer_done)
        client.cb.buffer_done(buf);
}

static void handle_buffer_to_host(const mmal_worker_buffer_from_host_t *msg)
{
    mmal_vc_buffer_t *buf = (mmal_vc_buffer_t *)(uintptr_t)msg->drvbuf.client_context;
    /* client_context comes back off the wire; a garbled message must not
       be able to write through it to arbitrary ARM memory. */
    if (!inflight_known(buf)) {
        LOG_DEBUG("mmal: buffer_to_host with unknown context\r\n");
        return;
    }
    /* Off the books before the callback, which may resubmit it. */
    inflight_remove(buf);

    buf->cmd    = msg->buffer_header.cmd;
    buf->offset = msg->buffer_header.offset;
    buf->length = msg->buffer_header.length;
    buf->flags  = msg->buffer_header.flags;
    buf->pts    = msg->buffer_header.pts;

    /* Written as two tests: offset + length would wrap. */
    if (msg->buffer_header.length > buf->alloc_size ||
        msg->buffer_header.offset > buf->alloc_size - msg->buffer_header.length) {
        LOG_INFO("mmal: returned buffer too big (%"PRIu32")\r\n",
                 msg->buffer_header.length);
        buf->length = 0;
        buf->flags |= MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED;
        complete_buffer(buf);
        return;
    }

    /* Zero-copy: any payload is already in the buffer's own memory, which
       the component wrote directly. The message carries only the updated
       header, so the buffer is complete as soon as it arrives. */
    complete_buffer(buf);
}

static void on_data(const void *data, unsigned int size)
{
    const mmal_worker_msg_header_t *h = (const mmal_worker_msg_header_t *)data;

    if (size < sizeof(*h) || h->magic != MMAL_MAGIC) {
        LOG_INFO("mmal: bad message (size %u)\r\n", size);
        return;
    }

    /* The casts below read structures out of the message, so the message
       must actually be long enough for the fields that get read - a short
       one would read (and, through client_context, write) past its end.
       The bound is the offset of the first field NOT read, never sizeof:
       the VideoCore returns a buffer in 292 bytes (hardware-measured, VC
       version 16.1) because the struct's trailing alignment padding, and
       the unused short_data tail, do not travel. */
    if (h->msgid == MMAL_WORKER_BUFFER_TO_HOST) {
        if (size < offsetof(mmal_worker_buffer_from_host_t, short_data)) {
            LOG_DEBUG("mmal: short buffer_to_host (%u)\r\n", size);
            return;
        }
        handle_buffer_to_host((const mmal_worker_buffer_from_host_t *)data);
        return;
    }

    if (h->msgid == MMAL_WORKER_EVENT_TO_HOST) {
        const mmal_worker_event_to_host_t *ev =
            (const mmal_worker_event_to_host_t *)data;
        if (size < offsetof(mmal_worker_event_to_host_t, data)) {
            LOG_DEBUG("mmal: short event_to_host (%u)\r\n", size);
            return;
        }
        /* Events larger than the inline space are delivered by bulk, which
           this client never uses. Nothing video_decode emits is that big;
           drop it rather than pass on data we do not have. */
        if (ev->length > MMAL_WORKER_EVENT_SPACE)
            return;
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

/* ------------------------------------------------------------------ */
/* Synchronous call helper                                            */
/* ------------------------------------------------------------------ */

static bool sendwait(mmal_worker_msg_header_t *msg, uint32_t size, uint32_t msgid,
                     void *reply, uint32_t reply_size)
{
    /* Once a call has timed out the VC side is not coming back for this
       session: fail every later call immediately rather than spending
       another REPLY_TIMEOUT_US each time stalling the main loop. */
    if (!client.inited || client.dead)
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
    while (!vchiq_queue_message(client.service, msg, size)) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US) {
            client.wait_context = 0;
            client.dead = true;
            return false;
        }
    }

    while (!client.reply_ready) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US) {
            LOG_INFO("mmal: reply timeout (msgid %"PRIu32")\r\n", msgid);
            client.wait_context = 0;
            client.dead = true;
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

    /* A previous attempt left an 'mmal' service open on the VCHIQ side.
       There are only two service slots, so trying again would strand the
       one vcsm still needs. */
    if (mmal_failed)
        return false;

    if (!vchiq_init())
        return false;

    memset(&client, 0, sizeof(client));
    client.cb = *callbacks;

    static const vchiq_callbacks_t vcb = {
        .on_data = on_data,
    };
    /* on_data captures 'client' statically; safe to pass const struct */
    client.service = vchiq_open_service(MMAL_CONTROL_FOURCC,
                                        MMAL_WORKER_VER_MAJOR,
                                        MMAL_WORKER_VER_MINIMUM, &vcb);
    if (client.service < 0)
        return false;

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
            mmal_failed = true;
            return false;
        }
    } else {
        client.inited = false;
        mmal_failed = true;
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
    /* Current firmware answers with the full port description but leaves
       'found' as 0 (hardware-observed, VC version 16.1) - so judge by the
       evidence: success status and a port of the requested type. */
    if (reply.status != MMAL_STATUS_SUCCESS ||
        (!reply.found && reply.port.type != port_type)) {
        LOG_INFO("mmal: no port type %"PRIu32" index %"PRIu32"\r\n",
                 port_type, index);
        return false;
    }

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

/* Discard whatever the port is holding. Plain PORT_ACTION flush: the
   MMAL_WORKER_PORT_FLUSH message exists to order a flush against payload
   still crossing as a bulk transfer, which zero-copy ports never have. */
bool mmal_vc_port_flush(mmal_vc_port_t *port)
{
    return port_action(port, MMAL_WORKER_PORT_ACTION_FLUSH, false);
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
    /* The value bytes continue past the header into 'space' - copy in two
       steps so the compiler sees each write bounded by its own member. */
    memcpy(&msg.param, param, sizeof(msg.param));
    if (size > sizeof(msg.param))
        memcpy(msg.space, (const uint8_t *)param + sizeof(msg.param),
               size - sizeof(msg.param));

    uint32_t msg_len = (uint32_t)(sizeof(msg) - sizeof(msg.space)) + size
                       - (uint32_t)sizeof(mmal_parameter_header_t);
    if (!sendwait(&msg.header, msg_len, MMAL_WORKER_PORT_PARAMETER_SET,
                  &reply, sizeof(reply)))
        return false;
    return reply.status == MMAL_STATUS_SUCCESS;
}

bool mmal_vc_port_set_zero_copy(mmal_vc_port_t *port)
{
    struct {
        mmal_parameter_header_t hdr;
        uint32_t enable;
    } p = { { MMAL_PARAMETER_ZERO_COPY, sizeof(p) }, 1 };

    return mmal_vc_port_parameter_set(port, &p, sizeof(p));
}

bool mmal_vc_submit_buffer(mmal_vc_port_t *port, mmal_vc_buffer_t *buf)
{
    mmal_worker_buffer_from_host_t msg;

    /* Zero-copy: 'data' carries the VC memory handle instead of an
       address, so a buffer with no handle can never be submitted. */
    if (!client.inited || !buf->vc_handle)
        return false;

    memset(&msg, 0, sizeof(msg));
    msg.header.magic = MMAL_MAGIC;
    msg.drvbuf.magic = MMAL_MAGIC;
    msg.drvbuf.component_handle = port->component;
    msg.drvbuf.port_handle = port->handle;
    msg.drvbuf.client_context = (uint32_t)(uintptr_t)buf;

    msg.buffer_header.cmd = 0;
    msg.buffer_header.data = buf->vc_handle;
    msg.buffer_header.alloc_size = buf->alloc_size;
    msg.buffer_header.length = buf->length;
    msg.buffer_header.offset = 0;
    msg.buffer_header.flags = buf->flags;
    msg.buffer_header.pts = buf->pts;
    msg.buffer_header.dts = MMAL_TIME_UNKNOWN;
    msg.payload_in_message = 0;
    msg.is_zero_copy = 1;

    /* A zero-length EOS marker goes as an ordinary BUFFER_FROM_HOST here:
       the ZEROLEN variant exists only to carry its dummy bulk transfer,
       and the reference client excludes zero-copy ports from it. */
    msg.header.msgid = MMAL_WORKER_BUFFER_FROM_HOST;
    msg.header.context = 0;

    /* Only a buffer on this list is accepted back from the VideoCore. */
    if (!inflight_add(buf))
        return false;

    /* A full TX slot is transient; the caller simply retries. */
    if (!vchiq_queue_message(client.service, &msg, sizeof(msg))) {
        inflight_remove(buf);
        return false;
    }

    buf->port = port;
    buf->in_flight = true;
    return true;
}
