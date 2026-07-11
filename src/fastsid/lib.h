/* Minimal lib_* shims used by FastSID. */
#ifndef VICE_LIB_H
#define VICE_LIB_H

#include <stdlib.h>
#include <string.h>

static inline void *lib_calloc(size_t n, size_t sz)
{
    return calloc(n, sz);
}

static inline void lib_free(void *p)
{
    free(p);
}

static inline char *lib_stralloc(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

#endif
