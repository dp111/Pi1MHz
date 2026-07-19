/* memcmp for ARM1176 (ARMv6).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The included implementation is 3-clause BSD, (c) Raspberry Pi Foundation and
 * RISC OS Open Ltd -- see arm-mem/README.md.
 *
 * newlib has no ARM assembly memcmp for any target, so both builds otherwise
 * get the generic C byte loop (56 bytes on rpi, 32 on rpi3) for ~126 call sites.
 *
 * Only the ARMv6 variant is vendored, so this covers the rpi target; rpi3 keeps
 * the generic C version until someone vendors memcmp-v7l.S and measures it.
 */

#if __ARM_ARCH == 6
#include "arm-mem/memcmp-v6l.S"
#endif
