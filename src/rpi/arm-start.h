/* arm-start.h */

#ifndef STARTUP_H
#define STARTUP_H

/* Interrupt and barrier helpers: inline asm here, except the ARMv6
   _data_memory_barrier and _get_cpsr, which are in asm-helpers.c */

#define _enable_interrupts() {__asm volatile ("CPSIE if" ::: "memory");}

#define _disable_interrupts() {__asm volatile ("CPSID if" ::: "memory");}

#if (__ARM_ARCH >= 7 )
    #define _data_memory_barrier() {asm volatile ("dmb" ::: "memory");}
#else
    extern void _data_memory_barrier(void);
#endif

extern unsigned int _get_cpsr(void);

#endif
