#include <stdint.h>
#include <stdio.h>
#include "rpi.h"

/* define the stack space which is setup in arm-start.s */
__attribute__ ((aligned (0x1000) )) __attribute__((used))  NOINIT_SECTION uint8_t arm_stack[8*64*1024];

#if (__ARM_ARCH >= 7 )
int _get_core(void)
{
   int core;
   asm volatile ("mrc p15, 0, %0, c0, c0,  5" : "=r" (core));
   return core & 3;
}

void start_core(int core, func_ptr func) {
   LOG_DEBUG("starting core %d\r\n", core);
   *(unsigned int *)(0x4000008C + 0x10 * core) = (unsigned int) func;
}
#endif
