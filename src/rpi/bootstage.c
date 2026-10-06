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
#include "systimer.h"

/* In .noinit: this block MUST NOT live at a fixed low address - at 0x7C00
   (the first attempt) stray bytes (CR, 0x0D) corrupted the detail words into
   phantom "died in init N" reports.  The writer was never found: the VPU
   never touches ARM low RAM (it reads Pi1MHz_MEM_BASE in peripheral space
   and posts through the SMI registers), and nothing at 0x100-0x13FF, the
   Pi1MHz struct and callback table, reaches that far (lowmem.h).
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
   It lives at a fixed address under the kernel instead (lowmem.h).

   Living there, it also survives what it must not: a reset.  If the
   incoming kernel dies before it consumes the marker, or a kernel that does
   not know this marker runs for a while and is then reset, the SD kernel
   that boots next would find it - and, believing it was chain-booted,
   report "n/a (chain-boot)" and skip launching the VPU, leaving the bus
   dead.  So the marker counts only for a jump that has just happened with
   no reset in between.  Two tests, either of which a reset fails:
   - the 64-bit system timer, stamped at the jump, has moved on less than
     CHAIN_MARK_MAX_US.  A reset either restarts the timer (now before the
     stamp) or takes far longer, because the firmware then reloads
     bootcode, start.elf and the kernel from the card.  INFERRED, both: not
     the watchdog timeout - reboot_now() fires it after one tick.  The jump
     itself - a cached copy of at most 4 MB, the cache clean and the
     incoming .bss clear - should be well inside it; RPI_ChainBootJumpUs()
     reports what it took, on the /status Boot time row.
   - the reset-reason register is unchanged.  Its flags are sticky, so this
     alone would miss a second watchdog reset after a first; the timer does
     not.  INFERRED: it catches a reset whose timing happened to fit.
   And two words, the magic and its complement, so that whatever RAM holds
   after a power-on, or a stray write, reads as a cold boot - the safe
   direction - never as a phantom chain-boot. */
#define chain_marker ((volatile uint32_t *)LOWMEM_CHAIN_MARKER)
#define CHAIN_MAGIC 0xC4A1B007u
#define CHAIN_MARK_MAX_US 500000u
static unsigned int chain_booted_flag;
static uint32_t chain_jump_us;
void RPI_ChainBootMark(void)
{
   uint64_t now = RPI_GetSystemTime64();
   chain_marker[2] = (uint32_t)now;
   chain_marker[3] = (uint32_t)(now >> 32);
   chain_marker[4] = RPI_ResetReason();
   chain_marker[0] = CHAIN_MAGIC;
   chain_marker[1] = ~CHAIN_MAGIC;
   /* Out to RAM now, rather than leave it to the copier's set/way clean:
      the copy runs with the D-cache on. */
   _clean_cache_area((const void *)LOWMEM_CHAIN_MARKER, 5u * sizeof(uint32_t));
}
void RPI_ChainBootConsume(void)
{
   uint64_t now = RPI_GetSystemTime64();
   uint64_t stamp = ((uint64_t)chain_marker[3] << 32) | chain_marker[2];
   chain_booted_flag = (chain_marker[0] == CHAIN_MAGIC &&
                        chain_marker[1] == ~CHAIN_MAGIC &&
                        now >= stamp && now - stamp < CHAIN_MARK_MAX_US &&
                        chain_marker[4] == RPI_ResetReason()) ? 1u : 0u;
   /* Mark to here: the copy, the cache clean and the .bss clear - the
      measurement behind CHAIN_MARK_MAX_US. */
   chain_jump_us = chain_booted_flag ? (uint32_t)(now - stamp) : 0u;
   chain_marker[0] = 0u;
   chain_marker[1] = 0u;
}
unsigned int RPI_ChainBooted(void) { return chain_booted_flag; }
unsigned int RPI_ChainBootJumpUs(void) { return chain_jump_us; }
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
   flag is bit 5 on BCM2835 (0x20), "had power-on reset" bit 12 (0x1000);
   power-on shows the full set.  The register survives until something
   clears it. */
volatile unsigned int *RPI_BootStageBlock( void )
{
   return (volatile unsigned int *)boot_stage_block;
}

unsigned int RPI_ResetReason( void )
{
   return (*(volatile unsigned int *)(PERIPHERAL_BASE + 0x00100020u)) & 0x1fffu;
}

boot_stage_t RPI_BootStagePrevious( void )
{
   return (boot_stage_t)boot_stage_previous;
}

