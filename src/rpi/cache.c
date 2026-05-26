#include <stdio.h>
#include <string.h>
#include "arm-start.h"
#include "base.h"
#include "cache.h"
#include "rpi.h"
#include "info.h"
/* Historical Note:
   Were seeing core 3 crashes if inner *and* outer both set to some flavour of WB (i.e. 1 or 3)
   The point of crashing is when the data cache is enabled
   At that point, the stack appears to vanish and the data read back is 0x55555555
   Reason turned out to be failure to correctly invalidate the entire data cache */

volatile __attribute__ ((aligned (0x4000) )) NOINIT_SECTION unsigned int PageTable[4096];
#ifdef NUM_4K_PAGES
volatile __attribute__ ((aligned (0x4000) )) NOINIT_SECTION unsigned int PageTable2[NUM_4K_PAGES];
#endif

/* Just to keep things simple we cache all the ram (mem_info(1))*/
#define L1_CACHED_MEM_TOP (mem_info(1)>>20)
#define L2_CACHED_MEM_TOP (mem_info(1)>>20)

#define VC_TOP ((mem_info(1)>>20)+( (RPI_PropertyGetWord(TAG_GET_VC_MEMORY, 0)->data.buffer_32[1])>>20))
#define PERIPHERAL_END 0x80000000

#if (__ARM_ARCH >= 7 )
static const unsigned int aa0 = 0; /* note ARM ARM bit ordering is confusing */
static const unsigned int aa6 = 1;
static const unsigned int shareable = 1;
#endif

#if (__ARM_ARCH >= 7 )

#define SETWAY_LEVEL_SHIFT          1

/* 4 ways x 128 sets x 64 bytes per line 32KB */
#define L1_DATA_CACHE_SETS        128
#define L1_DATA_CACHE_WAYS          4
#define L1_SETWAY_WAY_SHIFT        30   /* 32-Log2(L1_DATA_CACHE_WAYS) */
#define L1_SETWAY_SET_SHIFT         6   /* Log2(L1_DATA_CACHE_LINE_LENGTH) */

#if (__ARM_ARCH == 7 )
/* 8 ways x 1024 sets x 64 bytes per line = 512KB */
#define L2_CACHE_SETS            1024
#define L2_CACHE_WAYS               8
#define L2_SETWAY_WAY_SHIFT        29   /* 32-Log2(L2_CACHE_WAYS) */
#else
/* 16 ways x 512 sets x 64 bytes per line = 512KB */
#define L2_CACHE_SETS             512
#define L2_CACHE_WAYS              16
#define L2_SETWAY_WAY_SHIFT        28   /* 32-Log2(L2_CACHE_WAYS) */
#endif

#define L2_SETWAY_SET_SHIFT         6   // Log2(L2_CACHE_LINE_LENGTH)

// The origin of this function is:
// https://github.com/rsta2/uspi/blob/master/env/lib/synchronize.c

void InvalidateDataCache (void)
{
   unsigned nSet;
   unsigned nWay;
   uint32_t nSetWayLevel;
   // invalidate L1 data cache
   for (nSet = 0; nSet < L1_DATA_CACHE_SETS; nSet++) {
      for (nWay = 0; nWay < L1_DATA_CACHE_WAYS; nWay++) {
         nSetWayLevel = nWay << L1_SETWAY_WAY_SHIFT
                      | nSet << L1_SETWAY_SET_SHIFT
                      | 0 << SETWAY_LEVEL_SHIFT;
         __asm volatile ("mcr p15, 0, %0, c7, c6,  2" : : "r" (nSetWayLevel) : "memory");   // DCISW
      }
   }

   // invalidate L2 unified cache
   for (nSet = 0; nSet < L2_CACHE_SETS; nSet++) {
      for (nWay = 0; nWay < L2_CACHE_WAYS; nWay++) {
         nSetWayLevel = nWay << L2_SETWAY_WAY_SHIFT
                      | nSet << L2_SETWAY_SET_SHIFT
                      | 1 << SETWAY_LEVEL_SHIFT;
         __asm volatile ("mcr p15, 0, %0, c7, c6,  2" : : "r" (nSetWayLevel) : "memory");   // DCISW
      }
   }
}

void CleanDataCache (void)
{
   unsigned nSet;
   unsigned nWay;
   uint32_t nSetWayLevel;
   // clean L1 data cache
   for (nSet = 0; nSet < L1_DATA_CACHE_SETS; nSet++) {
      for (nWay = 0; nWay < L1_DATA_CACHE_WAYS; nWay++) {
         nSetWayLevel = nWay << L1_SETWAY_WAY_SHIFT
                      | nSet << L1_SETWAY_SET_SHIFT
                      | 0 << SETWAY_LEVEL_SHIFT;
         __asm volatile ("mcr p15, 0, %0, c7, c10,  2" : : "r" (nSetWayLevel) : "memory");
      }
   }

   // clean L2 unified cache
   for (nSet = 0; nSet < L2_CACHE_SETS; nSet++) {
      for (nWay = 0; nWay < L2_CACHE_WAYS; nWay++) {
         nSetWayLevel = nWay << L2_SETWAY_WAY_SHIFT
                      | nSet << L2_SETWAY_SET_SHIFT
                      | 1 << SETWAY_LEVEL_SHIFT;
         __asm volatile ("mcr p15, 0, %0, c7, c10,  2" : : "r" (nSetWayLevel) : "memory");
      }
   }
}
#endif

//
void _clean_cache_area(const void * start, unsigned int length)
{
#if (__ARM_ARCH >= 7 )
   uint32_t cachelinesize;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
   char * startptr = start;
#pragma GCC diagnostic pop
   char * endptr;
   __asm volatile ("mrc p15, 0, %0, c0, c0,  1" : "=r" (cachelinesize));
   cachelinesize = (cachelinesize>> 16 ) & 0xF;
   cachelinesize = 4<<cachelinesize;
   endptr = startptr + length;

   // round down start address
   startptr = (char *)(((uint32_t)start) & ~(cachelinesize - 1));

   do{
      __asm volatile ("mcr     p15, 0, %0, c7, c14, 1" : : "r" (startptr));
      startptr = startptr + cachelinesize;
   } while ( startptr  < endptr);
#else
   __asm volatile("mcrr p15,0,%0,%1,c14"::"r" (((uint32_t)start)+length), "r" (start));
   _data_memory_barrier();
#endif
}

// cppcheck-suppress unusedFunction
void _invalidate_cache_area(const void * start, unsigned int length)
{
#if (__ARM_ARCH >= 7 )
   uint32_t cachelinesize;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
   char * startptr = start;
#pragma GCC diagnostic pop
   char * endptr;
   __asm volatile ("mrc p15, 0, %0, c0, c0,  1" : "=r" (cachelinesize));
   cachelinesize = (cachelinesize>> 16 ) & 0xF;
   cachelinesize = 4<<cachelinesize;
   endptr = startptr + length;
   // round down start address
   startptr = (char *)(((uint32_t)start) & ~(cachelinesize - 1));

   do{
      __asm volatile ("mcr     p15, 0, %0, c7, c6, 1" : : "r" (startptr));
      startptr = startptr + cachelinesize;
   } while ( startptr  < endptr);
#else
   __asm volatile("mcrr p15,0,%0,%1,c6"::"r" ((uint32_t)start+length), "r" (start));
#endif
   _data_memory_barrier();
}

#if 0
static void _invalidate_icache()
{
   __asm volatile ("mcr p15, 0, %0, c7, c5, 0" : : "r" (0));
}

static void _invalidate_dcache()
{
   __asm volatile ("mcr p15, 0, %0, c7, c6, 0" : : "r" (0));
}

static void _clean_invalidate_dcache()
{
   __asm volatile ("mcr p15, 0, %0, c7, c14, 0" : : "r" (0));
}

static void _invalidate_dcache_mva(void *address)
{
   __asm volatile ("mcr p15, 0, %0, c7, c6, 1" : : "r" (address));
}

static void _clean_invalidate_dcache_mva(void *address)
{
   __asm volatile ("mcr p15, 0, %0, c7, c14, 1" : : "r" (address));
}

static void _invalidate_dtlb()
{
   __asm volatile ("mcr p15, 0, %0, c8, c6, 0" : : "r" (0));
}
#endif

// TLB 4KB Section Descriptor format
// 31..12 Section Base Address
// 11..9        - unused, set to zero
// 8..6   TEX   - type extension- TEX, C, B used together, see below
// 5..4   AP    - access ctrl   - set to 11 for full access from user and super modes
// 3      C     - cacheable     - TEX, C, B used together, see below
// 2      B     - bufferable    - TEX, C, B used together, see below
// 1      1
// 0      1
#ifdef NUM_4K_PAGES
static void _invalidate_dtlb_mva(const void *address)
{
   __asm volatile ("mcr p15, 0, %0, c8, c6, 1" : : "r" (address));
}

void map_4k_page(unsigned int logical, unsigned int physical) {
  // Invalidate the data TLB before changing mapping
  _invalidate_dtlb_mva((void *)(logical << 12));
  // Setup the 4K page table entry
  // Second level descriptors use extended small page format
  //  so inner/outer caching can be controlled
  // Pi 0/1:
  //   XP (bit 23) in SCTRL is 0 so descriptors use ARMv4/5 backwards compatible format
  // Pi 2/3:
  //   XP (bit 23) in SCTRL no longer exists, and we see to be using ARMv6 table formats
  //   this means bit 0 of the page table is actually XN and must be clear
  //   to allow native ARM code to execute
  //   (this was the cause of issue #27)
#if (__ARM_ARCH >= 7 )
  PageTable2[logical] = (physical<<12) | 0x132u | (1 << 6) | (1<<3) | (1 << 2);
#else
  PageTable2[logical] = (physical<<12) | 0x133u | (1 << 6) | (1<<3) | (1 << 2);
#endif
}
#endif
void enable_MMU_and_IDCaches(unsigned int num_4k_pages)
{

  LOG_DEBUG("enable_MMU_and_IDCaches\r\n");

  unsigned int base=0;
  unsigned int end;
  unsigned int start;

  // TLB 1MB Sector Descriptor format
  // 31..20 Section Base Address
  // 19     NS    - ?             - set to 0
  // 18     0     -               - set to 0 - 1 = 16Mbyte section
  // 17     nG    - ?             - set to 0
  // 16     S     - ?             - set to 0
  // 15     APX   - access ctrl   - set to 0 for full access from user and super modes
  // 14..12 TEX   - type extension- TEX, C, B used together, see below
  // 11..10 AP    - access ctrl   - set to 11 for full access from user and super modes
  // 9      P     -               - set to 0
  // 8..5   Domain- access domain - set to 0000 as nor using access ctrl
  // 4      XN    - eXecute Never - set to 1 for I/O devices
  // 3      C     - cacheable     - set to 1 for cacheable RAM i
  // 2      B     - bufferable    - set to 1 for cacheable RAM
  // 1      1                     - TEX, C, B used together, see below
  // 0      0                     - TEX, C, B used together, see below

  // For I/O devices
  // TEX = 000; C=0; B=1 (Shared device)

  // For cacheable RAM
  // TEX = 001; C=1; B=1 (Outer and inner write back, write allocate)

  // For non-cacheable RAM
  // TEX = 001; C=0; B=0 (Outer and inner non-cacheable)

  // For individual control
  // TEX = 1BB CB=AA
  // AA = inner policy
  // BB = outer policy
  // 00 = NC    (non-cacheable)
  // 01 = WBWA  (write-back, write allocate)
  // 10 = WT    (write-through
  // 11 = WBNWA (write-back, no write allocate)
  // TEX = 100; C=0; B=1 (outer non cacheable, inner write-back, write allocate)

  // replace the first N 1MB entries with second level page tables, giving N x 256 4K pages
  #ifdef NUM_4K_PAGES
  for ( base = 0; base < num_4k_pages >> 8; base++)
  {
    PageTable[base] = ((unsigned int) (&PageTable2[base << 8])) | 1;
  }
  #endif
  end = L1_CACHED_MEM_TOP;

  for (; base < end;  base++)
  {
    // Value from my original RPI code = 11C0E (outer and inner write back, write allocate, shareable)
    // bits 11..10 are the AP bits, and setting them to 11 enables user mode access as well
    // Values from RPI2 = 11C0E (outer and inner write back, write allocate, shareable (fast but unsafe)); works on RPI
    // Values from RPI2 = 10C0A (outer and inner write through, no write allocate, shareable)
    // Values from RPI2 = 15C0A (outer write back, write allocate, inner write through, no write allocate, shareable)
    PageTable[base] = base << 20 | 0x0C0E ;
  }
  end = L2_CACHED_MEM_TOP;
  for (; base < end; base++)
  {
     PageTable[base] = (base << 20) | 0x10C0E;
  }
  for (; base < (PERIPHERAL_BASE>>20); base++)
  {
     PageTable[base] = (base << 20) | 0x11C06 ;
  }

  for (; base < PERIPHERAL_END>>20; base++)
  {
    // shared device, never execute store ordered
     PageTable[base] = (base << 20) | 0x10C12;
  }
  // now create and uncached copy of the memory
  end = base + VC_TOP;
  start = 0;
  for (; base <  end; base++)
     PageTable[base] = ((start++) << 20 )| 0x0C0E ;
#ifdef NUM_4K_PAGES
  if ( num_4k_pages != 0 )
  {
     for (uint32_t i = 0; i < num_4k_pages >> 8; i++)
     {
        PageTable[i] = (unsigned int) (&PageTable2[i << 8]);
        PageTable[i] +=1;
     }

     // populate the second level page tables
     for (base = 0; base < num_4k_pages; base++)
     {
        map_4k_page(base, base);
     }
  }
#endif

#if (__ARM_ARCH >= 8 )
  //unsigned cpuextctrl0, cpuextctrl1;
  //__asm volatile ("mrrc p15, 1, %0, %1, c15" : "=r" (cpuextctrl0), "=r" (cpuextctrl1));
  //LOG_DEBUG("extctrl = %08x %08x\r\n", cpuextctrl1, cpuextctrl0);
#else
  // RPI:  bit 6 of auxctrl is restrict cache size to 16K (no page coloring)
  // RPI2: bit 6 of auxctrl is set SMP bit, otherwise all caching disabled
#if (__ARM_ARCH == 7 )
 unsigned auxctrl;
  __asm volatile ("mrc p15, 0, %0, c1, c0,  1" : "=r" (auxctrl));
  auxctrl |= 1 << 6;
  __asm volatile ("mcr p15, 0, %0, c1, c0,  1" :: "r" (auxctrl));
 #endif
#endif

  // set domain 0 to client
  __asm volatile ("mcr p15, 0, %0, c3, c0, 0" :: "r" (1));

#if (__ARM_ARCH >= 7 )
  // set TTBR0 - page table walk memory cacheability/shareable
  // [Bit 0, Bit 6] indicates inner cachability: 01 = normal memory, inner write-back write-allocate cacheable
  // [Bit 4, Bit 3] indicates outer cachability: 01 = normal memory, outer write-back write-allocate cacheable
  // Bit 1 indicates shareable
  // 4A = 0100 1010
  unsigned int attr = ((aa6) << 6) | (1 << 3) | (shareable << 1) | ((aa0 ));
  __asm volatile ("mcr p15, 0, %0, c2, c0, 0" :: "r" (attr | (unsigned) &PageTable));
#else
  // set TTBR0 (page table walk inner cacheable, outer non-cacheable, shareable memory)
  __asm volatile ("mcr p15, 0, %0, c2, c0, 0" :: "r" (0x03 | (unsigned) &PageTable));
#endif

  // Invalidate entire data cache
#if (__ARM_ARCH >= 7 )
  __asm volatile ("isb" ::: "memory");
  InvalidateDataCache();
#else
  // invalidate data cache and flush prefetch buffer
  __asm volatile ("mcr p15, 0, %0, c7, c5,  4" :: "r" (0) : "memory");
  __asm volatile ("mcr p15, 0, %0, c7, c6,  0" :: "r" (0) : "memory");
#endif

  // enable MMU, L1 cache and instruction cache, L2 cache, write buffer,
  //   branch prediction and extended page table on
  unsigned sctrl;
  __asm volatile ("mrc p15,0,%0,c1,c0,0" : "=r" (sctrl));
  // Bit 12 enables the L1 instruction cache
  // Bit 11 enables branch pre-fetching
  // Bit  2 enables the L1 data cache
  // Bit  0 enabled the MMU
  // The L1 instruction cache can be used independently of the MMU
  // The L1 data cache will one be enabled if the MMU is enabled
  sctrl |= 0x00001805;
  sctrl |= 1<<22; // U (v6 unaligned access model)
  __asm volatile ("mcr p15,0,%0,c1,c0,0" :: "r" (sctrl) : "memory");
}