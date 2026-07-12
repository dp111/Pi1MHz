#ifndef VICE_RESOURCES_H
#define VICE_RESOURCES_H

#include <string.h>

/* SidFilters=1 (the 6581 filter is the signature SID sound), SidModel=0 (6581). */
static inline int resources_get_int(const char *name, int *value)
{
    if (strcmp(name, "SidFilters") == 0) {
        *value = 1;
        return 0;
    }
    if (strcmp(name, "SidModel") == 0) {
        *value = 0; /* 6581 */
        return 0;
    }
    return -1;
}

#endif
