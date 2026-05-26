unsigned int _get_cpsr(void)
{
    unsigned int result;
    __asm volatile
    (
        "mrs     %[result], cpsr \r\n"
    :
        [result] "=r" (result)
    :
    :
    );
return result;
}
// cppcheck-suppress unusedFunction
unsigned int _get_stack_pointer(void)
{
    unsigned int result;
    __asm volatile
    (
        "mov     %[result], sp \r\n"
    :
        [result] "=r" (result)
    :
    :
    );
return result;
}
// cppcheck-suppress unusedFunction
void _enable_interrupts(void)
{
    __asm volatile
    (
        "CPSIE if \r\n"
    :
    :
    :
    );
}
// cppcheck-suppress unusedFunction
void _disable_interrupts(void)
{
    __asm volatile
    (
        "CPSID if \r\n"
    :
    :
    :
    );
}

unsigned int _disable_interrupts_cspr(void)
{
    unsigned int result;
    __asm volatile
    (
        "mrs     %[result], cpsr \r\n"
        "CPSID if \r\n"
    :
        [result] "=r" (result)
    :
    :
    );
    return result;
}

#define  CPSR_IRQ_INHIBIT       0x80
#define  CPSR_FIQ_INHIBIT       0x40

void _set_interrupts(unsigned int cpsr)
{
    cpsr = cpsr & (CPSR_IRQ_INHIBIT | CPSR_FIQ_INHIBIT );
    cpsr = cpsr | (_get_cpsr()& (unsigned int)~(CPSR_IRQ_INHIBIT | CPSR_FIQ_INHIBIT ));
    __asm volatile
    (
        "msr cpsr_c,%[cpsr]"
    :
    :
        [cpsr] "r" (cpsr)
    :
    );
}
// cppcheck-suppress unusedFunction
void _restore_cpsr(unsigned int cpsr)
{
    __asm volatile
    (
        "msr cpsr_c,%[cpsr]"
    :
    :
        [cpsr] "r" (cpsr)
    :
    );
}

void _data_memory_barrier(void)
{
#if (__ARM_ARCH >= 7 )
    __asm volatile
    (
        "dmb \r\n"
    :
    :
    :
    );
#else
    __asm volatile ("mcr p15, 0, %0, c7, c10, 5" :: "r" (0));
#endif
}
// cppcheck-suppress unusedFunction
void _invalidate_icache(void)
{
    __asm volatile ("mcr p15, 0, %0, c7, c5, 0" :: "r" (0));
}

// cppcheck-suppress unusedFunction
void _data_synchronization_barrier(void)
{
#if (__ARM_ARCH >= 7 )
    __asm volatile
    (
        "dsb \r\n"
    :
    :
    :
    );
#else
    __asm volatile ("mcr p15, 0, %0, c7, c10, 4" :: "r" (0));
#endif
}
// cppcheck-suppress unusedFunction
void _invalidate_tlb_mva(const void *address)
 {
    __asm volatile
    (

#if (__ARM_ARCH >= 7 )
    "dsb \r\n"
#endif

    "mcr     p15, 0, %[address], c8, c7, 1 \r\n"  // invalidate tlb

#if (__ARM_ARCH >= 7 )
    "MCR p15, 0, %[address], c7, c5, 6 \r\n" // invalidate all of the BTB (BPIALL)
    "dsb \r\n"
    "isb \r\n"
#endif
    :
    :
        [address] "r" (address)
    : "memory"
    );
}

#if (__ARM_ARCH >= 7 )
unsigned int _get_core(void)
{
unsigned int result;
    __asm volatile
    (
        "mrc     p15, 0, %[result], c0, c0, 5"
    :
        [result] "=r" (result)
    :
    :
    );
return result & 0x3;
}
#endif
