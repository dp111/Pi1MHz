#ifndef LWIP_ERR_H
#define LWIP_ERR_H
#include <stdint.h>
typedef int8_t err_t;
#define ERR_OK          0
#define ERR_INPROGRESS (-5)
#define ERR_ARG        (-16)
#endif
