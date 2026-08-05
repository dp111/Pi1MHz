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
      - messages only: payloads never cross the wire, so there is no bulk
        DMA machinery here. The MMAL client runs its ports in zero-copy
        mode, where a buffer is identified by a VideoCore memory handle
        and the codec reads and writes that memory itself.
      - all shared memory (slot memory, fragment pool) lives in the
        VideoCore heap, allocated with the mailbox ALLOCATE_MEMORY tag.
        The ARM maps that region effectively uncached (see cache.c: the
        arm_mem..PERIPHERAL_BASE range gets descriptor 0x11C06), so no ARM
        cache maintenance is needed for any buffer handed to VCHIQ.

    Wire format (slot layout, message ids) matches the Linux vchiq driver
    (drivers/staging/vc04_services, GPL-2.0 OR BSD-3-Clause) protocol
    version 8, which is what every start.elf since ~2012 speaks.

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
} vchiq_callbacks_t;

/* Bring the channel up: allocate shared memory, tell the firmware where it
   is (mailbox tag 0x48010), exchange CONNECT. Returns false if the
   firmware has no VCHIQ (e.g. start_cd.elf) - safe to call anyway. */
bool vchiq_init(void);

/* How many services may be open at once (MMAL + SMEM). */
#define VCHIQ_MAX_SERVICES 2

/* Open a service, e.g. VCHIQ_FOURCC('m','m','a','l'). Version pair is the
   service protocol version (MMAL: 16/10). Returns a service id (>= 0) to
   pass to the calls below, or -1 on timeout/reject. */
int vchiq_open_service(uint32_t fourcc, short version, short version_min,
                       const vchiq_callbacks_t *callbacks);

/* Queue a DATA message on a service. Copies into the shared slot, so the
   buffer may live in ordinary cached ARM memory. Returns false if no TX
   slot space is available (caller retries). */
bool vchiq_queue_message(int service, const void *msg, unsigned int size);

/* Pump the channel: parse received messages (dispatching callbacks),
   recycle consumed slots, replenish TX slots. Call frequently. */
void vchiq_poll(void);

/* Allocate a block from the VideoCore heap for memory shared with the
   VideoCore. Returns the ARM-visible physical address (== bus address & 0x3FFFFFFF)
   or 0 on failure. The block is 4K aligned and permanently locked.
   vchiq_bus_addr() converts to the address VCHIQ wants. */
uint32_t vchiq_alloc_shared(uint32_t size, uint32_t *handle);
/* Give a block from vchiq_alloc_shared() back to the GPU heap (unlock,
   then release). Only ever safe for memory the VideoCore was not told
   about. No-op for handle 0. */
void vchiq_free_shared(uint32_t handle);
/* On BCM2835 the ARM's memory port is routed through the VideoCore L2, so
   the VC must be given the 0x4 (L2-coherent) alias or the two sides see
   torn views of the shared slots (Linux's dma-ranges and Circle's
   GPU_MEM_BASE make the same choice: 0x4 on Pi 1/Zero, 0xC on Pi 2/3,
   where the ARM bypasses the VC L2). */
#if (__ARM_ARCH >= 7)
#define vchiq_bus_addr(phys) ((uint32_t)(phys) | 0xC0000000u)
#else
#define vchiq_bus_addr(phys) ((uint32_t)(phys) | 0x40000000u)
#endif

/* True once CONNECT has been exchanged with the firmware. */
bool vchiq_connected(void);

#endif /* RPI_VCHIQ_H */
