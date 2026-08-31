#ifndef LWIP_ERR_H
#define LWIP_ERR_H
#include "lwip/arch.h"
typedef s8_t err_t;
#define ERR_OK          0
#define ERR_MEM        (-1)
#define ERR_TIMEOUT    (-3)
#define ERR_RTE        (-4)
#define ERR_INPROGRESS (-5)
#define ERR_VAL        (-6)
#define ERR_IF         (-12)
#define ERR_ABRT       (-13)
#define ERR_RST        (-14)
#define ERR_CLSD       (-15)
#endif
