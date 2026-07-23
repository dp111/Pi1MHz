/* ASCII-only toupper/tolower, transparently replacing newlib's table lookups.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This header is force-included into every translation unit by CMakeLists.txt
 * (-include). Source files keep their ordinary `#include <ctype.h>` and their
 * ordinary toupper()/tolower() calls -- nothing at the call sites changes.
 *
 * Why: newlib's toupper() is a lookup in the 257-byte `_ctype_` table reached
 * through a locale pointer, which inlines to
 *
 *      ldr  r3, .L4          @ &_ctype_+1
 *      add  r2, r0, #32
 *      ldrb r3, [r3, r0]
 *      and  r3, r3, #3
 *      cmp  r3, #1
 *      andeq r0, r2, #255
 *
 * plus the table and a literal-pool word. The versions below compile to three
 * instructions and no data:
 *
 *      sub   r3, r0, #97
 *      cmp   r3, #25
 *      andls r0, r0, #223
 *
 * The scratch register is free -- r3 is call-clobbered, so there is no spill.
 * Avoiding it would mean restoring r0 on both paths, which costs an extra
 * instruction to save a register that was not scarce.
 *
 * NOTE the mask asymmetry: `| 0x20` (tolower) leaves digits and most
 * punctuation untouched, but `& 0xDF` (toupper) does NOT -- '3' & 0xDF is 0x13.
 * That is exactly why both functions below range-check first rather than
 * masking unconditionally. Do not "simplify" them into a bare AND/OR.
 *
 * Semantics are identical to newlib in the C locale for every int input,
 * including EOF (-1) and the 128..255 range, both of which fall outside the
 * range checks and are returned unchanged. Pi1MHz never sets a locale, so this
 * is a pure implementation swap, not a behaviour change -- which is why it is
 * safe to apply globally, including to lwip/tinyusb/FatFs.
 *
 * Only toupper/tolower are overridden. isalpha(), isdigit(), isspace() and the
 * rest still use `_ctype_`, so the table remains linked until those are dealt
 * with separately.
 */

#ifndef PI1MHZ_ASCII_CTYPE_H
#define PI1MHZ_ASCII_CTYPE_H

#include <ctype.h>

/* always_inline: at -Os / in LTO-cold callers GCC's heuristics decline to
   inline these (tripping -Winline), but the 3-instruction body is no larger
   than a call, and emitting it inline is the entire point of this header. */
static inline __attribute__((always_inline)) int pi1mhz_ascii_toupper(int c)
{
   return ((unsigned)c - (unsigned)'a') < 26u ? (c & 0xDF) : c;
}

static inline __attribute__((always_inline)) int pi1mhz_ascii_tolower(int c)
{
   return ((unsigned)c - (unsigned)'A') < 26u ? (c | 0x20) : c;
}

/* The classification predicates. These are the eight actually used across the
   tree (ours and vendored); isalnum/ispunct/isgraph/isblank/isascii are unused,
   so they are deliberately left on newlib's implementation.

   All are inline FUNCTIONS, not multi-evaluating macros -- several need `c`
   twice, and call sites like isdigit(*p++) must stay correct. */

static inline int pi1mhz_ascii_isdigit(int c)
{
   return ((unsigned)c - (unsigned)'0') < 10u;
}

static inline int pi1mhz_ascii_isupper(int c)
{
   return ((unsigned)c - (unsigned)'A') < 26u;
}

static inline int pi1mhz_ascii_islower(int c)
{
   return ((unsigned)c - (unsigned)'a') < 26u;
}

/* Setting bit 5 folds A-Z onto a-z, so one range check covers both cases. */
static inline int pi1mhz_ascii_isalpha(int c)
{
   return ((unsigned)(c | 0x20) - (unsigned)'a') < 26u;
}

static inline int pi1mhz_ascii_isxdigit(int c)
{
   return pi1mhz_ascii_isdigit(c) || (((unsigned)(c | 0x20) - (unsigned)'a') < 6u);
}

/* \t \n \v \f \r are 9..13 -- one contiguous range -- plus space. */
static inline int pi1mhz_ascii_isspace(int c)
{
   return c == ' ' || (((unsigned)c - (unsigned)'\t') < 5u);
}

static inline int pi1mhz_ascii_isprint(int c)
{
   return ((unsigned)c - 0x20u) < 0x5Fu;          /* 0x20..0x7E */
}

static inline int pi1mhz_ascii_iscntrl(int c)
{
   return ((unsigned)c < 0x20u) || c == 0x7F;
}

/* newlib defines these as macros, so they must be undefined before ours can
   take effect. Undefining an unset macro is legal, so no guards needed. */
#undef toupper
#undef tolower
#undef isdigit
#undef isupper
#undef islower
#undef isalpha
#undef isxdigit
#undef isspace
#undef isprint
#undef iscntrl

#define toupper(c)  pi1mhz_ascii_toupper(c)
#define tolower(c)  pi1mhz_ascii_tolower(c)
#define isdigit(c)  pi1mhz_ascii_isdigit(c)
#define isupper(c)  pi1mhz_ascii_isupper(c)
#define islower(c)  pi1mhz_ascii_islower(c)
#define isalpha(c)  pi1mhz_ascii_isalpha(c)
#define isxdigit(c) pi1mhz_ascii_isxdigit(c)
#define isspace(c)  pi1mhz_ascii_isspace(c)
#define isprint(c)  pi1mhz_ascii_isprint(c)
#define iscntrl(c)  pi1mhz_ascii_iscntrl(c)

#endif /* PI1MHZ_ASCII_CTYPE_H */
