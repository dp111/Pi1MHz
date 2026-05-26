// startup.h

#ifndef STARTUP_H
#define STARTUP_H

/* Found in the *start.S file, implemented in assembler */

#define _enable_interrupts() {asm volatile ("CPSIE if");}

extern void _set_interrupts( int cpsr );

#define _disable_interrupts() {asm volatile ("CPSID if");}

#if defined(RPI2) || defined(RPI3)
    #define _data_memory_barrier() {asm volatile ("dmb");}
#else
    extern void _data_memory_barrier();
#endif

extern unsigned int _get_cpsr();

extern unsigned int _get_stack_pointer();

extern void _enable_unaligned_access();

#endif
