/* startup.h */

#ifndef STARTUP_H
#define STARTUP_H

/* Found in the *start.S file, implemented in assembler */

#define _enable_interrupts() {asm volatile ("CPSIE if");}

#define _disable_interrupts() {asm volatile ("CPSID if");}

#if (__ARM_ARCH >= 7 )
    #define _data_memory_barrier() {asm volatile ("dmb");}
#else
    extern void _data_memory_barrier();
#endif

extern unsigned int _get_cpsr();

#endif
