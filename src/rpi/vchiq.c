/*
    vchiq.c - minimal bare-metal VCHIQ client (ARM side, "slave")

    See vchiq.h for the design notes. The wire format below mirrors the
    Linux vchiq driver (drivers/staging/vc04_services/interface/vchiq_arm,
    SPDX GPL-2.0 OR BSD-3-Clause), protocol version 8:

      - shared "slot" memory: 4K slots, slot 0 holds vchiq_slot_zero with
        the two shared_state blocks (master = VideoCore, slave = ARM)
      - messages are packed back to back in slots taken from the sender's
        half of the pool; tx_pos/rx_pos are monotonic byte counters whose
        upper bits index the slot_queue ring
      - consumed RX slots are recycled by appending their index to the
        REMOTE side's slot_queue at slot_queue_recycle and bumping that
        counter; the remote replenishes our TX pool the same way
      - bulk payloads never travel through slots: the slave posts a
        BULK_TX/BULK_RX message whose 8-byte payload is {pagelist bus
        address, size}; the VideoCore (bus master) does the copy with its
        own DMA and answers BULK_TX_DONE/BULK_RX_DONE {actual}

    Everything shared with the VideoCore lives in the VC heap, which the
    ARM maps effectively uncached (cache.c maps arm_mem..PERIPHERAL_BASE
    with 0x11C06), so there is no cache maintenance anywhere here.
*/

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "base.h"
#include "rpi.h"
#include "mailbox.h"
#include "systimer.h"
#include "asm-helpers.h"
#include "vchiq.h"

/* ------------------------------------------------------------------ */
/* Wire format                                                        */
/* ------------------------------------------------------------------ */

#define VCHIQ_MAGIC        VCHIQ_FOURCC('V','C','H','I')
#define VCHIQ_VERSION      8
#define VCHIQ_VERSION_MIN  3

#define VCHIQ_SLOT_SIZE       4096u
#define VCHIQ_SLOT_MASK       (VCHIQ_SLOT_SIZE - 1u)
#define VCHIQ_MAX_SLOTS       128
#define VCHIQ_MAX_SLOTS_PER_SIDE 64
#define VCHIQ_SLOT_QUEUE_MASK (VCHIQ_MAX_SLOTS_PER_SIDE - 1u)

/* Both ends must agree on sizeof(shared_state); the firmware is built
   with VCHIQ_ENABLE_DEBUG so debug[] has 11 entries. */
#define VCHIQ_DEBUG_MAX 11

/* Message types (high byte of msgid) */
#define VCHIQ_MSG_PADDING            0
#define VCHIQ_MSG_CONNECT            1
#define VCHIQ_MSG_OPEN               2
#define VCHIQ_MSG_OPENACK            3
#define VCHIQ_MSG_CLOSE              4
#define VCHIQ_MSG_DATA               5
#define VCHIQ_MSG_BULK_RX            6
#define VCHIQ_MSG_BULK_TX            7
#define VCHIQ_MSG_BULK_RX_DONE       8
#define VCHIQ_MSG_BULK_TX_DONE       9
#define VCHIQ_MSG_PAUSE             10
#define VCHIQ_MSG_RESUME            11
#define VCHIQ_MSG_REMOTE_USE        12
#define VCHIQ_MSG_REMOTE_RELEASE    13
#define VCHIQ_MSG_REMOTE_USE_ACTIVE 14

#define TYPE_SHIFT 24
#define VCHIQ_MAKE_MSG(type, srcport, dstport) \
    (((uint32_t)(type) << TYPE_SHIFT) | (((uint32_t)(srcport) & 0xFFFu) << 12) | \
     ((uint32_t)(dstport) & 0xFFFu))
#define VCHIQ_MSG_TYPE(msgid)     ((uint32_t)(msgid) >> TYPE_SHIFT)
#define VCHIQ_MSG_SRCPORT(msgid)  (((uint32_t)(msgid) >> 12) & 0xFFFu)
#define VCHIQ_MSG_DSTPORT(msgid)  ((uint32_t)(msgid) & 0xFFFu)

typedef struct {
    int32_t  armed;
    int32_t  fired;
    uint32_t unused;
} vchiq_remote_event_t;

typedef struct {
    int16_t use_count;
    int16_t release_count;
} vchiq_slot_info_t;

typedef struct {
    int32_t initialised;
    int32_t slot_first;              /* first/last (inclusive) slots owned */
    int32_t slot_last;
    int32_t slot_sync;               /* slot for synchronous messages (unused here) */
    vchiq_remote_event_t trigger;    /* "owner's slot handler should run" */
    int32_t tx_pos;                  /* next write position in the message stream */
    vchiq_remote_event_t recycle;    /* "a slot was recycled back to the owner" */
    int32_t slot_queue_recycle;      /* write index into slot_queue for recycling */
    vchiq_remote_event_t sync_trigger;
    vchiq_remote_event_t sync_release;
    int32_t slot_queue[VCHIQ_MAX_SLOTS_PER_SIDE];  /* ring of slot indexes */
    int32_t debug[VCHIQ_DEBUG_MAX];
} vchiq_shared_state_t;

typedef struct {
    int32_t magic;
    int16_t version;
    int16_t version_min;
    int32_t slot_zero_size;
    int32_t slot_size;
    int32_t max_slots;
    int32_t max_slots_per_side;
    int32_t platform_data[2];        /* [0] fragments bus address, [1] count */
    vchiq_shared_state_t master;     /* VideoCore */
    vchiq_shared_state_t slave;      /* us */
    vchiq_slot_info_t slots[VCHIQ_MAX_SLOTS];
} vchiq_slot_zero_t;

typedef struct {
    int32_t  msgid;
    uint32_t size;
    /* payload follows */
} vchiq_header_t;

#define VCHIQ_HEADER_SIZE 8u
#define CALC_STRIDE(size) (((size) + VCHIQ_HEADER_SIZE + 7u) & ~7u)

typedef struct {
    int32_t fourcc;
    int32_t client_id;
    int16_t version;
    int16_t version_min;
} vchiq_open_payload_t;

/* Bulk pagelist, as built by the Linux driver. addrs[] entries are bus
   addresses of page runs; the low 12 bits hold (number of FOLLOWING
   consecutive pages), so one entry describes a whole contiguous buffer. */
#define PAGELIST_WRITE               0   /* host -> VC (VC reads our memory) */
#define PAGELIST_READ                1   /* VC -> host (VC writes our memory) */
typedef struct {
    uint32_t length;
    uint16_t type;
    uint16_t offset;                 /* offset of data within the first page */
    uint32_t addrs[1];
} vchiq_pagelist_t;

#define MAX_FRAGMENTS      64        /* 2 * VCHIQ_NUM_CURRENT_BULKS */
#define FRAGMENT_SIZE      64        /* 2 cache lines of 32 bytes */

/* ------------------------------------------------------------------ */
/* Layout of our single shared allocation                             */
/* ------------------------------------------------------------------ */

/* 1 slot_zero slot + 2 * 16 slots (one sync + 15 data each side) */
#define NUM_SLOTS          33u
#define SLOT_MEM_SIZE      (NUM_SLOTS * VCHIQ_SLOT_SIZE)
#define FRAG_MEM_SIZE      4096u     /* 64 * 64 bytes, page rounded */
#define BULK_QUEUE_DEPTH   4u        /* per direction, power of two */
#define PAGELIST_STRIDE    32u       /* room for one run entry, padded */
#define PAGELIST_MEM_SIZE  4096u
#define SHARED_MEM_SIZE    (SLOT_MEM_SIZE + FRAG_MEM_SIZE + PAGELIST_MEM_SIZE)

/* Doorbells: DT node 0x7e00b840. Writing BELL2 interrupts the VideoCore. */
#define VCHIQ_BELL_BASE    (PERIPHERAL_BASE + 0xB840u)
#define VCHIQ_BELL2        (*(volatile uint32_t *)(VCHIQ_BELL_BASE + 0x8u))

/* ------------------------------------------------------------------ */
/* Client state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    void *user;
} bulk_pending_t;

typedef struct {
    bulk_pending_t pend[BULK_QUEUE_DEPTH];
    uint32_t head;                   /* next to complete */
    uint32_t tail;                   /* next free */
} bulk_queue_t;

static struct {
    bool inited;
    bool connected;
    volatile vchiq_slot_zero_t *zero;
    volatile vchiq_shared_state_t *local;    /* slave  (us) */
    volatile vchiq_shared_state_t *remote;   /* master (VideoCore) */
    uint8_t *slot_base;              /* ARM view of slot memory */
    uint32_t slot_base_bus;          /* VC view (0xC0000000 alias) */
    uint8_t *pagelist_base;          /* 8 pagelists, PAGELIST_STRIDE apart */
    uint32_t pagelist_base_bus;

    int32_t  rx_pos;                 /* our read position in remote's stream */
    int32_t  tx_pos;                 /* cached copy of local->tx_pos */
    uint32_t slot_queue_available;   /* how many entries of local->slot_queue are usable */

    /* single service */
    uint32_t localport;
    uint32_t remoteport;
    bool     open;
    vchiq_callbacks_t cb;

    bulk_queue_t bulk_tx;
    bulk_queue_t bulk_rx;
} vc;

static uint8_t *slot_ptr(uint32_t index)
{
    return vc.slot_base + index * VCHIQ_SLOT_SIZE;
}

/* Ring the VideoCore's event: set fired, then knock if it is waiting. */
static void remote_event_signal(volatile vchiq_remote_event_t *ev)
{
    _data_memory_barrier();
    ev->fired = 1;
    _data_synchronization_barrier();
    if (ev->armed)
        VCHIQ_BELL2 = 0;
    _data_memory_barrier();
}

uint32_t vchiq_alloc_shared(uint32_t size, uint32_t *handle)
{
    rpi_mailbox_property_t *mp;
    RPI_PropertyStart(TAG_ALLOCATE_MEMORY, 3);
    RPI_PropertyAddTwoWords(size, 4096);
    /* DIRECT (uncached 0xC alias) | ZERO | NO_INIT | HINT_PERMALOCK -
       same flags screen_allocate_buffer() uses */
    RPI_PropertyAdd((1 << 6) + (1 << 5) + (1 << 4) + (1 << 2));
    RPI_PropertyProcess(true);
    if ((mp = RPI_PropertyGet(TAG_ALLOCATE_MEMORY))) {
        *handle = mp->data.buffer_32[0];
        RPI_PropertyStart(TAG_LOCK_MEMORY, 1);
        RPI_PropertyAdd(*handle);
        RPI_PropertyProcess(true);
        if ((mp = RPI_PropertyGet(TAG_LOCK_MEMORY)))
            return mp->data.buffer_32[0] & 0x3FFFFFFFu;
    }
    return 0;
}

bool vchiq_connected(void)
{
    return vc.connected;
}

/* ------------------------------------------------------------------ */
/* TX path                                                            */
/* ------------------------------------------------------------------ */

/* How many TX slot-queue entries are valid. The VideoCore extends this by
   recycling slots we have transmitted: it appends their indexes to OUR
   local->slot_queue and advances local->slot_queue_recycle. The trailing
   barrier matters: the queue ENTRY the VC wrote must not be read (or
   speculated) before the counter that published it - a control dependency
   does not order loads on ARM. */
static void refresh_tx_slots(void)
{
    _data_memory_barrier();
    vc.slot_queue_available = (uint32_t)vc.local->slot_queue_recycle;
    _data_memory_barrier();
}

/* Reserve 'stride' bytes in the TX stream, inserting a padding message and
   skipping to the next slot when the current one cannot hold it. Returns
   NULL when out of slots (transient - the VC recycles them). */
static volatile vchiq_header_t *reserve_space(uint32_t stride)
{
    uint32_t tx_pos = (uint32_t)vc.tx_pos;
    uint32_t slot_space = VCHIQ_SLOT_SIZE - (tx_pos & VCHIQ_SLOT_MASK);

    if (stride > slot_space) {
        /* pad out the current slot */
        volatile vchiq_header_t *pad = (volatile vchiq_header_t *)(uintptr_t)
            (slot_ptr((uint32_t)vc.local->slot_queue[(tx_pos / VCHIQ_SLOT_SIZE) & VCHIQ_SLOT_QUEUE_MASK])
             + (tx_pos & VCHIQ_SLOT_MASK));
        pad->size = slot_space - VCHIQ_HEADER_SIZE;
        _data_memory_barrier();
        pad->msgid = (int32_t)VCHIQ_MAKE_MSG(VCHIQ_MSG_PADDING, 0, 0);
        tx_pos += slot_space;
    }

    /* Starting a fresh slot? Make sure we own one. Both tx_pos and
       slot_queue_available are monotonic mod-2^32 counters, so the test
       must be the wrap-consistent EQUALITY form the Linux driver uses
       ("consumed everything granted"), not an ordered compare - a >=
       would misjudge once tx_pos wraps at 4 GiB of stream (~3 days of
       continuous playback). Mid-slot positions need no check: the slot
       was granted when we entered it. */
    if ((tx_pos & VCHIQ_SLOT_MASK) == 0 &&
        tx_pos == vc.slot_queue_available * VCHIQ_SLOT_SIZE) {
        refresh_tx_slots();
        if (tx_pos == vc.slot_queue_available * VCHIQ_SLOT_SIZE)
            return NULL;             /* all slots in flight; try again later */
    }

    vc.tx_pos = (int32_t)(tx_pos + stride);

    return (volatile vchiq_header_t *)(uintptr_t)
        (slot_ptr((uint32_t)vc.local->slot_queue[(tx_pos / VCHIQ_SLOT_SIZE) & VCHIQ_SLOT_QUEUE_MASK])
         + (tx_pos & VCHIQ_SLOT_MASK));
}

/* Copy a (possibly gathered) message into the stream and kick the VC. */
static bool queue_message_raw(uint32_t msgid,
                              const void *p1, unsigned int s1,
                              const void *p2, unsigned int s2)
{
    uint32_t size = s1 + s2;
    uint32_t stride = CALC_STRIDE(size);
    volatile vchiq_header_t *h;

    if (stride > VCHIQ_SLOT_SIZE)
        return false;

    h = reserve_space(stride);
    if (!h)
        return false;

    /* The slot memory is uncached, so plain memcpy is fine and ordering
       against the tx_pos update below is guaranteed by the barrier. */
    uint8_t *dst = (uint8_t *)(uintptr_t)h + VCHIQ_HEADER_SIZE;
    if (s1) memcpy(dst, p1, s1);
    if (s2) memcpy(dst + s1, p2, s2);
    h->size = size;
    h->msgid = (int32_t)msgid;
    _data_memory_barrier();

    vc.local->tx_pos = vc.tx_pos;    /* publish */
    remote_event_signal(&vc.remote->trigger);
    return true;
}

bool vchiq_queue_message(const void *msg, unsigned int size)
{
    if (!vc.open)
        return false;
    return queue_message_raw(VCHIQ_MAKE_MSG(VCHIQ_MSG_DATA, vc.localport, vc.remoteport),
                             msg, size, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Bulk transfers                                                     */
/* ------------------------------------------------------------------ */

static bool queue_bulk(bulk_queue_t *q, uint32_t pl_index_base, int msg_type,
                       uint32_t pl_type, uint32_t busaddr, unsigned int size,
                       void *user)
{
    if (!vc.open)
        return false;
    if (q->tail - q->head >= BULK_QUEUE_DEPTH)
        return false;

    /* Build a one-run pagelist. The buffer must be physically contiguous,
       which everything from vchiq_alloc_shared() is.

       A pagelist stays owned by the VideoCore until its BULK_*_DONE, so
       each direction gets its own bank of BULK_QUEUE_DEPTH pagelists
       indexed by the queue tail: with at most BULK_QUEUE_DEPTH transfers
       outstanding per direction, a slot cannot be reused before its DONE
       has retired it (and a failed send below leaves tail - and hence
       this slot - unclaimed for the retry). */
    uint32_t idx = pl_index_base + (q->tail & (BULK_QUEUE_DEPTH - 1u));
    volatile vchiq_pagelist_t *pl =
        (volatile vchiq_pagelist_t *)(uintptr_t)(vc.pagelist_base + idx * PAGELIST_STRIDE);
    uint32_t pl_bus = vc.pagelist_base_bus + idx * PAGELIST_STRIDE;

    /* Round to 32 bytes so the VideoCore never needs the fragment
       workaround for partially owned cache lines (and we have no ARM
       cache lines to worry about anyway - the memory is uncached). */
    uint32_t xfer = (size + 31u) & ~31u;
    uint32_t first_page = busaddr & ~0xFFFu;
    uint32_t offset = busaddr & 0xFFFu;
    uint32_t npages = (offset + xfer + 4095u) / 4096u;

    pl->length = xfer;
    pl->type = (uint16_t)pl_type;
    pl->offset = (uint16_t)offset;
    pl->addrs[0] = first_page | (npages - 1u);
    _data_memory_barrier();

    uint32_t payload[2] = { pl_bus, xfer };
    if (!queue_message_raw(VCHIQ_MAKE_MSG((uint32_t)msg_type, vc.localport, vc.remoteport),
                           payload, sizeof(payload), NULL, 0))
        return false;

    q->pend[q->tail & (BULK_QUEUE_DEPTH - 1u)].user = user;
    q->tail++;
    return true;
}

bool vchiq_bulk_transmit(uint32_t busaddr, unsigned int size, void *user)
{
    return queue_bulk(&vc.bulk_tx, 0, VCHIQ_MSG_BULK_TX, PAGELIST_WRITE,
                      busaddr, size, user);
}

bool vchiq_bulk_receive(uint32_t busaddr, unsigned int size, void *user)
{
    return queue_bulk(&vc.bulk_rx, BULK_QUEUE_DEPTH, VCHIQ_MSG_BULK_RX,
                      PAGELIST_READ, busaddr, size, user);
}

bool vchiq_bulk_tx_space(void)
{
    return vc.open && (vc.bulk_tx.tail - vc.bulk_tx.head) < BULK_QUEUE_DEPTH;
}

bool vchiq_bulk_rx_space(void)
{
    return vc.open && (vc.bulk_rx.tail - vc.bulk_rx.head) < BULK_QUEUE_DEPTH;
}

/* ------------------------------------------------------------------ */
/* RX path                                                            */
/* ------------------------------------------------------------------ */

static void bulk_done(bulk_queue_t *q, int actual,
                      void (*fn)(void *user, int actual))
{
    if (q->head == q->tail) {
        LOG_INFO("vchiq: unexpected bulk done\r\n");
        return;
    }
    void *user = q->pend[q->head & (BULK_QUEUE_DEPTH - 1u)].user;
    q->head++;
    if (fn)
        fn(user, actual);
}

/* Give a fully consumed remote slot back to the VideoCore. */
static void recycle_slot(uint32_t slot_index)
{
    volatile vchiq_shared_state_t *remote = vc.remote;
    int32_t r = remote->slot_queue_recycle;
    remote->slot_queue[(uint32_t)r & VCHIQ_SLOT_QUEUE_MASK] = (int32_t)slot_index;
    _data_memory_barrier();
    remote->slot_queue_recycle = r + 1;
    remote_event_signal(&remote->recycle);
}

static void parse_message(volatile vchiq_header_t *h)
{
    uint32_t msgid = (uint32_t)h->msgid;
    uint32_t type = VCHIQ_MSG_TYPE(msgid);
    uint32_t size = h->size;
    const void *payload = (const uint8_t *)(uintptr_t)h + VCHIQ_HEADER_SIZE;

    switch (type) {
    case VCHIQ_MSG_PADDING:
        break;

    case VCHIQ_MSG_CONNECT:
        vc.connected = true;
        break;

    case VCHIQ_MSG_OPENACK:
        if (VCHIQ_MSG_DSTPORT(msgid) == vc.localport) {
            vc.remoteport = VCHIQ_MSG_SRCPORT(msgid);
            vc.open = true;
        }
        break;

    case VCHIQ_MSG_CLOSE:
        LOG_INFO("vchiq: service closed by VC\r\n");
        /* complete the close handshake so the VC side can free the port */
        queue_message_raw(VCHIQ_MAKE_MSG(VCHIQ_MSG_CLOSE, vc.localport,
                                         VCHIQ_MSG_SRCPORT(msgid)),
                          NULL, 0, NULL, 0);
        vc.open = false;
        break;

    case VCHIQ_MSG_DATA:
        if (VCHIQ_MSG_DSTPORT(msgid) == vc.localport && vc.cb.on_data)
            vc.cb.on_data(payload, size);
        break;

    case VCHIQ_MSG_BULK_TX_DONE:
        bulk_done(&vc.bulk_tx, *(const int32_t *)payload, vc.cb.on_bulk_tx_done);
        break;

    case VCHIQ_MSG_BULK_RX_DONE:
        bulk_done(&vc.bulk_rx, *(const int32_t *)payload, vc.cb.on_bulk_rx_done);
        break;

    case VCHIQ_MSG_PAUSE:
        /* Protocol: acknowledge a PAUSE with a PAUSE, then send RESUME
           to carry on - we never want to stay paused. */
        queue_message_raw(VCHIQ_MAKE_MSG(VCHIQ_MSG_PAUSE, 0, 0), NULL, 0, NULL, 0);
        queue_message_raw(VCHIQ_MAKE_MSG(VCHIQ_MSG_RESUME, 0, 0), NULL, 0, NULL, 0);
        break;

    default:
        break;
    }
}

void vchiq_poll(void)
{
    if (!vc.inited)
        return;

    _data_memory_barrier();
    while (vc.rx_pos != vc.remote->tx_pos) {
        uint32_t rx_pos = (uint32_t)vc.rx_pos;
        uint32_t slot_index =
            (uint32_t)vc.remote->slot_queue[(rx_pos / VCHIQ_SLOT_SIZE) & VCHIQ_SLOT_QUEUE_MASK];
        volatile vchiq_header_t *h = (volatile vchiq_header_t *)(uintptr_t)
            (slot_ptr(slot_index) + (rx_pos & VCHIQ_SLOT_MASK));
        _data_memory_barrier();

        uint32_t stride = CALC_STRIDE(h->size);
        if (stride > VCHIQ_SLOT_SIZE - (rx_pos & VCHIQ_SLOT_MASK)) {
            LOG_INFO("vchiq: corrupt rx stream at %"PRIx32"\r\n", rx_pos);
            vc.inited = false;       /* fail safe: stop processing */
            return;
        }

        parse_message(h);

        vc.rx_pos = (int32_t)(rx_pos + stride);
        if ((((uint32_t)vc.rx_pos) & VCHIQ_SLOT_MASK) == 0)
            recycle_slot(slot_index);
        _data_memory_barrier();
    }
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                           */
/* ------------------------------------------------------------------ */

static void poll_for(uint32_t us)
{
    uint32_t start = RPI_GetSystemTime();
    do {
        vchiq_poll();
    } while ((RPI_GetSystemTime() - start) < us);
}

bool vchiq_init(void)
{
    if (vc.inited)
        return true;

    uint32_t handle;
    uint32_t phys = vchiq_alloc_shared(SHARED_MEM_SIZE, &handle);
    if (!phys) {
        LOG_INFO("vchiq: no shared memory\r\n");
        return false;
    }

    memset(&vc, 0, sizeof(vc));
    vc.slot_base = (uint8_t *)(uintptr_t)phys;
    vc.slot_base_bus = vchiq_bus_addr(phys);
    vc.pagelist_base = vc.slot_base + SLOT_MEM_SIZE + FRAG_MEM_SIZE;
    vc.pagelist_base_bus = vc.slot_base_bus + SLOT_MEM_SIZE + FRAG_MEM_SIZE;
    vc.localport = 1;

    /* --- build slot zero ------------------------------------------ */
    memset(vc.slot_base, 0, SHARED_MEM_SIZE);
    volatile vchiq_slot_zero_t *z = (volatile vchiq_slot_zero_t *)(uintptr_t)vc.slot_base;
    vc.zero = z;
    vc.local = &z->slave;
    vc.remote = &z->master;

    z->magic = (int32_t)VCHIQ_MAGIC;
    z->version = VCHIQ_VERSION;
    z->version_min = VCHIQ_VERSION_MIN;
    z->slot_zero_size = (int32_t)sizeof(vchiq_slot_zero_t);
    z->slot_size = (int32_t)VCHIQ_SLOT_SIZE;
    z->max_slots = VCHIQ_MAX_SLOTS;
    z->max_slots_per_side = VCHIQ_MAX_SLOTS_PER_SIDE;

    /* Fragment pool for the VC's bulk cache-line workaround. We always
       hand over 32-byte aligned transfers so it should go unused, but the
       firmware expects it to exist. */
    z->platform_data[0] = (int32_t)(vc.slot_base_bus + SLOT_MEM_SIZE);
    z->platform_data[1] = MAX_FRAGMENTS;

    /* Split the data slots between the two sides, exactly as the Linux
       driver's vchiq_init_slots() does with this memory size. */
    int32_t first_data_slot = 1;          /* slot 0 holds this header */
    int32_t num_slots = (int32_t)NUM_SLOTS - first_data_slot;   /* 32 */
    z->master.slot_sync  = first_data_slot;
    z->master.slot_first = first_data_slot + 1;
    z->master.slot_last  = first_data_slot + (num_slots / 2) - 1;
    z->slave.slot_sync   = first_data_slot + (num_slots / 2);
    z->slave.slot_first  = first_data_slot + (num_slots / 2) + 1;
    z->slave.slot_last   = first_data_slot + num_slots - 1;

    /* Pre-load our TX pool with the slots we own (vchiq_init_state()). */
    uint32_t n = 0;
    for (int32_t i = z->slave.slot_first; i <= z->slave.slot_last; i++)
        z->slave.slot_queue[n++] = i;
    vc.slot_queue_available = n;
    z->slave.slot_queue_recycle = (int32_t)n;
    z->slave.tx_pos = 0;
    _data_memory_barrier();
    z->slave.initialised = 1;
    _data_memory_barrier();

    /* --- tell the firmware ---------------------------------------- */
    rpi_mailbox_property_t *mp;
    RPI_PropertyStart(TAG_VCHIQ_INIT, 1);
    RPI_PropertyAdd(vc.slot_base_bus);
    RPI_PropertyProcess(true);
    mp = RPI_PropertyGet(TAG_VCHIQ_INIT);
    if (!mp || mp->data.buffer_32[0] != 0) {
        LOG_INFO("vchiq: firmware rejected init (start_cd.elf? need full start.elf)\r\n");
        return false;
    }

    vc.inited = true;

    /* --- connect --------------------------------------------------- */
    queue_message_raw(VCHIQ_MAKE_MSG(VCHIQ_MSG_CONNECT, 0, 0), NULL, 0, NULL, 0);
    uint32_t start = RPI_GetSystemTime();
    while (!vc.connected && (RPI_GetSystemTime() - start) < 500000u)
        vchiq_poll();

    if (!vc.connected) {
        LOG_INFO("vchiq: no CONNECT from VideoCore\r\n");
        vc.inited = false;
        return false;
    }

    LOG_DEBUG("vchiq: connected\r\n");
    return true;
}

bool vchiq_open_service(uint32_t fourcc, short version, short version_min,
                        const vchiq_callbacks_t *callbacks)
{
    if (!vc.inited || !vc.connected)
        return false;

    vc.cb = *callbacks;
    vc.open = false;

    vchiq_open_payload_t open = {
        .fourcc = (int32_t)fourcc,
        .client_id = 0,
        .version = version,
        .version_min = version_min,
    };
    if (!queue_message_raw(VCHIQ_MAKE_MSG(VCHIQ_MSG_OPEN, vc.localport, 0),
                           &open, sizeof(open), NULL, 0))
        return false;

    uint32_t start = RPI_GetSystemTime();
    while (!vc.open && (RPI_GetSystemTime() - start) < 500000u)
        vchiq_poll();

    if (!vc.open) {
        LOG_INFO("vchiq: OPEN '%c%c%c%c' not acknowledged\r\n",
                 (char)(fourcc >> 24), (char)(fourcc >> 16),
                 (char)(fourcc >> 8), (char)fourcc);
        return false;
    }

    /* Give the service a moment to settle (matches Linux behaviour of
       processing any immediately queued messages). */
    poll_for(1000);
    return true;
}
