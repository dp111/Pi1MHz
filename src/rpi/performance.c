#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "performance.h"

#if (__ARM_ARCH == 8 )

const char * type_names[] = {

   "SW_INCR",
   "L1I_CACHE_REFILL",
   "L1I_TLB_REFILL",
   "L1D_CACHE_REFILL",
   "L1D_CACHE",
   "L1D_TLB_REFILL",
   "LD_RETIRED",
   "ST_RETIRED",
   "INST_RETIRED",
   "EXC_TAKEN",
   "EXC_RETURN",
   "CID_WRITE_RETIRED",
   "PC_WRITE_RETIRED",
   "BR_IMM_RETIRED",
   "BR_RETURN_RETIRED",
   "UNALIGNED_LDST_RETIRED",
   "BR_MIS_PRED",
   "CPU_CYCLES",
   "BR_PRED",
   "MEM_ACCESS",
   "L1I_CACHE",
   "L1D_CACHE_WB",
   "L2D_CACHE",
   "L2D_CACHE_REFILL",
   "L2D_CACHE_WB",
   "BUS_ACCESS",
   "MEMORY_ERROR",
   "INST_SPEC",
   "TTRB_WRITE_RETIRED",
   "BUS_CYCLES",
   "CHAIN",
   "L1D_CACHE_ALLOCATE"
};

#elif (__ARM_ARCH == 7 )

const char * type_names[] = {
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO",
   "TODO"
};

#else

const char * type_names[] = {
   "I_CACHE_MISS",
   "IBUF_STALL",
   "DATA_DEP_STALL",
   "I_MICROTLB_MISS",
   "D_MICROTLB_MISS",
   "BRANCH_EXECUTED",
   "BRANCH_PRED_INCORRECT",
   "INSTRUCTION_EXECUTED",
   "UNDEFINED",
   "D_CACHE_ACCESS_CACHEABLE",
   "D_CACHE_ACCESS",
   "D_CACHE_MISS",
   "D_CACHE_WRITEBACK",
   "SOFTWARE_CHANGED_PC",
   "MAINTLB_MISS",
   "EXPLICIT_DATA_ACCESS",
   "FULL_LOAD_STORE_REQ_QUEUE",
   "WRITE_BUFF_DRAINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "UNDEFINED",
   "EXT0",
   "EXT1",
   "EXT0_AND_EXT1",
   "PROC_RETURN_PUSHED",
   "PROC_RETURN_POPPED",
   "PROC_RETURN_PRED_CORRECT",
   "PROC_RETURN_PRED_INCORRECT"
};

#endif

const char *type_lookup(unsigned int type) {
   unsigned int num_types = sizeof(type_names) / sizeof(type_names[0]);
   if (type < num_types) {
      return type_names[type];
   } else {
      static const char *UNKNOWN = "UNKNOWN";
      return UNKNOWN;
   }
}


/* Set control register and zero counters */
void reset_performance_counters(perf_counters_t *pct) {
   /* bit 3 = 1 means count every 64th processor cycle
      bit 2 = 1 means reset cycle counter to zero
      bit 1 = 1 means reset counters to zero
      bit 0 = 1 enable counters */
   unsigned ctrl = 0x0F;

  /* Initialize performance counters */
#if (__ARM_ARCH >= 7 )
   pct->num_counters = MAX_COUNTERS;
   pct->type[0] = PERF_TYPE_L1I_CACHE;
   pct->type[1] = PERF_TYPE_L1I_CACHE_REFILL;
   pct->type[2] = PERF_TYPE_L1D_CACHE;
   pct->type[3] = PERF_TYPE_L1D_CACHE_REFILL;
   pct->type[4] = PERF_TYPE_L2D_CACHE_REFILL;
   pct->type[5] = PERF_TYPE_INST_RETIRED;
   pct->counter[0] = 0;
   pct->counter[1] = 0;
   pct->counter[2] = 0;
   pct->counter[3] = 0;
   pct->counter[4] = 0;
   pct->counter[5] = 0;
#else
   pct->num_counters = MAX_COUNTERS;
   pct->type[0] = PERF_TYPE_I_CACHE_MISS;
   pct->type[1] = PERF_TYPE_D_CACHE_MISS;
   pct->counter[0] = 0;
   pct->counter[1] = 0;
#endif


#if (__ARM_ARCH >= 7 )
   unsigned cntenset = (1U << 31);

   unsigned type_impl;

   /* Read the common event identification register
      to see test whether the requested event is implemented */
   __asm volatile ("mrc p15,0,%0,c9,c12,6" : "=r" (type_impl));

   for (unsigned int i = 0; i < pct->num_counters; i++) {
      if ((type_impl >> pct->type[i]) & 1) {
         /* Select the event count/type via the event type selection register */
         __asm volatile ("mcr p15,0,%0,c9,c12,5" :: "r" (i) : "memory");
         /* Configure the required event type */
         __asm volatile ("mcr p15,0,%0,c9,c13,1" :: "r" (pct->type[i]) : "memory");
         /* Set the bit to enable the counter */
         cntenset |= (1 << i);
      } else {
         printf("Event: %s not implemented\r\n", type_lookup(pct->type[i]));
      }
   }
   /* Write the control register */
   __asm volatile ("mcr p15,0,%0,c9,c12,0" :: "r" (ctrl) : "memory");

   /* Enable the counters */
   __asm volatile ("mcr p15,0,%0,c9,c12,1" :: "r" (cntenset) : "memory");
#else
   /* Only two counters (0 and 1) are supported on the arm11 */
   ctrl |= (pct->type[0] << 20);
   ctrl |= (pct->type[1] << 12);
   __asm volatile ("mcr p15,0,%0,c15,c12,0" :: "r" (ctrl) : "memory");
#endif
}

// cppcheck-suppress constParameterPointer
void read_performance_counters(perf_counters_t *pct) {
#if (__ARM_ARCH >= 7 )
   for (unsigned int i = 0; i < pct->num_counters; i++) {
      /* Select the event count/type via the event type selection register */
      __asm volatile ("mcr p15,0,%0,c9,c12,5" :: "r" (i) : "memory");
      /* Read the required event count */
      __asm volatile ("mrc p15,0,%0,c9,c13,2" : "=r" (pct->counter[i]));
   }
   __asm volatile ("mrc p15,0,%0,c9,c13,0" : "=r" (pct->cycle_counter));
#else
   // Only two counters (0 and 1) are supported on the arm11
   __asm volatile ("mrc p15,0,%0,c15,c12,2" : "=r" (pct->counter[0]));
   __asm volatile ("mrc p15,0,%0,c15,c12,3" : "=r" (pct->counter[1]));
   __asm volatile ("mrc p15,0,%0,c15,c12,1" : "=r" (pct->cycle_counter));
#endif
}

static char bfr[20+1];

static char* uint64ToDecimal(uint64_t v) {
   int first = 1;
   char* p = bfr + sizeof(bfr);
   *(--p) = '\0';
   while (v || first) {
      *(--p) = (char)('0' + v % 10);
      v = v / 10;
      first = 0;
   }
   return p;
}

void print_performance_counters(const perf_counters_t *pct) {
   uint64_t cycle_counter = pct->cycle_counter;
   cycle_counter *= 64;
   /* newlib-nano doesn't appear to support 64-bit printf/scanf on 32-bit systems
      printf("%26s = %"PRIu64"\r\n", "cycle counter", cycle_counter); */
   printf("%26s = %s\r\n", "cycle counter", uint64ToDecimal(cycle_counter));
   for (unsigned int i = 0; i < pct->num_counters; i++) {
      printf("%26s = %u\r\n", type_lookup(pct->type[i]), pct->counter[i]);
   }
}
