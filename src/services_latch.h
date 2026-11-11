#ifndef SERVICES_LATCH_H
#define SERVICES_LATCH_H

/* One-slot command mailbox for a services-port handler: the FIQ latches the
   command, the service's own poll takes it on the main loop and does the
   work there.  Each service keeps its own static instance.

   post() runs in FIQ and stores the payload before the flag, in that order
   (all fields volatile), so a poll that sees the flag set reads a whole
   command.  take() reads the flag, copies the payload and clears the flag
   before the caller dispatches, so a command latched during that dispatch is
   kept for the next pass.  A command posted while one is still pending
   replaces it: the Beeb waits for each result, so the newer caller is the
   live one.

   Only for services whose FIQ side is exactly this; secure_service.c and
   fujibus_service.c take under masked interrupts and publish conditionally,
   and keep their own latches. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
   volatile uint32_t cp;        /* command block (JIM offset)         */
   volatile uint32_t addr;      /* FRED result register               */
   volatile uint8_t  data;      /* raw command-register value         */
   volatile bool     pending;   /* set last by post, cleared by take  */
} service_latch_t;

/* FIQ context. */
static inline void service_latch_post(service_latch_t *l, uint32_t cp,
                                      uint32_t addr, uint8_t data)
{
   l->cp      = cp;
   l->addr    = addr;
   l->data    = data;
   l->pending = true;
}

/* Main loop: is a command waiting? */
static inline bool service_latch_pending(const service_latch_t *l)
{
   return l->pending;
}

/* Main loop, after service_latch_pending() said yes: copy the command out
   and free the slot, before the caller dispatches it. */
static inline void service_latch_take(service_latch_t *l, uint32_t *cp,
                                      uint32_t *addr, uint8_t *data)
{
   *cp   = l->cp;
   *addr = l->addr;
   *data = l->data;
   l->pending = false;
}

#endif
