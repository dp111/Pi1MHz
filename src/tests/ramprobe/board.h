/* board.h - the Pi1MHz board around the firmware, for an emulated BBC: FRED and JIM as the
   board serves them, the FIQ, the main-loop poll, the SD card's 6502code.bin and the config.
   Included once, by the emulator's glue (test_ramprobe.c and bbcsim.c, both on kc-beeb),
   which supplies board_nmi_line() and calls board_bus_read(), board_bus_write(),
   board_reset() and board_poll().

   The firmware's own code does the rest: the &FC88 select is helpers.c's, and the &FD00
   window and JIM page registers are ram_emulator.c's.  The one thing added is a delay before
   a select takes effect, as a parameter (select_delay). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Pi1MHz.h"
#include "ramprobe.h"
#include "ramprobe_api.h"
#include "ram_emulator.h"

/* ---- the board around the firmware --------------------------------------- */
static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;
uint32_t Pi1MHz_now_us;
uint32_t board_now;
static callback_func_ptr write_cb[256], read_cb[256], jim_cb[256];
static func_ptr poll_cb[8];
static int polls;
static uint32_t select_delay;

void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr fn)
{
   if (access == WRITE_FRED) write_cb[addr & 0xffu] = fn;
   else if (access == WRITE_JIM) jim_cb[addr & 0xffu] = fn;
   else read_cb[addr & 0xffu] = fn;
}
void Pi1MHz_Register_Poll(func_ptr fn, const char *name)
{
   (void)name;
   for (int i = 0; i < polls; i++) if (poll_cb[i] == fn) return;
   poll_cb[polls++] = fn;
}
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data) { pi.Memory[addr & 0x1ffu] = data; }
void Pi1MHz_MemoryWrite_FIQ(uint32_t addr, uint8_t data) { pi.Memory[addr & 0x1ffu] = data; }
void Pi1MHz_MemoryWritePage(uint32_t addr, const void *data) { memcpy(pi.Memory + (addr & 0x100u), data, 256); }
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
/* What helpers.c and ram_emulator.c need of the rest of the firmware. */
volatile Pi1MHz_break_t Pi1MHz_break;
uint8_t fx_register[256];
uint32_t RPI_GetSystemTime(void) { return Pi1MHz_now_us; }
uint8_t harddisc_emulator_get_address(void) { return 0; }
uint8_t harddisc_emulator_get_instance(void) { return 0; }
uint8_t M5000_emulator_read_instance(void) { return 0; }
bool videoplayer_active(void) { return false; }
uint8_t serial_redirect_address(void) { return 0; }
char *get_info_string(void) { return "host"; }
uint32_t get_temp_millidegrees(void) { return 40000; }
bool info_refresh_cached(void) { return true; }
void fat_service_wild_leaves(const char *dir, const char *const *pats, unsigned int n, char *out, size_t size)
{
   (void)dir; (void)pats;
   for (unsigned i = 0; i < n; i++) out[i * size] = '\0';
}
/* ram_emulator.c sizes JIM RAM as the memory past the program's end (_end) less 4 MB, in 16 MB
   steps: 32 MB here.  run_tests.sh links with -no-pie, so _end is low and fixed and the sum
   doesn't wrap in 32 bits. */
extern char _end;
uint32_t mem_info(int size) { (void)size; return (uint32_t)(uintptr_t)&_end + 4u * 1024u * 1024u + 2u * JIM_RAM_STEP; }
extern uint8_t helper_ram[];          /* helpers.c's: the pages it loaded from 6502code.bin */
void Pi1MHz_EmulatedMemoryByte(unsigned int gpio) { pi.Memory[GET_ADDR(gpio) & 0xffu] = (uint8_t)GET_DATA(gpio); }

/* The SD card's 6502code.bin (RAMPROBE_6502CODE names it), for helpers.c to load. */
static const uint8_t *rp_fred;
static uint8_t code_bin[47 * 256];
uint32_t filesystemReadFile(const char *filename, uint8_t **address, unsigned int max_size)
{
   if (strcmp(filename, "Pi1MHz/6502code.bin") != 0) return 0;
   size_t n = sizeof code_bin < max_size ? sizeof code_bin : max_size;
   memcpy(*address, code_bin, n);
   return (uint32_t)n;
}
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

/* The board answers the BBC at once from the byte it prepared, and runs the callback in its FIQ
   afterwards, in bus order: a read straight after a write can see the old byte
   (docs/dev/bus-post-ring.md).  fiq_delay is how late the FIQ runs; select_delay is how much
   longer a helper select takes, and what comes after it on the bus waits for it. */
static uint32_t fiq_delay;
#define FIQ_Q 1024u
static struct { uint32_t due; callback_func_ptr fn; unsigned gpio; } fiq_q[FIQ_Q];
static unsigned fiq_head, fiq_tail;
static void fiq_run(void)
{
   while (fiq_head != fiq_tail && (int32_t)(board_now - fiq_q[fiq_head].due) >= 0) {
      fiq_q[fiq_head].fn(fiq_q[fiq_head].gpio);
      fiq_head = (fiq_head + 1) % FIQ_Q;
   }
}
static void fiq_post_after(callback_func_ptr fn, unsigned gpio, uint32_t extra)
{
   if (!fn) return;
   if (!fiq_delay && !extra && fiq_head == fiq_tail) { fn(gpio); return; }
   if ((fiq_tail + 1) % FIQ_Q == fiq_head) { fprintf(stderr, "board: FIQ queue full\n"); exit(2); }
   fiq_q[fiq_tail].due = board_now + fiq_delay + extra;
   fiq_q[fiq_tail].fn = fn;
   fiq_q[fiq_tail].gpio = gpio;
   fiq_tail = (fiq_tail + 1) % FIQ_Q;
}
static void fiq_post(callback_func_ptr fn, unsigned gpio) { fiq_post_after(fn, gpio, 0); }
static uint8_t board_bus_read(uint16_t a)
{
   fiq_run();
   if (a >= 0xFD00) return pi.Memory[256 + (a & 0xFF)];
   uint8_t v = pi.Memory[a & 0xFF];
   fiq_post(read_cb[a & 0xFF], TEST_GPIO(a & 0xFFu, 0));
   return v;
}
static void board_bus_write(uint16_t a, uint8_t v)
{
   fiq_run();
   if (a >= 0xFD00) {                 /* the window: ram_emulator_page_write */
      pi.Memory[256 + (a & 0xFF)] = v;
      fiq_post(jim_cb[a & 0xFF], TEST_GPIO(a & 0xFFu, v));
      return;
   }
   uint8_t r = a & 0xFF;
   fiq_post_after(write_cb[r], TEST_GPIO(r, v), r == 0x88 ? select_delay : 0u);
}
void rampage_emulator_init(uint8_t instance, uint8_t address);
void helpers_init(uint8_t instance, uint8_t address);
/* A BBC reset: init_emulator's table, for the emulators here, in its order. */
static void board_reset(void)
{
   memset(pi.Memory, 0xFF, 256);
   memset(write_cb, 0, sizeof write_cb);
   memset(read_cb, 0, sizeof read_cb);
   memset(jim_cb, 0, sizeof jim_cb);
   if (rp_fred == NULL) load_6502code();
   fiq_head = fiq_tail = 0;            /* a reset: whatever was in flight is gone */
   polls = 0;                          /* init_emulator empties the poll table at every reset */
   rampage_emulator_init(0, 0xFD);
   services_emulator_init(0, 0xA6);
   board_extra_init();
   helpers_init(0, 0x88);
   ramprobe_init(0, 0);
}

/* A pass of the firmware's main loop, every 100 us of the emulated machine. */
static void board_poll(void)
{
   Pi1MHz_now_us += 100;
   for (int i = 0; i < polls; i++) poll_cb[i]();
}

