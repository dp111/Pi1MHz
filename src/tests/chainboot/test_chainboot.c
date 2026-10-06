/* Host tests for chainboot.c.
 *
 * What a chain-boot hands over and when it jumps are only seen on hardware
 * as "the Pi came back" or "the Pi went silent", so they are pinned here:
 * the image check; the hand-over, which pads the image to 64 bytes with
 * zeros inside its buffer, refuses (and frees) one that cannot be padded,
 * and lets a second image replace the first; and the steps the main loop
 * takes before the jump - a settle for the sender's answer, USB off, the
 * card ejected as the Beeb's reboot does, and the refusal asked once more
 * at the last moment.  The jump is caught (_copyandreboot longjmps out),
 * and each case that reaches it runs in its own process, since chainboot
 * keeps its state in statics that a real jump never comes back to.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "chainboot_stubs.h"
#include "chainboot.h"

/* ---- the stub platform -------------------------------------------------- */

static uint32_t now = 0x10000000u;
static bool     player_open, decoder_running;
static int      eject_fails;          /* filesystemEject answers false this often first */
static char     events[64];           /* M usb off, E/e eject step not done/done,
                                         I card back, S sdio, J jump */
static jmp_buf  jumped;
static uint8_t *jump_src;
static int      jump_len;
static uint8_t  jump_copy[256];
static uint32_t jump_at;
static void    *freed[8];
static int      nfreed;

static void ev(char c)
{
   size_t n = strlen(events);
   if (n + 1u < sizeof events) { events[n] = c; events[n + 1u] = '\0'; }
}

uint32_t RPI_GetSystemTime(void) { return now; }
void _disable_interrupts(void) { }
void disable_data_cache(void) { }
void RPI_ChainBootMark(void) { }
void sdio_runtime_prepare_for_warm_reboot(void) { ev('S'); }
void mtp_fs_prepare_for_warm_reboot(void) { ev('M'); }
bool videoplayer_active(void) { return player_open; }
bool h264dec_running(void) { return decoder_running; }

bool filesystemEject(void)
{
   if (eject_fails > 0) { eject_fails--; ev('E'); return false; }
   ev('e');
   return true;
}

bool filesystemInsert(void) { ev('I'); return true; }

void _copyandreboot(void *src, int num_bytes)
{
   ev('J');
   jump_src = src;
   jump_len = num_bytes;
   jump_at = now;
   memcpy(jump_copy, src, (size_t)num_bytes < sizeof jump_copy ? (size_t)num_bytes : sizeof jump_copy);
   longjmp(jumped, 1);
}

void test_free(void *p);
void test_free(void *p)
{
   if (p != NULL && nfreed < 8)
      freed[nfreed++] = p;
   free(p);
}

static bool was_freed(const void *p)
{
   for (int i = 0; i < nfreed; i++)
      if (freed[i] == p) return true;
   return false;
}

/* ---- helpers ------------------------------------------------------------ */

static int failures, checks;

#define CHECK(cond, ...) do { \
      checks++; \
      if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                     printf(__VA_ARGS__); printf("\n"); } \
   } while (0)

static bool req(uint8_t *image, uint32_t length, uint32_t capacity)
{
   return chainboot_request(image, length, capacity);
}

/* A kernel-shaped buffer: an ARM branch, then fill. */
static uint8_t *image(uint32_t capacity, uint8_t fill)
{
   uint8_t *p = malloc(capacity);
   memset(p, fill, capacity);
   p[0] = 0x06; p[1] = 0x00; p[2] = 0x00; p[3] = 0xEA;   /* B +0x20 */
   return p;
}

/* Main-loop passes, a millisecond apart, for up to ms: true if it jumped. */
static bool run(uint32_t ms)
{
   if (setjmp(jumped))
      return true;
   for (uint32_t i = 0; i < ms; i++) {
      chainboot_poll();
      now += 1000u;
   }
   return false;
}

/* Passes until the event c has happened (or ms runs out): true if it did. */
static bool run_until(char c, uint32_t ms)
{
   if (setjmp(jumped))
      return false;
   for (uint32_t i = 0; i < ms && strchr(events, c) == NULL; i++) {
      chainboot_poll();
      now += 1000u;
   }
   return strchr(events, c) != NULL;
}

/* Run a case in a child: chainboot's statics start fresh each time. */
static void in_child(void (*fn)(void), const char *name)
{
   fflush(stdout);
   pid_t pid = fork();
   if (pid == 0) {
      failures = 0;                  /* this case's own, not the parent's so far */
      fn();
      fflush(stdout);
      _exit(failures > 255 ? 255 : failures);
   }
   int status = 0;
   (void)waitpid(pid, &status, 0);
   checks++;
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      failures++;
      printf("FAIL case %s (%s %d)\n", name,
             WIFEXITED(status) ? "failed checks" : "died, signal",
             WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
   }
}

/* ---- tests -------------------------------------------------------------- */

static void test_image_ok(void)
{
   uint8_t k[8] = { 0x06, 0x00, 0x00, 0xEA, 0, 0, 0, 0 };
   uint8_t bad[8] = { 0x06, 0x00, 0x00, 0xEB, 0, 0, 0, 0 };
   CHECK(!chainboot_image_ok(NULL, 8u), "NULL accepted");
   CHECK(!chainboot_image_ok(k, 3u), "3 bytes accepted");
   CHECK(chainboot_image_ok(k, 4u), "4-byte branch refused");
   CHECK(!chainboot_image_ok(bad, 8u), "a BL taken for a kernel");
   CHECK(chainboot_image_ok(k, CHAINBOOT_MAX_IMAGE), "CHAINBOOT_MAX_IMAGE refused");
   CHECK(!chainboot_image_ok(k, CHAINBOOT_MAX_IMAGE + 1u), "over CHAINBOOT_MAX_IMAGE accepted");
}

static void test_refusal(void)
{
   player_open = decoder_running = false;
   CHECK(chainboot_refusal() == NULL, "refused with nothing running");
   player_open = true;
   CHECK(chainboot_refusal() != NULL, "not refused with the player open");
   player_open = false;
   decoder_running = true;
   CHECK(chainboot_refusal() != NULL, "not refused with the decoder holding GPU buffers");
   decoder_running = false;
}

/* Padded to 64 with zeros, and the steps in order: USB off after the
   settle, the card ejected, then the jump. */
static void case_pads_and_jumps(void)
{
   uint8_t *p = image(128u, 0xAA);
   uint32_t asked = now;
   CHECK(req(p, 100u, 128u), "100 bytes in 128 not taken");
   CHECK(run(2000u), "never jumped");
   CHECK(jump_src == p && jump_len == 128, "jumped with %p/%d, want %p/128", (void *)jump_src, jump_len, (void *)p);
   bool zero = true, kept = true;
   for (int i = 100; i < 128; i++) zero &= jump_copy[i] == 0u;
   for (int i = 4; i < 100; i++) kept &= jump_copy[i] == 0xAAu;
   CHECK(zero, "padding not zeroed");
   CHECK(kept, "image bytes changed");
   CHECK(strcmp(events, "MeSJ") == 0, "steps \"%s\", want \"MeSJ\"", events);
   CHECK(jump_at - asked >= 250000u, "jumped %u us after the request", (unsigned)(jump_at - asked));
   CHECK(nfreed == 0, "the image was freed before the jump");
}

/* A busy subsystem makes the eject wait: no jump until it is done, and no
   further eject once it is. */
static void case_eject_stepped(void)
{
   eject_fails = 3;
   CHECK(req(image(64u, 0x55), 64u, 64u), "64 bytes in 64 not taken");
   CHECK(run(2000u), "never jumped");
   CHECK(strcmp(events, "MEEEeSJ") == 0, "steps \"%s\", want \"MEEEeSJ\"", events);
}

/* Nothing to pad into: refused, and the buffer goes - never silently. */
static void case_over_capacity(void)
{
   uint8_t *p = image(100u, 0x11);
   CHECK(!req(p, 100u, 100u), "100 bytes taken with nowhere to pad to 128");
   CHECK(was_freed(p), "refused image not freed");
   CHECK(!req(NULL, 0u, 0u), "NULL taken");
   CHECK(!run(2000u), "jumped with nothing taken");
   CHECK(events[0] == '\0', "steps \"%s\" with nothing taken", events);
}

/* A second image replaces the first, which is freed. */
static void case_replace(void)
{
   uint8_t *a = image(64u, 0xA1);
   uint8_t *b = image(64u, 0xB2);
   CHECK(req(a, 64u, 64u), "first not taken");
   CHECK(req(b, 64u, 64u), "second not taken");
   CHECK(was_freed(a), "the first image was not freed when replaced");
   CHECK(run(2000u), "never jumped");
   CHECK(jump_src == b && jump_copy[10] == 0xB2u, "jumped into the wrong image");
}

/* The player or the decoder started while the jump was pending: the request
   is given up at the last moment - image freed, card given back - and a
   later one starts over from its settle. */
static void refused_at_jump(bool *flag)
{
   uint8_t *p = image(64u, 0x77);
   CHECK(req(p, 64u, 64u), "not taken");
   CHECK(run_until('e', 2000u), "card never ejected");
   *flag = true;
   CHECK(!run(2000u), "jumped although refused");
   CHECK(was_freed(p), "abandoned image not freed");
   CHECK(strchr(events, 'I') != NULL, "card not given back: steps \"%s\"", events);
   CHECK(strchr(events, 'S') == NULL, "sdio told to stop for a jump that was given up");

   *flag = false;
   events[0] = '\0';
   uint8_t *q = image(64u, 0x88);
   uint32_t asked = now;
   CHECK(req(q, 64u, 64u), "second request not taken");
   CHECK(run(2000u), "second request never jumped");
   CHECK(jump_src == q, "jumped into the wrong image");
   CHECK(strcmp(events, "MeSJ") == 0, "second steps \"%s\", want \"MeSJ\"", events);
   CHECK(jump_at - asked >= 250000u, "second jumped only %u us after its request", (unsigned)(jump_at - asked));
}

static void case_player_opened_at_jump(void) { refused_at_jump(&player_open); }
static void case_decoder_started_at_jump(void) { refused_at_jump(&decoder_running); }

int main(void)
{
   test_image_ok();
   test_refusal();
   in_child(case_pads_and_jumps, "pads_and_jumps");
   in_child(case_eject_stepped, "eject_stepped");
   in_child(case_over_capacity, "over_capacity");
   in_child(case_replace, "replace");
   in_child(case_player_opened_at_jump, "player_opened_at_jump");
   in_child(case_decoder_started_at_jump, "decoder_started_at_jump");
   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("CHAINBOOT TESTS PASSED\n");
   return failures ? 1 : 0;
}
