/* lowmem.h - the fixed ARM addresses below the kernel.

   Low RAM map (ARM physical = virtual, identity mapped):

     0x0000-0x00FF  exception vectors / the firmware's ARM stub at boot
     0x0100-0x13FF  Pi1MHz struct (FRED/JIM pages, JIM RAM fields) and the
                    FRED/JIM callback table - Pi1MHz.h, checked there
     0x1400-0x39FF  free
     0x3A00-0x3CFF  the VPU's 1MHz-bus program (Pi1MHz.c), copied here at
                    a cold boot and launched from here; a chain-boot leaves
                    it alone, because the VPU is still running it
     0x3D00-0x3DFF  markers handed from one kernel to the next:
                      0x3D00  chain-boot marker: CHAIN_MAGIC, its
                              complement, the jump's timer stamp and
                              reset reason (rpi/bootstage.c)
                      0x3D20  the video player's GPU handle block
                              (videoplayer.c)
     0x3E00-0x3EFF  the kernel.now copier (rpi/arm-start.S)
     0x4000-0x7FFF  the L1 page table (rpi/cache.c)
     0x8000-        the kernel image, then .data, .noinit, .bss, the heap

   These four blocks are a deliberate exception to "ARM state goes in
   .noinit".  A kernel.now chain-boot copies the incoming image from 0x8000
   upward over the running kernel, so everything above 0x8000 - .noinit
   included - is in the copy's path, and moves whenever a build changes the
   layout.  The page table the copy translates through, the copier itself,
   the code the VPU goes on running across the jump, and anything one build
   must hand to a different one therefore live below the kernel, where no
   image can reach them.  (Only the L1 table: the 4 KB
   second-level table, PageTable2, compiled only with NUM_4K_PAGES, is still
   in .noinit.)

   None of this is on the bus.  The VPU only fetches its program from low
   RAM and never writes it: it reads the FRED/JIM pages from Pi1MHz_MEM_BASE
   in peripheral space and posts bus cycles through the SMI registers.
   bootstage.c records stray bytes once seen at 0x7C00, cause never found -
   which is why the chain-boot marker is a magic word and its complement,
   and carries a time stamp.

   Plain numbers, no suffixes: arm-start.S includes this as well. */
#ifndef LOWMEM_H
#define LOWMEM_H

#define LOWMEM_KERNEL_BASE           0x8000

/* Up to the markers; Pi1MHz.c checks the program fits. */
#define LOWMEM_VPU_PROGRAM           0x3A00
#define LOWMEM_VPU_PROGRAM_SIZE      0x300

#define LOWMEM_MARKERS               0x3D00
#define LOWMEM_CHAIN_MARKER          (LOWMEM_MARKERS + 0x00)  /* 5 words */
#define LOWMEM_VIDEOBUF_PERSIST      (LOWMEM_MARKERS + 0x20)  /* 5 words */

#define LOWMEM_CHAINBOOT_COPIER      0x3E00
#define LOWMEM_CHAINBOOT_COPIER_SIZE 0x100

/* 16 KB and 16 KB-aligned: TTBR0 with TTBCR.N = 0 (see cache.c). */
#define LOWMEM_PAGE_TABLE            0x4000
#define LOWMEM_PAGE_TABLE_SIZE       0x4000

#endif
