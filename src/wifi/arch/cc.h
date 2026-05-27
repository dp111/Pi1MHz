#ifndef PI1MHZ_LWIP_ARCH_CC_H
#define PI1MHZ_LWIP_ARCH_CC_H

#include <stdint.h>

#include "../../rpi/exceptions.h"
#include "../../rpi/rpi.h"
#include "../../rpi/systimer.h"

typedef int sys_prot_t;

#define BYTE_ORDER LITTLE_ENDIAN
/* lwIP uses LWIP_RAND() for DHCP transaction IDs and TCP initial sequence
   numbers, both of which break (or become hijackable) when constant.  Mix
   the free-running 1 MHz system timer with a fixed golden-ratio
   multiplier so callers across a single boot get distinct values and
   the per-boot starting point is non-deterministic.  This is not
   cryptographically strong - lwIP's API does not require it to be - but
   it removes the predictable-ISN attack and the DHCP-XID collision. */
#define LWIP_RAND() ((uint32_t)(RPI_GetSystemTime() * 2654435761u))

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

/* lwIP only invokes this on a hard internal invariant failure.  The
   previous spin-forever swallowed the diagnostic and froze the Pi
   silently - on a board with no JTAG you'd never know why.  Print
   the failing assertion to whatever LOG_INFO is wired to (usually
   the aux UART) and reboot so the system recovers automatically.

   LWIP_ASSERT's contract (lwip/src/include/lwip/debug.h) is
       do { if (!(assertion)) { LWIP_PLATFORM_ASSERT(message); }} while(0)
   so by the time control reaches this macro the assertion has already
   failed and 'x' is the message *string literal* (never a boolean).  An
   earlier version of this override wrapped the body in 'if (!(x))',
   which evaluated to '!(address-of-string-literal)' - always 0 - and
   silently swallowed every assertion failure.  The body must fire
   unconditionally, and 'x' must be passed straight to %s (no '#x'
   stringification - that would add literal quotes around the message). */
#define LWIP_PLATFORM_ASSERT(x) \
   do { \
      LOG_INFO("lwIP assert: %s (%s:%d)\r\n", (x), __FILE__, __LINE__); \
      reboot_now(); \
   } while (0)

#endif
