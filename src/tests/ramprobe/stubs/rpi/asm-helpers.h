#pragma once
/* Host test: one thread; the "FIQ" callbacks are called directly. */
static inline unsigned int _disable_interrupts_cspr(void) { return 0; }
static inline void _set_interrupts(unsigned int cpsr) { (void)cpsr; }
