/* vcsm.c - VideoCore shared-memory ("SMEM") VCHIQ service client.
   See vcsm.h for why this exists. Wire format from Linux
   drivers/staging/vc04_services/vc-sm-cma/vc_sm_defs.h (GPL-2.0). */

#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "rpi.h"
#include "systimer.h"
#include "vchiq.h"
#include "vcsm.h"

#define VCSM_FOURCC        VCHIQ_FOURCC('S','M','E','M')

/* Service protocol version pair, as the Linux client opens it with. */
#define VC_SM_VER          1
#define VC_SM_MIN_VER      0

#define VC_SM_PROTOCOL_VERSION 2
#define VC_SM_RESOURCE_NAME    32

#define REPLY_TIMEOUT_US   1000000u

enum {
    VC_SM_MSG_TYPE_ALLOC = 0,
    VC_SM_MSG_TYPE_LOCK,
    VC_SM_MSG_TYPE_UNLOCK,
    VC_SM_MSG_TYPE_UNLOCK_NOANS,
    VC_SM_MSG_TYPE_FREE,
    VC_SM_MSG_TYPE_RESIZE,
    VC_SM_MSG_TYPE_WALK_ALLOC,
    VC_SM_MSG_TYPE_ACTION_CLEAN,
    VC_SM_MSG_TYPE_IMPORT,
    VC_SM_MSG_TYPE_CLIENT_VERSION,
};

/* Allocation cache behaviour. Our buffers are mapped uncached on the ARM
   (the VC heap region), so the VideoCore must not assume otherwise. */
#define VC_SM_ALLOC_NON_CACHED 1

typedef struct {
    uint32_t type;
    uint32_t trans_id;
} vcsm_msg_hdr_t;

typedef struct {
    vcsm_msg_hdr_t hdr;
    uint32_t type;                   /* vc_sm_alloc_type_t */
    uint32_t addr;
    uint32_t size;
    uint32_t kernel_id;
    uint32_t allocator;
    char     name[VC_SM_RESOURCE_NAME];
} vcsm_import_msg_t;

typedef struct {
    vcsm_msg_hdr_t hdr;
    uint32_t res_handle;
    uint32_t res_mem;
} vcsm_free_msg_t;

typedef struct {
    vcsm_msg_hdr_t hdr;
    uint32_t version;
} vcsm_version_msg_t;

/* Replies begin with the transaction id they answer. */
typedef struct {
    uint32_t trans_id;
    uint32_t res_handle;
} vcsm_import_result_t;

static struct {
    bool     inited;
    int      service;
    uint32_t next_trans;
    uint32_t wait_trans;             /* outstanding transaction, 0 = none */
    bool     reply_ready;
    uint32_t reply[8];
    uint32_t reply_size;
} sm;

static void on_data(const void *data, unsigned int size)
{
    if (size < sizeof(uint32_t))
        return;
    const uint32_t *w = (const uint32_t *)data;

    /* Every reply echoes the transaction id first; anything else is a
       VideoCore-initiated notification we do not act on. */
    if (sm.wait_trans && w[0] == sm.wait_trans) {
        sm.reply_size = (size <= sizeof(sm.reply)) ? size
                                                   : (uint32_t)sizeof(sm.reply);
        memcpy(sm.reply, data, sm.reply_size);
        sm.reply_ready = true;
    } else {
        LOG_DEBUG("vcsm: unmatched message trans=%"PRIu32"\r\n", w[0]);
    }
}

/* Send a message and, when 'reply' is given, wait for its answer. */
static bool sendwait(vcsm_msg_hdr_t *msg, uint32_t size,
                     void *reply, uint32_t reply_size)
{
    if (!sm.inited)
        return false;

    if (++sm.next_trans == 0)
        sm.next_trans = 1;
    msg->trans_id = sm.next_trans;

    sm.wait_trans = reply ? msg->trans_id : 0;
    sm.reply_ready = false;

    uint32_t start = RPI_GetSystemTime();
    while (!vchiq_queue_message(sm.service, msg, size)) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US) {
            sm.wait_trans = 0;
            return false;
        }
    }

    if (!reply)
        return true;

    while (!sm.reply_ready) {
        vchiq_poll();
        if ((RPI_GetSystemTime() - start) > REPLY_TIMEOUT_US) {
            LOG_INFO("vcsm: reply timeout (type %"PRIu32")\r\n", msg->type);
            sm.wait_trans = 0;
            return false;
        }
    }
    sm.wait_trans = 0;

    uint32_t n = (sm.reply_size < reply_size) ? sm.reply_size : reply_size;
    memcpy(reply, sm.reply, n);
    return true;
}

bool vcsm_init(void)
{
    if (sm.inited)
        return true;

    static const vchiq_callbacks_t cbs = {
        .on_data = on_data,
    };
    sm.service = vchiq_open_service(VCSM_FOURCC, VC_SM_VER, VC_SM_MIN_VER,
                                    &cbs);
    if (sm.service < 0)
        return false;

    sm.inited = true;

    /* Version handshake: the reference client sends this without waiting
       for an answer. */
    vcsm_version_msg_t ver;
    memset(&ver, 0, sizeof(ver));
    ver.hdr.type = VC_SM_MSG_TYPE_CLIENT_VERSION;
    ver.version = VC_SM_PROTOCOL_VERSION;
    sendwait(&ver.hdr, sizeof(ver), NULL, 0);

    LOG_DEBUG("vcsm: service open\r\n");
    return true;
}

uint32_t vcsm_import(uint32_t busaddr, uint32_t size, const char *name)
{
    if (!sm.inited || !busaddr || !size)
        return 0;

    vcsm_import_msg_t msg;
    vcsm_import_result_t reply;
    memset(&msg, 0, sizeof(msg));
    memset(&reply, 0, sizeof(reply));

    msg.hdr.type   = VC_SM_MSG_TYPE_IMPORT;
    msg.type       = VC_SM_ALLOC_NON_CACHED;
    msg.addr       = busaddr;
    msg.size       = size;
    /* Echoed back by the VideoCore to identify this registration; the
       address is unique among our buffers and needs no side table. */
    msg.kernel_id  = busaddr;
    msg.allocator  = 1;
    strncpy(msg.name, name ? name : "pi1mhz", sizeof(msg.name) - 1);

    if (!sendwait(&msg.hdr, sizeof(msg), &reply, sizeof(reply)))
        return 0;

    if (!reply.res_handle) {
        LOG_INFO("vcsm: import of %08"PRIx32" (%"PRIu32" bytes) refused\r\n",
                 busaddr, size);
        return 0;
    }
    LOG_DEBUG("vcsm: imported %08"PRIx32" -> handle %08"PRIx32"\r\n",
              busaddr, reply.res_handle);
    return reply.res_handle;
}

void vcsm_free(uint32_t handle)
{
    if (!sm.inited || !handle)
        return;

    vcsm_free_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type   = VC_SM_MSG_TYPE_FREE;
    msg.res_handle = handle;
    sendwait(&msg.hdr, sizeof(msg), NULL, 0);
}
