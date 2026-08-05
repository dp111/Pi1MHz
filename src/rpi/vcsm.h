/*
    vcsm.h - VideoCore shared-memory ("SMEM") VCHIQ service client

    MMAL's zero-copy mode does not take a raw address: the buffer header
    carries a VideoCore memory HANDLE, and the firmware only accepts
    handles minted by this service (mailbox TAG_ALLOCATE_MEMORY handles
    are rejected - hardware-observed, the buffers come straight back with
    MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED).

    We do not want the VideoCore to allocate the frames for us: our output
    buffers double as the HVS scan-out planes, so their address and the
    Y/Cb/Cr plane layout inside them must stay ours to choose. So instead
    of ALLOC we use IMPORT, which registers memory we already own and
    hands back a handle - the display path is completely untouched.

    Protocol mirrors Linux drivers/staging/vc04_services/vc-sm-cma
    (vc_sm_defs.h), service fourcc 'SMEM', plain messages, no bulk.
*/

#ifndef RPI_VCSM_H
#define RPI_VCSM_H

#include <stdint.h>
#include <stdbool.h>

/* Open the SMEM service and announce our protocol version. Safe to call
   more than once; false if the firmware has no such service. */
bool vcsm_init(void);

/* Register memory we already own with the VideoCore and return the handle
   MMAL zero-copy wants, or 0 on failure. 'busaddr' is the VC-alias
   address (vchiq_bus_addr()) of a physically contiguous, page-aligned
   block - exactly what vchiq_alloc_shared()/screen_allocate_buffer()
   produce. The registration lasts until vcsm_free(). */
uint32_t vcsm_import(uint32_t busaddr, uint32_t size, const char *name);

/* Release a handle from vcsm_import(). The memory itself is still ours;
   this only drops the VideoCore's registration of it. */
void vcsm_free(uint32_t handle);

#endif /* RPI_VCSM_H */
