/* Host tests for secure_service.c - the FIQ latch and poll that put the
 * secure ABI on the services port.
 *
 * The real wrapper and the real ABI core run against a fake provider (the
 * nts_pi_wolfssh_* functions) and the services stubs' Pi1MHz.  The test plays
 * the FIQ itself by calling the registered command handler, including from
 * inside a provider call, which is where the Beeb's next command lands when a
 * dispatch is slow: ESCAPE abandons a waiting SSH_OPEN and issues SSH_CLOSE
 * while the handshake is still running. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "Pi1MHz.h"
#include "services.h"
#include "secure_service.h"
#include "secure_service_core.h"
#include "secure_service_wolfssh.h"

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
   printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
   else { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the Pi1MHz side ---------------------------------------------------- */
static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;

void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data) { pi.Memory[addr] = data; }
void Pi1MHz_MemoryWrite16(uint32_t addr, uint32_t data) { (void)addr; (void)data; }
void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr f)
{ (void)access; (void)addr; (void)f; }
void Pi1MHz_nIRQ_ASSERT(uint8_t src) { (void)src; }
void Pi1MHz_nIRQ_CLEAR(uint8_t src) { (void)src; }

static func_ptr poll_cb;
void Pi1MHz_Register_Poll(func_ptr function_ptr, const char *name)
{ (void)name; poll_cb = function_ptr; }

static service_command_fn fiq;
bool services_register(uint8_t first, uint8_t last, service_command_fn handler)
{ (void)first; (void)last; fiq = handler; return true; }

/* ---- the provider --------------------------------------------------------- */
#define REG   0xA0u                  /* the result register (FRED offset)   */
#define CMD   0x1000u                /* the command page, a JIM offset      */
#define URL   0x2000u
#define USER  0x2100u

static int  open_calls, close_calls;
static void (*during_open)(void);

static uint8_t fake_open(void *opaque, const char *url, const char *username,
                         int trust_unknown, char fingerprint[96])
{
   (void)opaque; (void)url; (void)username; (void)trust_unknown; (void)fingerprint;
   open_calls++;
   if (during_open) { void (*f)(void) = during_open; during_open = NULL; f(); }
   return NTS_PENDING;
}
static void fake_close(void *opaque) { (void)opaque; close_calls++; }

static const nts_secure_port fake_port = {
   .ssh_open = fake_open, .ssh_close = fake_close
};

const nts_secure_port *nts_pi_wolfssh_port(void) { return &fake_port; }
void *nts_pi_wolfssh_context(void) { return NULL; }
int nts_pi_wolfssh_ready(void) { return 1; }
int nts_pi_wolfssh_random_ready(void) { return 1; }
void nts_pi_wolfssh_poll(void) {}
void nts_pi_wolfssh_reset(void) {}

/* ---- helpers --------------------------------------------------------------- */
static uint8_t *jim(uint32_t a) { return &pi.JIM_ram[DISC_RAM_BASE + a]; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void issue_open(void)
{
   uint8_t *c = jim(CMD);
   memset(c, 0, 16);
   c[0] = NTS_SEC_SSH_OPEN;
   put32(c + 2, URL);
   put32(c + 6, USER);
   fiq(DISC_RAM_BASE + CMD, REG, 0);
}
static void issue_close(void)
{
   jim(CMD)[0] = NTS_SEC_SSH_CLOSE;
   fiq(DISC_RAM_BASE + CMD, REG, 0);
}

int main(void)
{
   pi.JIM_ram_size = 2;
   pi.JIM_ram = calloc((size_t)pi.JIM_ram_size * JIM_RAM_STEP, 1);
   strcpy((char *)jim(URL), "TCP://host.test:22");
   strcpy((char *)jim(USER), "user");

   secure_service_init(0, 0);
   poll_cb();                                   /* the deferred reset */

   printf("== a command is latched, then answered by the poll ==\n");
   issue_open();
   CHECK(pi.Memory[REG] == 0x80u, "BUSY after the FIQ (%02X)", pi.Memory[REG]);
   poll_cb();
   CHECK(open_calls == 1 && pi.Memory[REG] == NTS_PENDING,
         "the poll dispatched it once and wrote its result (%d calls, %02X)",
         open_calls, pi.Memory[REG]);
   poll_cb();
   CHECK(open_calls == 1, "and does not dispatch it again (%d calls)", open_calls);

   printf("== a command latched while another is being dispatched ==\n");
   open_calls = close_calls = 0;
   issue_open();
   during_open = issue_close;                   /* ESCAPE, then SSH_CLOSE */
   poll_cb();
   CHECK(pi.Memory[REG] == 0x80u,
         "the newer command's BUSY is not overwritten by the older result (%02X)",
         pi.Memory[REG]);
   CHECK(close_calls == 0, "the newer command is not run inside the older one");
   poll_cb();
   CHECK(close_calls == 1, "the newer command is not lost: SSH_CLOSE ran (%d)", close_calls);
   CHECK(pi.Memory[REG] == NTS_OK, "and answered OK (%02X)", pi.Memory[REG]);
   poll_cb();
   CHECK(close_calls == 1 && open_calls == 1, "nothing ran twice (open %d, close %d)",
         open_calls, close_calls);

   free(pi.JIM_ram);
   printf("%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
