#ifndef PI1MHZ_H
#define PI1MHZ_H
#include <stdint.h>
/* the one field smb_client.c reads: nRST edges counted by the IRQ */
typedef struct { uint32_t edges; } Pi1MHz_break_t;
extern volatile Pi1MHz_break_t Pi1MHz_break;
#endif
