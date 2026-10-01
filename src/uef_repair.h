#ifndef UEF_REPAIR_H
#define UEF_REPAIR_H

#include <stddef.h>
#include <stdint.h>

/* Redirect the published Electron loader idiom which stamps the FILEV vector
 * blind. 84 of a 728-title Electron UEF corpus contain `?&212=` and 76 of
 * them `?&212=&D6:?&213=&F1`, which
 * overwrites whatever filing system owns the vector - including WiCFS - with
 * the Electron MOS 1.00 cassette entry. Rewriting the address token to
 * &900/&901 leaves the program the same length, so block layout and every
 * stored offset are untouched and only the affected block's data CRC is
 * recomputed. Returns the number of address tokens redirected. */
unsigned uef_repair_filev_stamp(uint8_t *window, size_t length);

/* Bytes of UEF file header - "UEF File!", its terminator and the two version
 * bytes - that precede the first chunk. */
#define UEF_REPAIR_HEADER 12u

/* The same repair, driven a window at a time, for a caller that never holds
 * the whole tape: the Pi streams a tape to the Beeb in 63 KB windows and a
 * cassette block can straddle one. Repairs every COMPLETE chunk from `start`
 * and returns the offset just past the last of them, so the caller can carry
 * the remainder into the next window. Holding the remainder back is not an
 * optimisation: the repair rewrites a block's payload and then the payload
 * CRC that follows it, so a block split across two windows cannot be
 * repaired from either half on its own.
 *
 * `start` is UEF_REPAIR_HEADER for the first window and 0 afterwards, because
 * every window this returns ends on a chunk boundary. `repaired`, when not
 * NULL, receives the number of address tokens redirected. */
size_t uef_repair_filev_span(uint8_t *window, size_t length, size_t start,
                             unsigned *repaired);

#endif
