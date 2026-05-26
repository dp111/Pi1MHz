#ifndef ARMC_CSTUBS_H
#define ARMC_CSTUBS_H

#include <sys/stat.h>
#include <sys/times.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void arm_setup_heap_limit(void *limit);

#ifdef __cplusplus
}
#endif

#endif // ARMC_CSTUBS_H
