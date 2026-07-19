/* memcpy / memmove / mempcpy for ARM1176 (ARMv6).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The included implementation is 3-clause BSD, (c) Raspberry Pi Foundation and
 * RISC OS Open Ltd -- see arm-mem/README.md.
 *
 * newlib has no ARMv6 memcpy: its fast paths need ARMv7-A or a Thumb-2-only
 * core, so ARM1176 falls through to a 28-byte byte loop. This wrapper pulls in
 * Ben Avison's ARM11-tuned version for that target only.
 *
 * On every other target this file emits nothing, and newlib's memcpy.S (which
 * emits nothing on ARMv6) supplies the NEON version instead. Exactly one of the
 * two ever defines the symbol, so both can sit in a single unconditional file
 * list and there are no per-target CMake conditionals.
 */

#if __ARM_ARCH == 6
#include "arm-mem/memcpymove-v6l.S"
#endif
