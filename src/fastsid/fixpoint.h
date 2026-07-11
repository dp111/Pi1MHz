/* Float path (no FIXPOINT_ARITHMETIC) — from VICE fixpoint.h. */
#ifndef VICE_FIXPOINT_H
#define VICE_FIXPOINT_H

typedef float vreal_t;
#define REAL_VALUE(x)   (x)
#define REAL_MULT(x, y) ((x) * (y))
#define REAL_TO_INT(x)  ((int)(x))

typedef double soundclk_t;
#define SOUNDCLK_CONSTANT(x)    ((soundclk_t)(x))
#define SOUNDCLK_MULT(a, b)     ((a) * (b))
#define SOUNDCLK_LONG(a)        ((long)(a))
#define SOUNDCLK_LONG_RAW(a)    ((long)(a))

#define BIG_FLOAT_TO_INT(f)     (f)
#define BIG_FLOAT_TO_UINT(f)    (f)

#endif
