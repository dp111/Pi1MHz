/* fujibus_service.c - the FujiNet device on the services port.

   fn-rom's 1MHz link hands whole FujiBus packets to the Pi here (see
   docs/dev/fujinet-device.md).  Command 114, FUJIBUS_EXCHANGE, with its
   command block (normally page &F0, &FFF000):

      block[0]      114
      block[1..3]   request offset in the buffer (24-bit, low first)
      block[4..5]   request length
      block[6..8]   reply offset
      block[9..10]  reply capacity
      -> block[11..12] reply length

   The result, read back from the command register once bit 7 clears:
      0  a reply is in the buffer (refusals included - its status says so)
      1  the block's offsets are outside the buffer or overlap
      2  no reply: the packet was malformed, as a serial device would drop it
      3  a reserved command in 115-119

   The FIQ handler only latches; fujibus_service_poll answers on the main
   loop, where the devices may reach the SD card.  A request waiting on a
   TNFS server (fujinet/fn_store.h) keeps the register busy and is run again
   on each later pass until it is answered; if the Beeb gives up and sends a
   new request meanwhile, the old one's work is abandoned. */

#include "Pi1MHz.h"
#include "config.h"
#include "services.h"
#include "fujibus_service.h"
#include "fujinet/fujibus.h"
#include "fujinet/fn_disk.h"
#include "fujinet/fn_store.h"
#include "fujinet/fn_tnfs.h"

#define FUJI_CMD_EXCHANGE   114u

#define FUJI_RES_OK         0u
#define FUJI_RES_BAD_BLOCK  1u
#define FUJI_RES_NO_REPLY   2u
#define FUJI_RES_UNKNOWN    3u
#define FUJI_RES_PENDING    0xFFu    /* internal: not answered yet */

static volatile bool     fuji_pending;
static volatile bool     fuji_replaced;    /* a new request arrived while one waited */
static volatile uint32_t fuji_pending_cp;
static volatile uint32_t fuji_pending_addr;

static void fujibus_service_command(uint32_t command_pointer, uint32_t addr, uint8_t data)
{
   /* FIQ context: latch only, and read busy until the poll answers. */
   if (fuji_pending)
      fuji_replaced = true;
   fuji_pending_cp   = command_pointer;
   fuji_pending_addr = addr;
   fuji_pending      = true;
   Pi1MHz_MemoryWrite(addr, (uint8_t)(data | 0x80u));
}

static uint32_t get24(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Do [a, a+an) and [b, b+bn) share a byte? */
static bool overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
   return an && bn && a < b + bn && b < a + an;
}

static uint8_t fujibus_execute(uint32_t cp)
{
   uint8_t *blk = &Pi1MHz->JIM_ram[cp];
   if (blk[0] != FUJI_CMD_EXCHANGE)
      return FUJI_RES_UNKNOWN;

   uint32_t req_off = get24(blk + 1), rep_off = get24(blk + 6);
   uint16_t req_len = get16(blk + 4), rep_cap = get16(blk + 9);
   uint32_t blk_off = cp - DISC_RAM_BASE;
   if (!service_buffer_ok(req_off, req_len) || !service_buffer_ok(rep_off, rep_cap) ||
       overlaps(req_off, req_len, rep_off, rep_cap) ||
       overlaps(req_off, req_len, blk_off, 13u) || overlaps(rep_off, rep_cap, blk_off, 13u))
      return FUJI_RES_BAD_BLOCK;

   uint16_t rep_len = 0;
   switch (fujibus_answer(&Pi1MHz->JIM_ram[DISC_RAM_BASE + req_off], req_len,
                          &Pi1MHz->JIM_ram[DISC_RAM_BASE + rep_off], rep_cap, &rep_len)) {
   case FB_ANSWER_NONE:    return FUJI_RES_NO_REPLY;
   case FB_ANSWER_PENDING: return FUJI_RES_PENDING;
   case FB_ANSWER_REPLY:   break;
   }
   blk[11] = (uint8_t)rep_len;
   blk[12] = (uint8_t)(rep_len >> 8);
   return FUJI_RES_OK;
}

static void fujibus_service_poll(void)
{
   fn_store_poll(fn_tnfs_io_now_ms());     /* TNFS resends and timeouts */
   if (!fuji_pending)
      return;
   if (fuji_replaced) {
      fuji_replaced = false;
      fn_store_request_abort();
   }
   uint32_t cp = fuji_pending_cp;
   uint32_t addr = fuji_pending_addr;
   uint8_t result = fujibus_execute(cp);
   if (result == FUJI_RES_PENDING)
      return;                               /* busy stays set; run it again next pass */
   fuji_pending = false;
   Pi1MHz_MemoryWrite(addr, result);
}

bool fujibus_service_path_busy(const char *host_path)
{
   return fn_disk_uses_path(host_path);
}

void fujibus_service_init(uint8_t instance, uint8_t address)
{
   (void)instance;
   (void)address;
   /* Runs on every BBC reset.  Mounted images stay mounted - a real FujiNet
      keeps its state across the host's BREAK, and fn-rom starts a new
      session (BeginHostSession) itself when it wants one. */
   const char *boot = config_get("fujinet_boot");
   fn_disk_set_boot(boot ? boot : "", true);
   (void)services_register(SERVICE_CMD_FUJI_FIRST, SERVICE_CMD_FUJI_LAST,
                           fujibus_service_command);
   Pi1MHz_Register_Poll(fujibus_service_poll, "fujinet");
}
