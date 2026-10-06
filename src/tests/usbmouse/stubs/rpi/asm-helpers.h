/* Host stub: no interrupts to mask here. */
#ifndef STUB_ASM_HELPERS_H
#define STUB_ASM_HELPERS_H
static inline unsigned int _disable_interrupts_cspr(void) { return 0u; }
static inline void _restore_cpsr(unsigned int cpsr) { (void)cpsr; }
#endif
