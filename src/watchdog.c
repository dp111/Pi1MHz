/* Hardware watchdog.
 *
 * The BCM2835 watchdog resets the Pi if it is not re-armed in time.  Kicking
 * it from a poll callback therefore turns "the main loop stopped" - a hang in
 * a driver, a wait that never completes, a jump into a bad image - from
 * something needing a physical power cycle into an automatic reboot.  During
 * development that is the difference between losing a minute and losing the
 * machine until someone can reach it.
 *
 * What it does NOT catch: a main loop that keeps running while something else
 * is dead.  A Pi whose WiFi has stopped but whose poll loop is happily going
 * round still kicks the watchdog, quite correctly - the loop is alive.  That
 * failure needs the separate rejoin and RX-silence logic in the WiFi driver.
 *
 * The timeout has to clear the longest legitimate stall.  Measured worst
 * cases on this machine: ~1.3 s for a full 256-block BeebSCSI READ6 paced by
 * the Beeb, and ~142 ms for a single SD write during a card garbage-collect.
 * The default leaves an order of magnitude of headroom over both.  The
 * hardware caps the timeout at just under 16 s.
 *
 * Off unless Pi1MHz.cfg asks for it, because a reboot is a heavy-handed
 * response to a machine that might merely be slow, and because anything that
 * reboots the Pi under a mounted ADFS deserves an explicit opt-in.
 */

#include <stdlib.h>

#include "Pi1MHz.h"
#include "config.h"
#include "rpi/base.h"
#include "rpi/rpi.h"
#include "rpi/systimer.h"
#include "watchdog.h"

#define PM_PASSWORD            0x5a000000u
#define PM_RSTC_WRCFG_FULL_RESET 0x00000020u
#define PM_RSTC_WRCFG_CLR      0xffffffcfu
/* The counter ticks at 65536 Hz and the field is 20 bits, so the longest
   timeout the hardware offers is 0xfffff / 65536 = 15.99 s. */
#define PM_WDOG_TICKS_PER_SEC  65536u
#define PM_WDOG_TIME_SET       0x000fffffu

#define WATCHDOG_DEFAULT_SECONDS 10u
#define WATCHDOG_MAX_SECONDS     15u
/* Boot runs unconfigured - Pi1MHz.cfg has not been read yet - so the boot
   watchdog uses the longest timeout the hardware offers. */
#define WATCHDOG_BOOT_SECONDS    WATCHDOG_MAX_SECONDS

static volatile uint32_t *const PM_RSTC = (uint32_t *)(PERIPHERAL_BASE + 0x0010001cu);
static volatile uint32_t *const PM_WDOG = (uint32_t *)(PERIPHERAL_BASE + 0x00100024u);

static uint32_t watchdog_ticks;
/* When the hardware was last re-armed.  The poll runs ~290,000 times a
   second, but the shortest timeout the config allows is one second, so
   kicking on every pass spent three peripheral accesses (~590 cycles, 19% of
   the whole idle loop) to service a deadline measured in seconds.  Kicking at
   4 Hz leaves at least three quarters of the timeout as margin even at the
   1 s setting, and costs one system-timer read. */
static uint32_t watchdog_last_kick_us;
#define WATCHDOG_KICK_INTERVAL_US 250000u
/* Nothing is armed until this passes.  Boot does a lot of blocking work -
   mounting the card, loading firmware and NVRAM, the bring-up settles, the
   join, DHCP - and arming before that has finished risks resetting mid-boot,
   which would loop forever and be far worse than the hang this exists to
   escape.  Steady state is only reached once all of that is done. */
/* No arming grace any more.  It existed because boot ran with the watchdog
   OFF and had to be protected from its own slowness; boot now arms the
   watchdog at the hardware maximum and kicks it between every emulator init,
   so the dog is fed continuously from the first instruction and there is
   nothing left for a grace period to cover.  Keeping one would only create a
   window where neither boot nor the poll loop was feeding it. */

/* Re-arm.  Writing WDOG then RSTC restarts the countdown, so simply calling
   this often enough is the whole mechanism - there is no separate "pet"
   register to poke. */
/* Arm the watchdog for the boot itself, and kick it.
 *
 * This exists because the opposite - stopping the watchdog at the top of
 * kernel_main - left the machine unprotected for the whole of boot plus the
 * arming grace, which is exactly when the lockups happen: a kernel.now
 * chain-boot that dies before USB enumerates used to sit there until someone
 * pulled the power.  The inherited-countdown problem that motivated stopping
 * it is real, but the answer is to take the countdown over, not to switch it
 * off: this re-arms at the hardware maximum and boot kicks it as it goes.
 *
 * Deliberately unconditional - the config that might disable the watchdog has
 * not been read at this point, and watchdog_init() turns it off again if the
 * user does not want one.  The cost of being wrong is a reset on a machine
 * whose boot stalled for 15 s, which is not a machine that was going to
 * finish booting. */
// cppcheck-suppress unusedFunction
void watchdog_boot_kick(void)
{
   *PM_WDOG = PM_PASSWORD | (WATCHDOG_BOOT_SECONDS * PM_WDOG_TICKS_PER_SEC);
   *PM_RSTC = PM_PASSWORD | ((*PM_RSTC & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET);
}

/* Stop the watchdog dead.  Clearing WRCFG leaves the counter running but tells
   the reset controller to do nothing when it expires.
   
   This has to be called early in boot, and not merely skipped: the PM block is
   a peripheral, so a countdown armed before a kernel.now chain-boot keeps
   counting straight through the handover.  The incoming kernel then spends its
   boot grace deliberately not kicking - and gets reset part-way up by the
   watchdog the OUTGOING kernel armed.  Seen as a flashed image booting cleanly
   all the way to "address ready" and then vanishing back to the SD kernel with
   no abort and nothing in the log. */
// cppcheck-suppress unusedFunction
void watchdog_stop(void)
{
   *PM_RSTC = PM_PASSWORD | (*PM_RSTC & PM_RSTC_WRCFG_CLR);
}

static void watchdog_poll(void)
{
   /* Shared per-pass clock: a 250 ms interval does not need better. */
   uint32_t now = Pi1MHz_now_us;

   if ((uint32_t)(now - watchdog_last_kick_us) < WATCHDOG_KICK_INTERVAL_US)
      return;
   watchdog_last_kick_us = now;

   *PM_WDOG = PM_PASSWORD | watchdog_ticks;
   *PM_RSTC = PM_PASSWORD | ((*PM_RSTC & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET);
}

// cppcheck-suppress unusedFunction
void watchdog_init(uint8_t instance, uint8_t address)
{
   const char *setting = config_get("watchdog");
   long seconds;

   /* Not watchdog_stop() here: boot armed the watchdog and has been kicking
      it, and dropping the guard now would reopen the window this whole
      arrangement exists to close.  Only an explicit "no watchdog" in the
      config disarms it, below. */

   (void)instance;
   (void)address;

   if (setting == NULL || setting[0] == '\0') {
      watchdog_stop();              /* not configured: stand the boot dog down */
      return;
   }

   {
      /* Tell "watchdog=0" (deliberately off) from "watchdog=yes" (a typo
         strtol also reads as 0): a mistyped setting must not silently
         disarm the guard, so an unparsable value takes the default. */
      char *end = NULL;
      seconds = strtol(setting, &end, 10);
      if (end == setting)
         seconds = (long)WATCHDOG_DEFAULT_SECONDS;
   }
   if (seconds <= 0) {
      watchdog_stop();              /* explicitly disabled */
      return;
   }

   if (seconds > (long)WATCHDOG_MAX_SECONDS)
      seconds = (long)WATCHDOG_MAX_SECONDS;

   watchdog_ticks = (uint32_t)seconds * PM_WDOG_TICKS_PER_SEC;
   if (watchdog_ticks > PM_WDOG_TIME_SET)
      watchdog_ticks = PM_WDOG_TIME_SET;

   /* Take over from the boot watchdog seamlessly: stamp the kick clock as
      already due so the first poll re-arms with the configured timeout. */
   watchdog_last_kick_us = RPI_GetSystemTime() - WATCHDOG_KICK_INTERVAL_US;
   Pi1MHz_Register_Poll(watchdog_poll);
}
