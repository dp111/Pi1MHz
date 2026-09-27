/* Host test for the FujiNet service's latch (fujibus_service.c): the FIQ
 * callback latches a ring, the poll answers it - re-running a pending
 * request from a copy taken on its first run, aborting it when the Beeb
 * rings again, and publishing a result only if no newer ring arrived.
 * fujibus_answer is a scripted fake here; the devices have their own tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Pi1MHz.h"
#include "services.h"
#include "fujibus_service.h"
#include "fujinet/fujibus.h"

static int s_fail, s_pass;
#define CHECK(c, ...) do { if (c) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- Pi1MHz / services stubs ---- */
static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;
static func_ptr poll_cb;
void Pi1MHz_Register_Poll(func_ptr fn, const char *name) { (void)name; poll_cb = fn; }
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data) { pi.Memory[addr & 0x1ff] = data; }
static service_command_fn fiq;
bool services_register(uint8_t first, uint8_t last, service_command_fn h)
{ (void)first; (void)last; fiq = h; return true; }
const char *config_get(const char *key) { (void)key; return NULL; }

/* ---- the rest of the FujiNet stack, faked ---- */
static int aborts, store_polls;
void fn_store_poll(void) { store_polls++; }
void fn_store_request_abort(void) { aborts++; }
void fn_network_poll(void) {}
void fn_network_reset(void) {}
void fn_disk_set_boot(const char *uri, bool ro) { (void)uri; (void)ro; }
bool fn_disk_uses_path(const char *p) { (void)p; return false; }

static int pend_left;               /* PENDING answers before a reply */
static uint8_t seen[64];            /* the request as the device saw it */
static uint16_t seen_len;
static int calls, mismatches;
static void (*during)(void);        /* run inside an answer, once */

fb_answer fujibus_answer(const uint8_t *req, uint16_t req_len,
                         uint8_t *reply, uint16_t reply_cap, uint16_t *reply_len)
{
   if (calls++ == 0 || seen_len == 0) {
      memcpy(seen, req, req_len);
      seen_len = req_len;
   } else if (req_len != seen_len || memcmp(req, seen, req_len) != 0)
      mismatches++;                 /* a re-run saw a different request */
   if (during) { void (*f)(void) = during; during = NULL; f(); }
   if (pend_left > 0) { pend_left--; return FB_ANSWER_PENDING; }
   if (reply_cap < 3) return FB_ANSWER_NONE;
   reply[0] = 0xAA; reply[1] = req[0]; reply[2] = (uint8_t)req_len;
   *reply_len = 3;
   return FB_ANSWER_REPLY;
}

/* ---- the Beeb side ---- */
#define REG    0xAAu                /* the command register the FIQ writes */
#define BLKPG  0xF0u
static uint32_t blk(void) { return DISC_RAM_BASE | 0xFF0000u | (BLKPG << 8); }
static uint8_t *jim(uint32_t off) { return &pi.JIM_ram[DISC_RAM_BASE + off]; }

static void build(const char *msg, uint32_t req_off, uint32_t rep_off)
{
   uint16_t n = (uint16_t)strlen(msg);
   memcpy(jim(req_off), msg, n);
   uint8_t *b = &pi.JIM_ram[blk()];
   b[0] = 114;
   b[1] = (uint8_t)req_off; b[2] = (uint8_t)(req_off >> 8); b[3] = (uint8_t)(req_off >> 16);
   b[4] = (uint8_t)n; b[5] = (uint8_t)(n >> 8);
   b[6] = (uint8_t)rep_off; b[7] = (uint8_t)(rep_off >> 8); b[8] = (uint8_t)(rep_off >> 16);
   b[9] = 0x00; b[10] = 0x08;       /* capacity &0800 */
   b[11] = b[12] = 0;
}
static void ring(void) { fiq(blk(), REG, BLKPG); }
static uint8_t reg(void) { return pi.Memory[REG]; }
static uint16_t rep_len(void) { uint8_t *b = &pi.JIM_ram[blk()]; return (uint16_t)(b[11] | b[12] << 8); }
static void reset_fake(void) { pend_left = 0; calls = 0; seen_len = 0; mismatches = 0; during = NULL; aborts = 0; }

static void ring_again_with(void) { build("SECOND", 0, 0x800); ring(); }

int main(void)
{
   pi.JIM_ram_size = 2;
   pi.JIM_ram = calloc((size_t)pi.JIM_ram_size * JIM_RAM_STEP, 1);
   fujibus_service_init(0, 0xA6);

   /* 1. answered on the first pass */
   reset_fake();
   build("HELLO", 0, 0x800);
   ring();
   CHECK(reg() == (BLKPG | 0x80u), "busy after the ring (%02X)", reg());
   poll_cb();
   CHECK(reg() == 0 && rep_len() == 3 && jim(0x800)[0] == 0xAA && jim(0x800)[1] == 'H',
         "answered: result %u, length %u", reg(), rep_len());

   /* 2. pending: busy stays set; re-runs see the request as it was, even
      after the Beeb starts rewriting its buffer */
   reset_fake();
   pend_left = 2;
   build("PENDING", 0, 0x800);
   ring();
   poll_cb();
   CHECK(reg() == (BLKPG | 0x80u), "still busy while pending");
   memcpy(jim(0), "XXXXXXX", 7);    /* the Beeb scribbles on its buffer */
   poll_cb();
   poll_cb();
   CHECK(reg() == 0 && calls == 3, "answered after two pending passes (%d calls)", calls);
   CHECK(mismatches == 0, "every re-run saw the original request (%d differed)", mismatches);
   CHECK(jim(0x800)[1] == 'P', "the answer is to the original request");

   /* 3. replaced while pending: the old request is aborted, the new one
      answered, and the old answer never published */
   reset_fake();
   pend_left = 1000;
   build("OLD", 0, 0x800);
   ring();
   poll_cb();
   CHECK(reg() == (BLKPG | 0x80u) && aborts == 0, "old pending");
   pend_left = 0;
   seen_len = 0;                    /* the fake learns the new request */
   build("NEW!", 0, 0x800);
   ring();
   poll_cb();
   CHECK(aborts == 1, "the old request was aborted once (%d)", aborts);
   CHECK(reg() == 0 && jim(0x800)[1] == 'N' && jim(0x800)[2] == 4, "the new request answered");

   /* 4. a ring arriving while the poll is answering: the finished answer
      is not published over it, and the new ring is answered next pass */
   reset_fake();
   build("FIRST", 0, 0x800);
   during = ring_again_with;        /* rings "SECOND" mid-answer */
   ring();
   poll_cb();
   CHECK(reg() == (BLKPG | 0x80u), "the second ring's busy was not overwritten (%02X)", reg());
   CHECK(rep_len() == 0, "and the first answer's length was not published (%u)", rep_len());
   seen_len = 0;
   poll_cb();
   CHECK(reg() == 0 && jim(0x800)[1] == 'S' && jim(0x800)[2] == 6, "the second request answered next");

   /* 5. a block whose request and reply overlap is refused */
   reset_fake();
   build("OVERLAP", 0, 0x4);
   ring();
   poll_cb();
   CHECK(reg() == 1 && calls == 0, "bad block: result %u, device not called", reg());

   /* 6. a non-114 command in the range */
   reset_fake();
   build("X", 0, 0x800);
   pi.JIM_ram[blk()] = 115;
   ring();
   poll_cb();
   CHECK(reg() == 3, "reserved command: result %u", reg());

   free(pi.JIM_ram);
   printf("%d checks, %d failures\n", s_pass + s_fail, s_fail);
   return s_fail ? 1 : 0;
}
