/* board.h - the Pi1MHz board around the firmware, for an emulated BBC: FRED and JIM, the
   &FC88 helper select, the FIQ, the main-loop poll and the config.  Included once, by the
   emulator's glue (test_ramprobe.c and bbcsim.c, both on kc-beeb),
   which supplies board_nmi_line() and calls board_bus_read(), board_bus_write(),
   board_reset() and board_poll().

   The one thing here that isn't the firmware's own is the &FC88 select: the logic of
   helpers_bank_select, with its timing as a parameter (helpers.c needs too much of the
   firmware to build here). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Pi1MHz.h"
#include "ramprobe.h"
#include "ramprobe_api.h"

/* ---- the board around the firmware --------------------------------------- */
static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;
uint32_t Pi1MHz_now_us;
uint32_t board_now;
static callback_func_ptr write_cb[256], read_cb[256];
static func_ptr poll_cb[4];
static int polls;
static uint8_t helper[47 * 256];
static uint32_t page_addr;
static bool window_is_helper;
static int pending = -1;
static uint32_t pending_at, select_delay;

void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr fn)
{
   if (access == WRITE_FRED) write_cb[addr & 0xffu] = fn; else read_cb[addr & 0xffu] = fn;
}
void Pi1MHz_Register_Poll(func_ptr fn, const char *name)
{
   (void)name;
   for (int i = 0; i < polls; i++) if (poll_cb[i] == fn) return;
   poll_cb[polls++] = fn;
}
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data) { pi.Memory[addr & 0x1ffu] = data; }
void Pi1MHz_MemoryWrite16(uint32_t addr, uint32_t data) { pi.Memory[addr & 0x1ffu] = (uint8_t)data; pi.Memory[(addr + 1u) & 0x1ffu] = (uint8_t)(data >> 8); }
static void board_nmi_line(bool low);   /* the emulator's: nNMI, low or not */
static void board_irq_line(bool low);   /* the emulator's: nIRQ, the Pi's share of it */
static uint32_t irq_sources;
void Pi1MHz_nIRQ_ASSERT(uint8_t src) { irq_sources |= 1u << src; board_irq_line(true); }
void Pi1MHz_nIRQ_CLEAR(uint8_t src) { irq_sources &= ~(1u << src); board_irq_line(irq_sources != 0u); }
void Pi1MHz_SetnNMI(bool nmi) { board_nmi_line(nmi); }
void fat_service_init(void) {}
#ifdef BOARD_EXTRA
static const char *board_config(const char *key);
#else
static const char *board_config(const char *key) { (void)key; return NULL; }
#endif
const char *config_get(const char *key) { return board_config(key); }
/* config.c's, for the values the tests use: absent or empty is dflt, 0/n/f/off false, else true. */
bool config_get_flag(const char *key, bool dflt)
{
   const char *v = config_get(key);
   if (v == NULL || v[0] == '\0') return dflt;
   return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F' || !strncasecmp(v, "off", 3));
}
int helpers_page_number(uint8_t page) { return page < 47 ? helper[(page << 8) + 255] : -1; }
static int window_page = -1;
int helpers_window_page(void) { return window_page; }

const uint8_t *helpers_page(uint8_t page) { return page < 47 ? helper + (page << 8) : NULL; }
void Pi1MHz_EmulatedMemoryByte(unsigned int gpio) { pi.Memory[GET_ADDR(gpio) & 0xffu] = (uint8_t)GET_DATA(gpio); }

/* The probe's pages, 22 and 23, from 6502code.bin as the firmware would load them
   (RAMPROBE_6502CODE names it), and its FRED code, at the end of the install page. */
static const uint8_t *rp_fred;
static uint8_t code_bin[47 * 256];
static void load_6502code(void)
{
   const char *path = getenv("RAMPROBE_6502CODE");
   FILE *f = path ? fopen(path, "rb") : NULL;
   if (f == NULL || fread(code_bin, 1, sizeof code_bin, f) != sizeof code_bin) {
      fprintf(stderr, "RAMPROBE_6502CODE: need the firmware's 6502code.bin, not %s\n", path ? path : "(unset)");
      exit(2);
   }
   fclose(f);
   const uint8_t *page = code_bin + (RP_HELPER_INSTALL << 8);
   if (page[255] != RP_HELPER_INSTALL) { fprintf(stderr, "6502code.bin: no probe install page\n"); exit(2); }
   rp_fred = page + RP_FRED_IN_PAGE;
}
void services_emulator_init(uint8_t instance, uint8_t address);

/* A request's body for the test servers: Content-Length bytes after the header, those that came
   with it first.  Its length, or -1 if there is none or it is too long for max. */
#include <unistd.h>
static long board_content_length(const char *req)
{
   const char *end = strstr(req, "\r\n\r\n");
   const char *cl = strcasestr(req, "\r\nContent-Length:");
   if (end == NULL || cl == NULL || cl > end) return -1;
   return strtol(cl + 17, NULL, 10);
}

static long board_read_body(int fd, const char *req, size_t got, uint8_t *body, size_t max)
{
   const char *end = strstr(req, "\r\n\r\n");
   long want = board_content_length(req);
   if (want <= 0 || (size_t)want > max) return -1;
   size_t have = got - (size_t)(end + 4 - req);
   if (have > (size_t)want) have = (size_t)want;
   memcpy(body, end + 4, have);
   while (have < (size_t)want) {
      ssize_t r = read(fd, body + have, (size_t)want - have);
      if (r <= 0) return -1;
      have += (size_t)r;
   }
   return want;
}

#ifndef BOARD_EXTRA
static void board_extra_init(void) {}
#else
static void board_extra_init(void);
#endif

/* helpers_bank_select, for a helper page or for &FF (JIM RAM again). */
static void apply_select(uint8_t data)
{
   if (data == 0xFF) {
      pi.Memory[0x88 + 4] = 0x60;
      memcpy(pi.Memory + 256, pi.JIM_ram + page_addr, 256);
      window_is_helper = false;
      window_page = -1;
   } else {
      pi.Memory[0x88 + 4] = 0x4C;
      if (data >= sizeof helper >> 8) data = 0;
      memcpy(pi.Memory + 256, helper + (data << 8), 256);
      window_is_helper = true;
      window_page = data;
   }
}
static void settle(void)
{
   if (pending >= 0 && (int32_t)(board_now - pending_at) >= 0) {
      apply_select((uint8_t)pending);
      pending = -1;
   }
}
/* The board answers the BBC at once from the byte it prepared, and runs the callback in its FIQ
   afterwards: a read straight after a write can see the old byte (docs/dev/bus-post-ring.md). */
static uint32_t fiq_delay;
static struct { uint32_t due; callback_func_ptr fn; unsigned gpio; } fiq_q[64];
static unsigned fiq_head, fiq_tail;
static void fiq_run(void)
{
   while (fiq_head != fiq_tail && (int32_t)(board_now - fiq_q[fiq_head].due) >= 0) {
      fiq_q[fiq_head].fn(fiq_q[fiq_head].gpio);
      fiq_head = (fiq_head + 1) % 64;
   }
}
static void fiq_post(callback_func_ptr fn, unsigned gpio)
{
   if (!fn) return;
   if (!fiq_delay) { fn(gpio); return; }
   fiq_q[fiq_tail].due = board_now + fiq_delay;
   fiq_q[fiq_tail].fn = fn;
   fiq_q[fiq_tail].gpio = gpio;
   fiq_tail = (fiq_tail + 1) % 64;
}
static uint8_t board_bus_read(uint16_t a)
{
   settle();
   fiq_run();
   if (a >= 0xFD00) return pi.Memory[256 + (a & 0xFF)];
   uint8_t v = pi.Memory[a & 0xFF];
   fiq_post(read_cb[a & 0xFF], TEST_GPIO(a & 0xFFu, 0));
   return v;
}
static void board_bus_write(uint16_t a, uint8_t v)
{
   settle();
   fiq_run();
   if (a >= 0xFD00) {
      pi.Memory[256 + (a & 0xFF)] = v;
      if (!window_is_helper) pi.JIM_ram[page_addr + (a & 0xFF)] = v;
      return;
   }
   uint8_t r = a & 0xFF;
   if (r == 0x88) {
      if (select_delay) { pending = v; pending_at = board_now + select_delay; }
      else apply_select(v);
      return;
   }
   if (r >= 0xFD) {                   /* the JIM page registers */
      int sh = r == 0xFF ? 8 : r == 0xFE ? 16 : 24;
      page_addr = ((page_addr & ~(0xFFu << sh)) | ((uint32_t)v << sh)) & 0xFFFFFF;
      window_is_helper = false;
      memcpy(pi.Memory + 256, pi.JIM_ram + page_addr, 256);
      return;
   }
   fiq_post(write_cb[r], TEST_GPIO(r, v));
}
static void board_reset(void)
{
   static const uint8_t code[7] = {0x8E, 0x88, 0xFC, 0xEA, 0x4C, 0x00, 0xFD};
   memset(pi.Memory, 0xFF, 256);
   memcpy(pi.Memory + 0x88, code, 7);
   memset(helper, 0, sizeof helper);
   for (int i = 0; i < 47; i++) { helper[i << 8] = 0xA2; helper[(i << 8) + 1] = 0xFF; helper[(i << 8) + 2] = 0x4C; helper[(i << 8) + 3] = 0x88; helper[(i << 8) + 4] = 0xFC; }
   if (rp_fred == NULL) load_6502code();
   for (unsigned i = RP_HELPER_AGENT; i <= RP_HELPER_INSTALL; i++) memcpy(helper + (i << 8), code_bin + (i << 8), 256);
   page_addr = 0;
   pending = -1;
   window_is_helper = false;
   window_page = -1;
   polls = 0;                          /* init_emulator empties the poll table at every reset */
   services_emulator_init(0, 0xA6);
   board_extra_init();
   ramprobe_init(0, 0);
}

/* A pass of the firmware's main loop, every 100 us of the emulated machine. */
static void board_poll(void)
{
   Pi1MHz_now_us += 100;
   for (int i = 0; i < polls; i++) poll_cb[i]();
}

