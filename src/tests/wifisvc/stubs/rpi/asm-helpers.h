#pragma once
/* Host-test stub: interrupt masking is a no-op on the host. */
static inline unsigned int _disable_interrupts_cspr(void) { return 0; }
static inline void _set_interrupts(unsigned int cpsr) { (void)cpsr; }
static inline void _restore_cpsr(unsigned int cpsr) { (void)cpsr; }
