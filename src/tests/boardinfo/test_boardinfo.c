/* Host tests for rpi/info.c's board-revision decisions.
 *
 * board_usb_behind_hub() decides the USB role for the whole session, once,
 * from the revision code.  Calling a Zero, A+ or CM "behind a hub" forces
 * host mode and takes away MTP and kernel.now - the Pi's remote-recovery
 * path - and the config cannot override it.  So:
 *   - an unknown revision (the query failed) must count as NOT behind a hub,
 *     which leaves the choice to usb_mode in the config;
 *   - a revision, once known, is kept and never asked for again;
 *   - the revision and ARM-memory queries, one-shot answers the session
 *     keeps, use the full mailbox bound, while the runtime temperature read
 *     keeps the short query bound.
 * Each scenario runs in its own process: info.c's caches are static.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "boardinfo_stubs.h"
#include "rpi/info.h"

uint32_t Pi1MHz_now_us;

/* The fake VideoCore. */
static bool vc_alive = true;
static uint32_t vc_revision;
static rpi_mailbox_property_t prop;

/* How each tag was asked: [0] the query bound, [1] the full bound. */
static unsigned int asked_revision[2], asked_arm_memory[2], asked_temperature[2];

static rpi_mailbox_property_t *vc(rpi_mailbox_tag_t tag, uint32_t data, int full)
{
   switch (tag) {
   case TAG_GET_BOARD_REVISION: asked_revision[full]++; break;
   case TAG_GET_ARM_MEMORY:     asked_arm_memory[full]++; break;
   case TAG_GET_TEMPERATURE:    asked_temperature[full]++; break;
   default: break;
   }
   if (!vc_alive)
      return NULL;
   memset(&prop, 0, sizeof prop);
   prop.tag = tag;
   switch (tag) {
   case TAG_GET_BOARD_REVISION: prop.data.buffer_32[0] = vc_revision; break;
   case TAG_GET_ARM_MEMORY:     prop.data.buffer_32[1] = 448u << 20; break;
   case TAG_GET_TEMPERATURE:    prop.data.buffer_32[1] = 45000u; break;
   case TAG_GET_CLOCK_RATE:     prop.data.buffer_32[0] = data;
                                prop.data.buffer_32[1] = 1000000000u; break;
   default: break;
   }
   return &prop;
}

rpi_mailbox_property_t *RPI_PropertyGetWord(rpi_mailbox_tag_t tag, uint32_t data)
{
   return vc(tag, data, 0);
}
/* Declared here as well as (after the fix) in mailbox.h, so the unmodified
   info.c - which never calls it - still links: the negative control. */
rpi_mailbox_property_t *RPI_PropertyGetWordOnce(rpi_mailbox_tag_t tag, uint32_t data);
rpi_mailbox_property_t *RPI_PropertyGetWordOnce(rpi_mailbox_tag_t tag, uint32_t data)
{
   return vc(tag, data, 1);
}
rpi_mailbox_property_t *RPI_PropertyGetBuffer(rpi_mailbox_tag_t tag)
{
   (void)tag;
   return NULL;
}

static int failures, checks;

#define CHECK(cond, ...) do { \
      checks++; \
      if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                     printf(__VA_ARGS__); printf("\n"); } \
   } while (0)

/* --- scenarios ----------------------------------------------------------- */

static void unknown_revision_is_device_capable(void)
{
   vc_alive = false;
   CHECK(!board_usb_behind_hub(),
         "a failed revision query forced USB host mode (unknown must not mean B)");
}

static void failed_query_is_not_cached(void)
{
   vc_alive = false;
   (void)board_usb_behind_hub();
   vc_alive = true;
   vc_revision = 0xa02082u;           /* 3B */
   CHECK(board_usb_behind_hub(), "a revision that arrived late was ignored");
}

static void known_revision_is_kept(void)
{
   vc_revision = 0x9000c1u;           /* Zero W */
   CHECK(!board_usb_behind_hub(), "Zero W taken as behind a hub");
   vc_alive = false;                  /* the VC stops answering */
   CHECK(!board_usb_behind_hub(), "Zero W answer lost once the VC went quiet");
   (void)get_info_string();
   CHECK(asked_revision[0] + asked_revision[1] == 1u,
         "revision asked %u times; it never changes, ask once",
         asked_revision[0] + asked_revision[1]);
   CHECK(strncmp(get_info_string(), "9000c1 ", 7) == 0,
         "help-screen Pi line '%s'", get_info_string());
}

static void known_b_is_kept(void)
{
   vc_revision = 0xa02082u;           /* 3B */
   CHECK(board_usb_behind_hub(), "3B not taken as behind a hub");
   vc_alive = false;
   CHECK(board_usb_behind_hub(), "3B answer lost once the VC went quiet");
}

static void boot_queries_use_the_full_bound(void)
{
   vc_revision = 0x9000c1u;
   (void)board_usb_behind_hub();
   CHECK(asked_revision[1] == 1u && asked_revision[0] == 0u,
         "revision asked with the query bound (%u) not the full bound (%u)",
         asked_revision[0], asked_revision[1]);
   CHECK(mem_info(1) == (448u << 20), "mem_info(1) = %u", (unsigned)mem_info(1));
   CHECK(asked_arm_memory[1] == 1u && asked_arm_memory[0] == 0u,
         "ARM memory asked with the query bound (%u) not the full bound (%u)",
         asked_arm_memory[0], asked_arm_memory[1]);
   /* The runtime read stays on the short bound: the BREAK budget. */
   info_refresh_cached();
   CHECK(asked_temperature[0] == 1u && asked_temperature[1] == 0u,
         "temperature asked with the full bound (%u) - a runtime path slowed",
         asked_temperature[1]);
   CHECK(get_temp_millidegrees() == 45000u, "temperature %u",
         (unsigned)get_temp_millidegrees());
}

/* The revision table itself, unchanged by the fix but pinned with it. */
static const struct { uint32_t rev; bool behind; const char *name; } boards[] = {
   { 0x0002u,   true,  "B rev 1" },
   { 0x000eu,   true,  "B rev 2" },
   { 0x0010u,   true,  "B+" },
   { 0x0012u,   false, "A+" },
   { 0x0011u,   false, "CM1" },
   { 0x900092u, false, "Zero" },
   { 0x9000c1u, false, "Zero W" },
   { 0x902120u, false, "Zero 2 W" },
   { 0xa01041u, true,  "2B" },
   { 0xa02082u, true,  "3B" },
   { 0xa020d3u, true,  "3B+" },
   { 0x9020e0u, false, "3A+" },
   { 0xa020a0u, false, "CM3" },
   { 0x1a02082u, true, "3B, warranty bit set" },
};
static unsigned int board_index;

static void board_table(void)
{
   vc_revision = boards[board_index].rev;
   CHECK(board_usb_behind_hub() == boards[board_index].behind,
         "%s (%06x): behind a hub should be %d", boards[board_index].name,
         (unsigned)boards[board_index].rev, (int)boards[board_index].behind);
}

/* --- runner -------------------------------------------------------------- */

static unsigned int scenarios, scenario_failures;

static void run(const char *name, void (*fn)(void))
{
   fflush(stdout);
   pid_t pid = fork();
   if (pid == 0) {
      fn();
      fflush(stdout);
      _exit(failures ? 1 : 0);
   }
   int status = 0;
   scenarios++;
   if (pid < 0 || waitpid(pid, &status, 0) != pid ||
       !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      scenario_failures++;
      printf("FAIL scenario: %s\n", name);
   }
}

int main(void)
{
   run("unknown revision is device-capable", unknown_revision_is_device_capable);
   run("a failed query is not cached", failed_query_is_not_cached);
   run("a known revision is kept (Zero W)", known_revision_is_kept);
   run("a known revision is kept (3B)", known_b_is_kept);
   run("boot queries use the full bound", boot_queries_use_the_full_bound);
   for (board_index = 0; board_index < sizeof boards / sizeof boards[0]; board_index++)
      run(boards[board_index].name, board_table);

   printf("%u scenarios, %u failed\n", scenarios, scenario_failures);
   if (scenario_failures == 0)
      printf("BOARDINFO TESTS PASSED\n");
   (void)checks;
   return scenario_failures ? 1 : 0;
}
