/*
    vchiq.h - minimal bare-metal VCHIQ client (ARM side, "slave")

    VCHIQ is the shared-memory message channel between the ARM and the
    VideoCore firmware (start.elf). It is the transport underneath MMAL,
    which is how the hardware H264 decoder is reached.

    This is a deliberately minimal single-service, polled implementation:

      - no interrupts: the caller pumps vchiq_poll() from a Pi1MHz poll task.
        Remote events are signalled by writing the shared "fired" flags and
        ringing doorbell 2 (ARM -> VC); incoming work is discovered by
        comparing the shared tx_pos/recycle counters, so the VC -> ARM
        doorbell interrupt is never enabled.
      - one service only (MMAL uses a single 'mmal' service).
      - all shared memory (slot memory, pagelists, fragments) lives in the
        VideoCore heap, allocated with the mailbox ALLOCATE_MEMORY tag.
        The ARM maps that region effectively uncached (see cache.c: the
        arm_mem..PERIPHERAL_BASE range gets descriptor 0x11C06), so no ARM
        cache maintenance is needed for any buffer handed to VCHIQ -
        as long as callers also place their bulk payload buffers there.

    Wire format (slot layout, message ids, pagelists) matches the Linux
    vchiq driver (drivers/staging/vc04_services, GPL-2.0 OR BSD-3-Clause)
    protocol version 8, which is what every start.elf since ~2012 speaks.

    Requires the FULL start.elf + fixup.dat (not start_cd.elf) and
    gpu_mem=64 (or more) in config.txt. vchiq_init() fails cleanly - and
    the video player falls back to still-frame mode - when the firmware
    does not support VCHIQ.
*/

#ifndef RPI_VCHIQ_H
#define RPI_VCHIQ_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Make a VCHIQ fourcc; note byte order (first char is the high byte) */
#define VCHIQ_FOURCC(a,b,c,d) \
    ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                ((uint32_t)(c) << 8)  |  (uint32_t)(d)))

/* Callbacks delivered from vchiq_poll(). All run in poll context. */
typedef struct {
    /* A DATA message for our service. 'data'/'size' point INTO the shared
       rx slot and are only valid for the duration of the call - copy out
       anything needed later. */
    void (*on_data)(const void *data, unsigned int size);
    /* A queued bulk transfer completed ('actual' = bytes transferred,
       'user' = the tag passed to the queue call). */
    void (*on_bulk_tx_done)(void *user, int actual);
    void (*on_bulk_rx_done)(void *user, int actual);
} vchiq_callbacks_t;

/* Bring the channel up: allocate shared memory, tell the firmware where it
   is (mailbox tag 0x48010), exchange CONNECT. Returns false if the
   firmware has no VCHIQ (e.g. start_cd.elf) - safe to call anyway. */
bool vchiq_init(void);

/* Open a service, e.g. VCHIQ_FOURCC('m','m','a','l'). Version pair is the
   service protocol version (MMAL: 16/10). Returns false on timeout/reject. */
bool vchiq_open_service(uint32_t fourcc, short version, short version_min,
                        const vchiq_callbacks_t *callbacks);

/* Queue a DATA message on the open service. Copies into the shared slot,
   so the buffer may live in ordinary cached ARM memory. Returns false if
   no TX slot space is available (caller retries). */
bool vchiq_queue_message(const void *msg, unsigned int size);

/* Queue a bulk transfer. 'busaddr' is a VideoCore bus address
   (0xC0000000-alias) of a buffer INSIDE the VC heap region (uncached from
   the ARM's point of view) - see vchiq_alloc_shared(). Size is rounded up
   internally to a 32-byte multiple, so keep buffers 32-byte aligned and
   allow for the roundup. 'user' is returned in the done callback.
   Transfers of each direction complete strictly in queue order. */
bool vchiq_bulk_transmit(uint32_t busaddr, unsigned int size, void *user);
bool vchiq_bulk_receive(uint32_t busaddr, unsigned int size, void *user);

/* True when another bulk of that direction can be queued right now.
   Callers that must pair a control message with a bulk transfer check
   this BEFORE sending the message, so the pair can never be split. */
bool vchiq_bulk_tx_space(void);
bool vchiq_bulk_rx_space(void);

/* Pump the channel: parse received messages (dispatching callbacks),
   recycle consumed slots, replenish TX slots. Call frequently. */
void vchiq_poll(void);

/* Allocate a block from the VideoCore heap for use as a bulk buffer.
   Returns the ARM-visible physical address (== bus address & 0x3FFFFFFF)
   or 0 on failure. The block is 4K aligned and permanently locked.
   vchiq_bus_addr() converts to the address VCHIQ wants. */
uint32_t vchiq_alloc_shared(uint32_t size, uint32_t *handle);
#define vchiq_bus_addr(phys) ((uint32_t)(phys) | 0xC0000000u)

/* True once CONNECT has been exchanged with the firmware. */
bool vchiq_connected(void);

#endif /* RPI_VCHIQ_H */
