/* Host tests for chainboot.c.
 *
 * What a chain-boot hands over and when it jumps are only seen on hardware
 * as "the Pi came back" or "the Pi went silent", so they are pinned here:
 * the image check; the hand-over, which pads the image to 64 bytes with
 * zeros inside its buffer, refuses (and frees) one that cannot be padded,
 * and lets a second image replace the first (with a settle of its own); and
 * the steps the main loop takes before the jump - a settle for the sender's
 * answer, USB off, the card ejected as the Beeb's reboot does, the video
 * shut down - with the refusal asked again before each step that would
 * cost the Beeb something, and the card given back only if this code took
 * it.  Video is never a refusal (the owner's call, 2026-10-06): it is shut
 * down before the jump instead, and the jump goes ahead even when that
 * shutdown does not come out clean.  The jump is caught (_copyandreboot
 * longjmps out), and each case that reaches it runs in its own process,
 * since chainboot keeps its state in statics that a real jump never comes
 * back to.
 *
 * Nothing refuses a chain-boot at present, but the give-up path stays for
 * the next refusal, so it is still pinned: built with REFUSAL_HOOK (and
 * chainboot.o's chainboot_refusal weakened - run_tests.sh), this file
 * supplies the refusal itself and runs only the give-up cases.  Each of
 * those checks that the give-up actually happened, so a hook that did not
 * take fails them rather than passing them unseen.
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
static bool     shutdown_ok = true;   /* what videoplayer_shutdown answers */
static bool     card_ejected;         /* filesystemEjected(): the user's own eject */
static bool     insert_fails;         /* filesystemInsert(): the card will not mount */
static int      eject_fails;          /* filesystemEject answers false this often first */
static char     events[64];           /* M usb off, U usb back, E/e eject step
                                         not done/done, I card back, V video
                                         shut down, S sdio, A audio DMA
                                         stopped, J jump */
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
void RPI_ChainBootMark(void) { }
void sdio_runtime_prepare_for_warm_reboot(void) { ev('S'); }
void audio_stop_dma(void) { ev('A'); }
void mtp_fs_prepare_for_warm_reboot(void) { ev('M'); }
void mtp_fs_inserted(void) { ev('U'); }
bool filesystemEjected(void) { return card_ejected; }
bool videoplayer_active(void) { return player_open; }
bool h264dec_running(void) { return decoder_running; }
bool videoplayer_shutdown(void) { ev('V'); return shutdown_ok; }

#ifdef REFUSAL_HOOK
/* Stands in for chainboot.c's own, which run_tests.sh weakened. */
static const char *refusal;
const char *chainboot_refusal(void) { return refusal; }
#define REFUSE (refusal = "refused for the test")
#define ALLOW  (refusal = NULL)
#endif

bool filesystemEject(void)
{
   if (eject_fails > 0) { eject_fails--; ev('E'); return false; }
   ev('e');
   card_ejected = true;
   return true;
}

bool filesystemInsert(void)
{
   ev('I');
   if (insert_fails) return false;   /* filesystemReset leaves it ejected */
   card_ejected = false;
   return true;
}

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

#ifndef REFUSAL_HOOK
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

/* Video is shut down before the jump, never a reason to refuse it. */
static void test_refusal(void)
{
   player_open = decoder_running = false;
   CHECK(chainboot_refusal() == NULL, "refused with nothing running");
   player_open = true;
   decoder_running = true;
   CHECK(chainboot_refusal() == NULL, "refused with the player open");
   player_open = false;
   CHECK(chainboot_refusal() == NULL, "refused once the decoder has been started");
   decoder_running = false;
}

/* Padded to 64 with zeros, and the steps in order: USB off after the
   settle, the card ejected, the video shut down (after the eject, so no
   F-code can bring the player back up in between: there is no card to
   open a video from), the WiFi chip quietened, the audio DMA stopped (the
   copy may run over its control blocks), then the jump. */
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
   CHECK(strcmp(events, "MeVSAJ") == 0, "steps \"%s\", want \"MeVSAJ\"", events);
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
   CHECK(strcmp(events, "MEEEeVSAJ") == 0, "steps \"%s\", want \"MEEEeVSAJ\"", events);
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

/* The player open, the decoder started - or opened while the jump waits:
   none of it holds the jump up any more.  It is shut down just before. */
static void case_video_in_use_still_jumps(void)
{
   player_open = decoder_running = true;
   uint8_t *p = image(64u, 0x71);
   CHECK(req(p, 64u, 64u), "not taken with the player open");
   CHECK(run(2000u), "never jumped with the player open");
   CHECK(jump_src == p, "jumped into the wrong image");
   CHECK(strcmp(events, "MeVSAJ") == 0, "steps \"%s\", want \"MeVSAJ\"", events);
   CHECK(nfreed == 0, "the image was freed before the jump");
}

static void case_video_opened_while_pending(void)
{
   uint8_t *p = image(64u, 0x72);
   CHECK(req(p, 64u, 64u), "not taken");
   CHECK(run_until('e', 2000u), "card never ejected");
   player_open = decoder_running = true;
   CHECK(run(2000u), "never jumped after the player opened");
   CHECK(jump_src == p, "jumped into the wrong image");
   CHECK(strcmp(events, "MeVSAJ") == 0, "steps \"%s\", want \"MeVSAJ\"", events);
}

/* A shutdown that did not come out clean (a VideoCore that stopped
   answering) is never a reason not to jump: what it leaves is leaked. */
static void case_shutdown_fails_still_jumps(void)
{
   shutdown_ok = false;
   player_open = decoder_running = true;
   uint8_t *p = image(64u, 0x73);
   CHECK(req(p, 64u, 64u), "not taken");
   CHECK(run(2000u), "no jump after a failed video shutdown");
   CHECK(jump_src == p, "jumped into the wrong image");
   CHECK(strcmp(events, "MeVSAJ") == 0, "steps \"%s\", want \"MeVSAJ\"", events);
}
#endif

#ifdef REFUSAL_HOOK
/* Refused while the jump was pending, after the card had gone: the request
   is given up at the last moment - image freed, card given back (this code
   took it), the video left alone - and a later one starts over from its
   settle. */
static void case_refused_at_jump(void)
{
   uint8_t *p = image(64u, 0x77);
   CHECK(req(p, 64u, 64u), "not taken");
   CHECK(run_until('e', 2000u), "card never ejected");
   REFUSE;
   CHECK(!run(2000u), "jumped although refused");
   CHECK(was_freed(p), "abandoned image not freed");
   CHECK(strchr(events, 'I') != NULL, "card not given back: steps \"%s\"", events);
   CHECK(strchr(events, 'S') == NULL, "sdio told to stop for a jump that was given up");
   CHECK(strchr(events, 'A') == NULL, "audio stopped for a jump that was given up");
   CHECK(strchr(events, 'V') == NULL, "video shut down for a jump that was given up");

   ALLOW;
   events[0] = '\0';
   uint8_t *q = image(64u, 0x88);
   uint32_t asked = now;
   CHECK(req(q, 64u, 64u), "second request not taken");
   CHECK(run(2000u), "second request never jumped");
   CHECK(jump_src == q, "jumped into the wrong image");
   CHECK(strcmp(events, "MeVSAJ") == 0, "second steps \"%s\", want \"MeVSAJ\"", events);
   CHECK(jump_at - asked >= 250000u, "second jumped only %u us after its request", (unsigned)(jump_at - asked));
}


/* Refused before anything was touched: given up during the settle, and the
   Pi carries on exactly as it was - USB never left the bus, the card never
   went. */
static void case_refused_in_settle(void)
{
   uint8_t *p = image(64u, 0x31);
   CHECK(req(p, 64u, 64u), "not taken");
   CHECK(!run(100u), "jumped inside the settle");
   REFUSE;
   CHECK(!run(2000u), "jumped although refused");
   CHECK(was_freed(p), "abandoned image not freed");
   CHECK(events[0] == '\0', "steps \"%s\", want none", events);
}

/* Refused after USB went off the bus but before the first eject step: given
   up there, while it costs nothing but a USB reconnect - the Beeb keeps its
   mounted discs (an eject stops every LUN, and putting the card back does not
   restart them). */
static void case_refused_before_eject(void)
{
   uint8_t *p = image(64u, 0x32);
   CHECK(req(p, 64u, 64u), "not taken");
   CHECK(run_until('M', 2000u), "USB never taken off the bus");
   REFUSE;
   CHECK(!run(2000u), "jumped although refused");
   CHECK(was_freed(p), "abandoned image not freed");
   CHECK(strcmp(events, "MU") == 0, "steps \"%s\", want \"MU\"", events);
}

/* The user had already ejected the card: the jump's eject is a no-op, and a
   late refusal must not mount whatever is in the slot. */
static void case_user_ejected_card_stays_out(void)
{
   card_ejected = true;
   uint8_t *p = image(64u, 0x33);
   CHECK(req(p, 64u, 64u), "not taken");
   CHECK(run_until('e', 2000u), "eject step never ran");
   REFUSE;
   CHECK(!run(2000u), "jumped although refused");
   CHECK(was_freed(p), "abandoned image not freed");
   CHECK(strchr(events, 'I') == NULL, "card the user ejected was mounted: steps \"%s\"", events);
   CHECK(strchr(events, 'U') == NULL, "USB back with no card: steps \"%s\"", events);
   CHECK(card_ejected, "card no longer ejected");
}

/* Given up after the eject, and the card will not mount again: left
   ejected, as a failed insert from the Beeb leaves it - USB not forced back
   with no card behind it. */
static void case_reinsert_fails(void)
{
   insert_fails = true;
   uint8_t *p = image(64u, 0x61);
   CHECK(req(p, 64u, 64u), "not taken");
   CHECK(run_until('e', 2000u), "card never ejected");
   REFUSE;
   CHECK(!run(2000u), "jumped although refused");
   CHECK(was_freed(p), "abandoned image not freed");
   CHECK(strcmp(events, "MeI") == 0, "steps \"%s\", want \"MeI\"", events);
   CHECK(card_ejected, "card not left ejected");
   events[0] = '\0';
   CHECK(!run(2000u), "something ran with nothing taken");
   CHECK(events[0] == '\0', "steps \"%s\" after the give-up", events);
}

/* A second request arrives after this code took the card, and is refused
   during its own settle: the card taken for the first still goes back. */
static void case_second_request_after_eject_refused(void)
{
   CHECK(req(image(64u, 0x51), 64u, 64u), "first not taken");
   CHECK(run_until('e', 2000u), "card never ejected");
   uint8_t *b = image(64u, 0x52);
   CHECK(req(b, 64u, 64u), "second not taken");
   REFUSE;
   CHECK(!run(2000u), "jumped although refused");
   CHECK(was_freed(b), "abandoned image not freed");
   CHECK(strchr(events, 'I') != NULL, "card taken for the first request kept: steps \"%s\"", events);
   CHECK(!card_ejected, "card still ejected");
}

#endif /* REFUSAL_HOOK */

#ifndef REFUSAL_HOOK
/* A second request while the first waits its settle: the settle starts over,
   so the second sender's answer gets out too. */
static void case_second_request_resettles(void)
{
   CHECK(req(image(64u, 0x41), 64u, 64u), "first not taken");
   CHECK(!run(150u), "jumped inside the first settle");
   uint8_t *b = image(64u, 0x42);
   uint32_t asked = now;
   CHECK(req(b, 64u, 64u), "second not taken");
   CHECK(run(2000u), "never jumped");
   CHECK(jump_src == b, "jumped into the wrong image");
   CHECK(jump_at - asked >= 250000u, "jumped only %u us after the second request", (unsigned)(jump_at - asked));
   CHECK(strcmp(events, "MeVSAJ") == 0, "steps \"%s\", want \"MeVSAJ\"", events);
}
#endif

int main(void)
{
#ifndef REFUSAL_HOOK
   test_image_ok();
   test_refusal();
   in_child(case_pads_and_jumps, "pads_and_jumps");
   in_child(case_eject_stepped, "eject_stepped");
   in_child(case_over_capacity, "over_capacity");
   in_child(case_replace, "replace");
   in_child(case_second_request_resettles, "second_request_resettles");
   in_child(case_video_in_use_still_jumps, "video_in_use_still_jumps");
   in_child(case_video_opened_while_pending, "video_opened_while_pending");
   in_child(case_shutdown_fails_still_jumps, "shutdown_fails_still_jumps");
#else
   in_child(case_refused_at_jump, "refused_at_jump");
   in_child(case_refused_in_settle, "refused_in_settle");
   in_child(case_refused_before_eject, "refused_before_eject");
   in_child(case_user_ejected_card_stays_out, "user_ejected_card_stays_out");
   in_child(case_second_request_after_eject_refused, "second_request_after_eject_refused");
   in_child(case_reinsert_fails, "reinsert_fails");
#endif
   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("CHAINBOOT TESTS PASSED\n");
   return failures ? 1 : 0;
}
