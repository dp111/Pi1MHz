/* Minimal sound.h — only what FastSID needs. */
#ifndef VICE_SOUND_H
#define VICE_SOUND_H

#include "types.h"

typedef struct sound_s sound_t;

/* Fractional sample position within current buffer; unused for our render path. */
static inline long sound_sample_position(void)
{
    return 0;
}

#endif
