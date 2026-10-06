/* bootstage.c - boot-stage breadcrumbs, crash-record home and reset reason.

   Pure forensics, no VideoCore involvement: a persistent .noinit block that
   records how far each boot got (and, via exception.c, where a crash
   landed), plus the PM block's reset-reason register.  Lives in its own
   file because it has nothing to do with the mailbox it once grew beside.

   Block layout (16 words): 0 magic, 1 stage, 2 previous stage, 3 detail,
   4..11 crash record (exception.c), 12 previous detail. */

#include <stdint.h>
#include "rpi.h"
#include "base.h"
#include "cache.h"
#include "lowmem.h"

/* In .noinit: this block MUST NOT live at a fixed low address - 0x7C00 (the
   first attempt) is inside the VPU-shared Pi1MHz region (struct at 0x100,
   Beeb-writable shadow/JIM RAM after it), and stray bus bytes (CR, 0x0D)
   corrupted the detail words into phantom "died in init N" reports.
   .noinit survives the watchdog reset and the SD loader alike.  The known
   cost, learned the hard way in the fixed-address era: if the image that
   dies and the image that reports are DIFFERENT builds, .noinit moves with
   the layout and the report is silently lost - the magic word makes that a
   clean "nothing to report", never a phantom.  Same-build reboots (the
   normal lockup case) always line up. */
NOINIT_SECTION static volatile uint32_t boot_stage_block[16];
/* Chain-boot marker: the outgoing kernel writes it just before it jumps
   (chainboot.c); the incoming kernel_main reads and clears it, so the
   session knows it was chain-booted rather than cold-booted.  Not in
   .noinit: the copy of the incoming image runs over the outgoing kernel's
   .noinit, and a different build places .noinit elsewhere, so a marker
   there was lost whenever the two builds differed or the image was large.
   It lives at a fixed address under the kernel instead (lowmem.h), as two
   words - the magic and its complement - so that a stray write, or
   whatever RAM holds after a power-on, reads as a cold boot (the safe
   direction) and never as a phantom chain-boot. */
#define chain_marker ((volatile uint32_t *)LOWMEM_CHAIN_MARKER)
#define CHAIN_MAGIC 0xC4A1B007u
static unsigned int chain_booted_flag;
void RPI_ChainBootMark(void)
{
   chain_marker[0] = CHAIN_MAGIC;
   chain_marker[1] = ~CHAIN_MAGIC;
}
void RPI_ChainBootConsume(void)
{
   chain_booted_flag = (chain_marker[0] == CHAIN_MAGIC &&
                        chain_marker[1] == ~CHAIN_MAGIC) ? 1u : 0u;
   chain_marker[0] = 0u;
   chain_marker[1] = 0u;
}
unsigned int RPI_ChainBooted(void) { return chain_booted_flag; }
#define boot_stage_magic    (boot_stage_block[0])
#define boot_stage_current  (boot_stage_block[1])
#define boot_stage_previous (boot_stage_block[2])
#define boot_detail_current  (boot_stage_block[3])
#define boot_detail_previous (boot_stage_block[12])
#define BOOT_STAGE_MAGIC 0x8007ADE5u

void RPI_BootStage( boot_stage_t stage )
{
   if (boot_stage_magic != BOOT_STAGE_MAGIC) {
      /* First boot after a power cycle: nothing to report, start recording. */
      boot_stage_magic = BOOT_STAGE_MAGIC;
      boot_stage_previous = 0u;
   } else if (stage == BOOT_STAGE_ENTRY) {
      /* A reset got us here; carry over how far the last attempt reached. */
      boot_stage_previous = boot_stage_current;
      boot_detail_previous = boot_detail_current;
   }
   boot_stage_current = (uint32_t)stage;
   boot_detail_current = 0u;
   /* The block lives in write-back kernel RAM: clean it so a hang followed
      by a watchdog reset cannot lose the dirty line - exactly the stamp the
      next boot needs. Eight calls per boot, zero hot-path cost. */
   _clean_cache_area((const void *)(uintptr_t)boot_stage_block, 64);
}

void RPI_BootDetail( unsigned int detail )
{
   boot_detail_current = detail;
}

unsigned int RPI_BootDetailPrevious( void )
{
   return boot_detail_previous;
}

/* Reset reason from the PM block. RSTS bits 12..0: the "had watchdog reset"
   flag is bit 5 on BCM2835 (0x20); power-on shows the full set. Read once -
   the register survives until something clears it. */
volatile unsigned int *RPI_BootStageBlock( void )
{
   return (volatile unsigned int *)boot_stage_block;
}

unsigned int RPI_ResetReason( void )
{
   return (*(volatile unsigned int *)(PERIPHERAL_BASE + 0x00100020u)) & 0xfffu;
}

boot_stage_t RPI_BootStagePrevious( void )
{
   return (boot_stage_t)boot_stage_previous;
}

