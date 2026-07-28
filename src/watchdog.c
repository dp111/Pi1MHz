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

static volatile uint32_t *const PM_RSTC = (uint32_t *)(PERIPHERAL_BASE + 0x0010001cu);
static volatile uint32_t *const PM_WDOG = (uint32_t *)(PERIPHERAL_BASE + 0x00100024u);

static uint32_t watchdog_ticks;
/* Nothing is armed until this passes.  Boot does a lot of blocking work -
   mounting the card, loading firmware and NVRAM, the bring-up settles, the
   join, DHCP - and arming before that has finished risks resetting mid-boot,
   which would loop forever and be far worse than the hang this exists to
   escape.  Steady state is only reached once all of that is done. */
static uint32_t watchdog_arm_after_us;
#define WATCHDOG_BOOT_GRACE_US (60u * 1000000u)

/* Re-arm.  Writing WDOG then RSTC restarts the countdown, so simply calling
   this often enough is the whole mechanism - there is no separate "pet"
   register to poke. */
static void watchdog_poll(void)
{
   if (watchdog_arm_after_us != 0u) {
      if ((int32_t)(RPI_GetSystemTime() - watchdog_arm_after_us) < 0)
         return;                    /* still booting; leave it disarmed */
      watchdog_arm_after_us = 0u;
   }

   *PM_WDOG = PM_PASSWORD | watchdog_ticks;
   *PM_RSTC = PM_PASSWORD | ((*PM_RSTC & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET);
}

// cppcheck-suppress unusedFunction
void watchdog_init(uint8_t instance, uint8_t address)
{
   const char *setting = config_get("watchdog");
   long seconds;

   (void)instance;
   (void)address;

   if (setting == NULL || setting[0] == '\0')
      return;                       /* not configured: leave the watchdog off */

   seconds = strtol(setting, NULL, 10);
   if (seconds <= 0)
      return;                       /* explicitly disabled */

   if (seconds > (long)WATCHDOG_MAX_SECONDS)
      seconds = (long)WATCHDOG_MAX_SECONDS;
   if (seconds < 1)
      seconds = (long)WATCHDOG_DEFAULT_SECONDS;

   watchdog_ticks = (uint32_t)seconds * PM_WDOG_TICKS_PER_SEC;
   if (watchdog_ticks > PM_WDOG_TIME_SET)
      watchdog_ticks = PM_WDOG_TIME_SET;

   /* Registered now, armed later - see watchdog_arm_after_us.  Emulator
      registration runs before the WiFi bring-up and DHCP have finished, so
      "the emulators are up" is not yet steady state. */
   watchdog_arm_after_us = RPI_GetSystemTime() + WATCHDOG_BOOT_GRACE_US;
   Pi1MHz_Register_Poll(watchdog_poll);
}
