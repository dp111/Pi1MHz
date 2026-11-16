/*
 * Wired Ethernet through the Pi 3 Model B+'s USB Ethernet chip (LAN7515,
 * USB ID 0424:7800, the LAN7800 register set).  See usb_ether.h and
 * docs/dev/ethernet-pi3-plan.md.
 *
 * Register addresses and bits are the LAN7800's (Microchip datasheet
 * DS00001992).  Written from the datasheet's register map - not from Linux's
 * lan78xx driver, which is GPLv2-only.
 *
 * Shape:
 *   - TinyUSB offers us each interface first (an app driver); we claim the
 *     LAN7800's vendor interface and open its bulk endpoints.
 *   - Setting the chip up is a table of register operations (init_script),
 *     each a non-blocking control transfer, stepped from usb_ether_poll(): a
 *     reset that takes 100 ms costs the main loop nothing.
 *   - Receive: one bulk IN transfer is kept queued while the previous one has
 *     been read out.  The chip is told to answer an empty IN with a zero-length
 *     packet rather than NAK (USB_CFG0.BIR), so the polled host controller is
 *     not left retrying a NAKing endpoint; after an empty answer the next one is
 *     asked for ETHER_RX_IDLE_US later.  One transfer can hold several frames
 *     (HW_CFG.MEF, burst cap), each behind a 10-byte header.
 *   - Transmit: a small ring of frames, each behind an 8-byte command header,
 *     sent one bulk OUT transfer at a time.
 *   - Link: the internal PHY's status register, read about once a second.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "usb/tusb_config.h"
#include <tusb.h>
#include "host/usbh_pvt.h"

#include "rpi/info.h"
#include "rpi/mailbox.h"
#include "rpi/systimer.h"
#include "usb_ether.h"

/* ---- the chip ----------------------------------------------------------- */

#define LAN78XX_VID          0x0424u
#define LAN7800_PID          0x7800u   /* LAN7800, and the LAN7515 on a Pi 3B+ */

#define REQ_WRITE_REGISTER   0xA0u
#define REQ_READ_REGISTER    0xA1u

#define ID_REV               0x000u
#define  ID_REV_CHIP_ID_MASK 0xFFFF0000u
#define INT_STS              0x00Cu
#define HW_CFG               0x010u
#define  HW_CFG_LED1_EN      (1u << 21)
#define  HW_CFG_LED0_EN      (1u << 20)
#define  HW_CFG_MEF          (1u << 4)    /* several frames per USB transfer */
#define  HW_CFG_LRST         (1u << 1)    /* lite reset */
#define PMT_CTL              0x014u
#define  PMT_CTL_READY       (1u << 7)
#define  PMT_CTL_PHY_RST     (1u << 4)
#define USB_CFG0             0x080u
#define  USB_CFG_BIR         (1u << 6)    /* empty bulk IN: zero-length packet, not NAK */
#define  USB_CFG_BCE         (1u << 5)    /* burst cap enable */
#define BURST_CAP            0x090u
#define BULK_IN_DLY          0x094u
#define RFE_CTL              0x0B0u
#define  RFE_CTL_BCAST_EN    (1u << 10)
#define  RFE_CTL_MCAST_EN    (1u << 9)    /* all multicast: mDNS and NBNS need it */
#define  RFE_CTL_DA_PERFECT  (1u << 1)
#define FCT_RX_CTL           0x0C0u
#define  FCT_RX_CTL_EN       (1u << 31)
#define FCT_TX_CTL           0x0C4u
#define  FCT_TX_CTL_EN       (1u << 31)
#define FCT_RX_FIFO_END      0x0C8u
#define FCT_TX_FIFO_END      0x0CCu
#define FCT_FLOW             0x0D0u
#define MAC_CR               0x100u
#define  MAC_CR_AUTO_DUPLEX  (1u << 12)
#define  MAC_CR_AUTO_SPEED   (1u << 11)
#define MAC_RX               0x104u
#define  MAC_RX_MAX_SIZE_MASK 0x3FFF0000u
#define  MAC_RX_MAX_SIZE_SHIFT 16
#define  MAC_RX_RXEN         (1u << 0)
#define MAC_TX               0x108u
#define  MAC_TX_TXEN         (1u << 0)
#define FLOW                 0x10Cu
#define RX_ADDRH             0x118u
#define RX_ADDRL             0x11Cu
#define MII_ACC              0x120u
#define  MII_ACC_PHY_SHIFT   11
#define  MII_ACC_REG_SHIFT   6
#define  MII_ACC_WRITE       (1u << 1)
#define  MII_ACC_BUSY        (1u << 0)
#define MII_DATA             0x124u
#define MAF_HI(n)            (0x400u + 8u * (n))
#define MAF_LO(n)            (0x404u + 8u * (n))
#define  MAF_HI_VALID        (1u << 31)

#define FIFO_SIZE            (12u * 1024u)     /* each of the RX and TX FIFOs */
#define DEFAULT_BULK_IN_DLY  0x0800u

#define PHY_ADDR             1u                /* the internal PHY */
#define MII_BMCR             0u
#define  BMCR_ANENABLE       (1u << 12)
#define  BMCR_ANRESTART      (1u << 9)
#define MII_BMSR             1u
#define  BMSR_LSTATUS        (1u << 2)

#define MII_CMD(reg, write)  ((PHY_ADDR << MII_ACC_PHY_SHIFT) | ((uint32_t)(reg) << MII_ACC_REG_SHIFT) \
                              | ((write) ? MII_ACC_WRITE : 0u) | MII_ACC_BUSY)

/* Frame headers */
#define RX_HDR_LEN           10u               /* RX_CMD_A, RX_CMD_B (32-bit), RX_CMD_C (16-bit) */
#define  RX_CMD_A_RED        (1u << 22)        /* receive error */
#define  RX_CMD_A_LEN_MASK   0x3FFFu           /* frame length, FCS included */
#define RX_PAD               2u                /* each header + frame is padded so the next starts 4-aligned */
#define TX_HDR_LEN           8u                /* TX_CMD_A, TX_CMD_B */
#define  TX_CMD_A_FCS        (1u << 22)        /* the chip appends the FCS */

#define ETH_FRAME_MAX        1518u             /* with VLAN tag room below */
#define MAC_RX_MAX_FRAME     1522u             /* 1500 + 18 (VLAN header) + 4 (FCS) */

/* ---- our side ----------------------------------------------------------- */

#define ETHER_RX_BUF         (8u * 1024u)      /* one bulk IN transfer */
#define ETHER_BURST_CAP      (ETHER_RX_BUF / 512u)   /* in high-speed packets */
#define ETHER_RX_IDLE_US     1000u             /* after an empty answer */
#define ETHER_TX_SLOTS       16u               /* a TCP burst: lwIP sends a window at once */
#define ETHER_TX_SLOT        (TX_HDR_LEN + ETH_FRAME_MAX + 2u)
#define ETHER_LINK_POLL_US   1000000u
#define ETHER_WAIT_STEP_US   1000u             /* between reads of a register being waited on */

typedef enum { OP_WRITE, OP_MODIFY, OP_WAIT, OP_EXPECT, OP_READ_LINK, OP_END } op_kind_t;

typedef struct {
   op_kind_t kind;
   uint16_t  reg;
   uint32_t  a;          /* WRITE: value; MODIFY: bits cleared; WAIT/EXPECT: mask */
   uint32_t  b;          /* MODIFY: bits set; WAIT/EXPECT: wanted value */
   uint16_t  timeout_ms; /* WAIT */
} reg_op_t;

/* Words filled in from the MAC address before the script runs. */
#define MAC_LO_WORD 0xFFFFFFF0u
#define MAC_HI_WORD 0xFFFFFFF1u
#define MAF_HI_WORD 0xFFFFFFF2u

static const reg_op_t init_script[] = {
   { OP_EXPECT, ID_REV,   ID_REV_CHIP_ID_MASK, (uint32_t)LAN7800_PID << 16, 0 },
   { OP_MODIFY, HW_CFG,   0, HW_CFG_LRST, 0 },
   { OP_WAIT,   HW_CFG,   HW_CFG_LRST, 0, 1000 },
   { OP_WRITE,  RX_ADDRL, MAC_LO_WORD, 0, 0 },
   { OP_WRITE,  RX_ADDRH, MAC_HI_WORD, 0, 0 },
   { OP_WRITE,  MAF_LO(0), MAC_LO_WORD, 0, 0 },
   { OP_WRITE,  MAF_HI(0), MAF_HI_WORD, 0, 0 },
   { OP_MODIFY, USB_CFG0, 0, USB_CFG_BIR, 0 },
   { OP_WRITE,  BURST_CAP, ETHER_BURST_CAP, 0, 0 },
   { OP_WRITE,  BULK_IN_DLY, DEFAULT_BULK_IN_DLY, 0, 0 },
   { OP_MODIFY, HW_CFG,   0, HW_CFG_MEF | HW_CFG_LED0_EN | HW_CFG_LED1_EN, 0 },
   { OP_MODIFY, USB_CFG0, 0, USB_CFG_BCE, 0 },
   { OP_WRITE,  FCT_RX_FIFO_END, (FIFO_SIZE - 512u) / 512u, 0, 0 },
   { OP_WRITE,  FCT_TX_FIFO_END, (FIFO_SIZE - 512u) / 512u, 0, 0 },
   { OP_WRITE,  INT_STS,  0xFFFFFFFFu, 0, 0 },
   { OP_WRITE,  FLOW,     0, 0, 0 },
   { OP_WRITE,  FCT_FLOW, 0, 0, 0 },
   { OP_MODIFY, RFE_CTL,  0, RFE_CTL_BCAST_EN | RFE_CTL_MCAST_EN | RFE_CTL_DA_PERFECT, 0 },
   { OP_MODIFY, PMT_CTL,  0, PMT_CTL_PHY_RST, 0 },
   { OP_WAIT,   PMT_CTL,  PMT_CTL_PHY_RST | PMT_CTL_READY, PMT_CTL_READY, 1000 },
   { OP_MODIFY, MAC_CR,   0, MAC_CR_AUTO_DUPLEX | MAC_CR_AUTO_SPEED, 0 },
   { OP_MODIFY, MAC_TX,   0, MAC_TX_TXEN, 0 },
   { OP_MODIFY, FCT_TX_CTL, 0, FCT_TX_CTL_EN, 0 },
   { OP_MODIFY, MAC_RX,   MAC_RX_MAX_SIZE_MASK, (MAC_RX_MAX_FRAME << MAC_RX_MAX_SIZE_SHIFT) | MAC_RX_RXEN, 0 },
   { OP_MODIFY, FCT_RX_CTL, 0, FCT_RX_CTL_EN, 0 },
   /* Autonegotiate: the PHY advertises everything it can after its reset. */
   { OP_WAIT,   MII_ACC,  MII_ACC_BUSY, 0, 100 },
   { OP_WRITE,  MII_DATA, BMCR_ANENABLE | BMCR_ANRESTART, 0, 0 },
   { OP_WRITE,  MII_ACC,  MII_CMD(MII_BMCR, true), 0, 0 },
   { OP_WAIT,   MII_ACC,  MII_ACC_BUSY, 0, 100 },
   { OP_END,    0, 0, 0, 0 }
};

/* One PHY status read: BMSR into the link flag. */
static const reg_op_t link_script[] = {
   { OP_WAIT,      MII_ACC, MII_ACC_BUSY, 0, 100 },
   { OP_WRITE,     MII_ACC, MII_CMD(MII_BMSR, false), 0, 0 },
   { OP_WAIT,      MII_ACC, MII_ACC_BUSY, 0, 100 },
   { OP_READ_LINK, MII_DATA, 0, 0, 0 },
   { OP_END,       0, 0, 0, 0 }
};

typedef enum { ETH_ABSENT, ETH_WAIT_MAC, ETH_INIT, ETH_READY, ETH_FAILED } eth_state_t;

static struct {
   eth_state_t state;
   uint8_t  daddr, itf, ep_in, ep_out;
   uint8_t  mac[6];
   uint32_t mac_lo, mac_hi;
   const char *error;

   /* the register-script engine */
   const reg_op_t *script;
   uint8_t  op, sub;
   bool     ctrl_busy, ctrl_ok;
   uint32_t value;              /* last register read */
   uint32_t op_started_us, next_try_us;

   bool     link_up;
   uint32_t link_poll_us;

   /* receive */
   bool     rx_armed;
   uint32_t rx_len, rx_pos, rx_next_us;

   /* transmit */
   bool     tx_busy;
   uint8_t  tx_head, tx_tail, tx_count;
   uint16_t tx_len[ETHER_TX_SLOTS];

   usb_ether_counts_t counts;
} E;

static uint32_t ctrl_word;                        /* control transfer data stage */
CFG_TUSB_MEM_ALIGN static uint8_t rx_buf[ETHER_RX_BUF];
CFG_TUSB_MEM_ALIGN static uint8_t tx_buf[ETHER_TX_SLOTS][(ETHER_TX_SLOT + 3u) & ~3u];

static inline uint32_t now_us(void) { return RPI_GetSystemTime(); }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void fail(const char *why)
{
   E.error = why;
   E.state = ETH_FAILED;
   E.script = NULL;
   E.link_up = false;
}

/* ---- register access: non-blocking control transfers ------------------- */

static void ctrl_done(tuh_xfer_t *xfer)
{
   E.ctrl_ok = xfer->result == XFER_RESULT_SUCCESS && xfer->actual_len == 4u;
   if (E.ctrl_ok)
      E.value = ctrl_word;      /* a read's answer; a write's echo of what went */
   else
      E.counts.usb_errors++;
   E.ctrl_busy = false;
}

static bool reg_xfer(bool read, uint16_t reg, uint32_t value)
{
   static tusb_control_request_t setup;
   tuh_xfer_t xfer;
   setup = (tusb_control_request_t){
      .bmRequestType_bit = { .recipient = TUSB_REQ_RCPT_DEVICE, .type = TUSB_REQ_TYPE_VENDOR,
                             .direction = read ? TUSB_DIR_IN : TUSB_DIR_OUT },
      .bRequest = read ? REQ_READ_REGISTER : REQ_WRITE_REGISTER,
      .wValue = 0, .wIndex = reg, .wLength = 4
   };
   ctrl_word = value;           /* little-endian on the wire, as the ARM is */
   memset(&xfer, 0, sizeof xfer);
   xfer.daddr = E.daddr;
   xfer.ep_addr = 0;
   xfer.setup = &setup;
   xfer.buffer = (uint8_t *)&ctrl_word;
   xfer.complete_cb = ctrl_done;
   E.ctrl_busy = true;
   if (!tuh_control_xfer(&xfer)) {
      E.ctrl_busy = false;
      E.counts.usb_errors++;
      return false;
   }
   return true;
}

static uint32_t op_value(uint32_t v)
{
   switch (v) {
   case MAC_LO_WORD: return E.mac_lo;
   case MAC_HI_WORD: return E.mac_hi;
   case MAF_HI_WORD: return E.mac_hi | MAF_HI_VALID;
   default:          return v;
   }
}

/* A script that cannot finish.  Setting the chip up fails outright; a link
   read on a running chip is just dropped, and the next one tried a second
   later - one bad transfer must not take a working link down for good. */
static bool script_fail(const char *why)
{
   if (E.state == ETH_READY) {
      E.script = NULL;
      return false;
   }
   fail(why);
   return false;
}

static void script_start(const reg_op_t *script)
{
   E.script = script;
   E.op = 0;
   E.sub = 0;
}

/* One step of the running script: issue the next transfer, or look at the
   answer to the last.  True while the script is still running. */
static bool script_step(void)
{
   const reg_op_t *op = &E.script[E.op];
   uint32_t t = now_us();

   if (E.ctrl_busy)
      return true;
   if (E.sub != 0u && !E.ctrl_ok)
      return script_fail("USB control transfer failed");
   if (E.sub == 0u)
      E.op_started_us = t;

   switch (op->kind) {
   case OP_END:
      E.script = NULL;
      return false;

   case OP_WRITE:
      if (E.sub == 0u) { E.sub = 1; (void)reg_xfer(false, op->reg, op_value(op->a)); return true; }
      break;

   case OP_MODIFY:
      if (E.sub == 0u) { E.sub = 1; (void)reg_xfer(true, op->reg, 0); return true; }
      if (E.sub == 1u) { E.sub = 2; (void)reg_xfer(false, op->reg, (E.value & ~op->a) | op->b); return true; }
      break;

   case OP_EXPECT:
      if (E.sub == 0u) { E.sub = 1; (void)reg_xfer(true, op->reg, 0); return true; }
      if ((E.value & op->a) != op->b)
         return script_fail("not a LAN7800");
      break;

   case OP_READ_LINK:
      if (E.sub == 0u) { E.sub = 1; (void)reg_xfer(true, op->reg, 0); return true; }
      E.link_up = (E.value & BMSR_LSTATUS) != 0u;
      break;

   case OP_WAIT:
      if (E.sub == 0u) {
         E.next_try_us = t;
         E.sub = 1;
      }
      if (E.sub == 1u) {
         if ((int32_t)(t - E.next_try_us) < 0)
            return true;
         E.sub = 2;
         (void)reg_xfer(true, op->reg, 0);
         return true;
      }
      if ((E.value & op->a) != op->b) {
         /* op_started_us was taken at sub 0 and is left alone while retrying */
         if ((uint32_t)(t - E.op_started_us) >= (uint32_t)op->timeout_ms * 1000u)
            return script_fail("chip did not finish a reset");
         E.sub = 1;
         E.next_try_us = t + ETHER_WAIT_STEP_US;
         return true;
      }
      break;
   }
   E.op++;
   E.sub = 0;
   return true;
}

/* ---- receive ------------------------------------------------------------ */

static void rx_arm(void)
{
   if (!usbh_edpt_claim(E.daddr, E.ep_in))
      return;
   if (!usbh_edpt_xfer(E.daddr, E.ep_in, rx_buf, (uint16_t)sizeof rx_buf)) {
      (void)usbh_edpt_release(E.daddr, E.ep_in);
      E.counts.usb_errors++;
      E.rx_next_us = now_us() + ETHER_RX_IDLE_US;
      return;
   }
   E.rx_armed = true;
}

bool usb_ether_next_frame(const uint8_t **frame, uint16_t *len)
{
   while (E.state == ETH_READY && E.rx_pos + RX_HDR_LEN <= E.rx_len) {
      const uint8_t *h = &rx_buf[E.rx_pos];
      uint32_t cmd_a = le32(h);
      uint32_t size = cmd_a & RX_CMD_A_LEN_MASK;
      uint32_t data = E.rx_pos + RX_HDR_LEN;

      if (data + size > E.rx_len) {          /* a header that runs off the end */
         E.counts.rx_errors++;
         E.rx_pos = E.rx_len;
         break;
      }
      E.rx_pos = data + size + (4u - ((size + RX_PAD) % 4u)) % 4u;
      if ((cmd_a & RX_CMD_A_RED) != 0u || size <= 4u || size - 4u > ETH_FRAME_MAX + 4u) {
         E.counts.rx_errors++;
         continue;
      }
      *frame = &rx_buf[data];
      *len = (uint16_t)(size - 4u);          /* the chip leaves the FCS on */
      E.counts.rx_frames++;
      return true;
   }
   return false;
}

/* ---- transmit ----------------------------------------------------------- */

static void tx_kick(void)
{
   if (E.tx_busy || E.tx_count == 0u || E.state != ETH_READY)
      return;
   if (!usbh_edpt_claim(E.daddr, E.ep_out))
      return;
   if (!usbh_edpt_xfer(E.daddr, E.ep_out, tx_buf[E.tx_tail], E.tx_len[E.tx_tail])) {
      (void)usbh_edpt_release(E.daddr, E.ep_out);
      E.counts.usb_errors++;
      return;
   }
   E.tx_busy = true;
}

bool usb_ether_send(const uint8_t *frame, uint16_t len)
{
   if (E.state != ETH_READY || len == 0u || len > ETH_FRAME_MAX
       || E.tx_count == ETHER_TX_SLOTS) {
      E.counts.tx_dropped++;
      return false;
   }
   uint8_t *slot = tx_buf[E.tx_head];
   put_le32(slot, (uint32_t)len | TX_CMD_A_FCS);
   put_le32(slot + 4, 0u);
   memcpy(slot + TX_HDR_LEN, frame, len);
   E.tx_len[E.tx_head] = (uint16_t)(TX_HDR_LEN + len);
   E.tx_head = (uint8_t)((E.tx_head + 1u) % ETHER_TX_SLOTS);
   E.tx_count++;
   tx_kick();
   return true;
}

/* ---- the main-loop step ------------------------------------------------- */

void usb_ether_poll(void)
{
   uint32_t t;

   switch (E.state) {
   case ETH_ABSENT:
   case ETH_FAILED:
      return;

   case ETH_WAIT_MAC:
      /* The board's MAC comes from the mailbox: ask only when nothing else
         is waiting on it, as the WiFi start does. */
      if (!RPI_PropertyReplyWaiting())
         return;
      if (!rpi_get_board_mac(E.mac)) {
         fail("no board MAC address from the firmware");
         return;
      }
      E.mac_lo = (uint32_t)E.mac[0] | (uint32_t)E.mac[1] << 8 | (uint32_t)E.mac[2] << 16 | (uint32_t)E.mac[3] << 24;
      E.mac_hi = (uint32_t)E.mac[4] | (uint32_t)E.mac[5] << 8;
      script_start(init_script);
      E.state = ETH_INIT;
      return;

   case ETH_INIT:
      if (!script_step() && E.state == ETH_INIT) {
         E.state = ETH_READY;
         E.link_poll_us = now_us();          /* read the link at once */
         E.rx_next_us = now_us();
      }
      return;

   case ETH_READY:
      t = now_us();
      if (E.script != NULL)
         (void)script_step();
      else if ((int32_t)(t - E.link_poll_us) >= 0) {
         E.link_poll_us = t + ETHER_LINK_POLL_US;
         script_start(link_script);
      }
      if (!E.rx_armed && E.rx_pos >= E.rx_len && (int32_t)(t - E.rx_next_us) >= 0)
         rx_arm();
      tx_kick();
      return;
   }
}

/* ---- TinyUSB class driver ----------------------------------------------- */

static bool ether_init(void) { memset(&E, 0, sizeof E); return true; }
static bool ether_deinit(void) { return true; }

static uint16_t ether_open(uint8_t rhport, uint8_t daddr, const tusb_desc_interface_t *itf, uint16_t max_len)
{
   uint16_t vid, pid;
   (void)rhport;

   if (E.state != ETH_ABSENT || itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC
       || !tuh_vid_pid_get(daddr, &vid, &pid) || vid != LAN78XX_VID || pid != LAN7800_PID)
      return 0;

   /* Walk this interface's descriptors: open the two bulk endpoints (the
      interrupt endpoint is not used - the link is read from the PHY). */
   const uint8_t *p = (const uint8_t *)itf;
   const uint8_t *end = p + max_len;
   uint8_t in = 0, out = 0, seen = 0;
   p = tu_desc_next(p);
   while (p < end && seen < itf->bNumEndpoints) {
      if (p[0] == 0u || p + p[0] > end)
         return 0;
      if (tu_desc_type(p) == TUSB_DESC_INTERFACE)
         break;
      if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
         const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)p;
         seen++;
         if (ep->bmAttributes.xfer == TUSB_XFER_BULK) {
            if (!tuh_edpt_open(daddr, ep))
               return 0;
            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN)
               in = ep->bEndpointAddress;
            else
               out = ep->bEndpointAddress;
         }
      }
      p = tu_desc_next(p);
   }
   if (in == 0u || out == 0u)
      return 0;

   E.daddr = daddr;
   E.itf = itf->bInterfaceNumber;
   E.ep_in = in;
   E.ep_out = out;
   return (uint16_t)(p - (const uint8_t *)itf);
}

static bool ether_set_config(uint8_t daddr, uint8_t itf_num)
{
   /* Setting the chip up takes many transfers and some waits: it is stepped
      from usb_ether_poll, not done here inside the host stack's enumeration. */
   E.error = "";
   E.state = ETH_WAIT_MAC;
   usbh_driver_set_config_complete(daddr, itf_num);
   return true;
}

static bool ether_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred)
{
   if (daddr != E.daddr)
      return false;
   if (ep_addr == E.ep_in) {
      E.rx_armed = false;
      E.rx_pos = 0;
      if (result != XFER_RESULT_SUCCESS) {
         E.counts.usb_errors++;
         E.rx_len = 0;
         E.rx_next_us = now_us() + ETHER_RX_IDLE_US;
      } else {
         E.rx_len = xferred;
         /* Data: ask again as soon as it has been read out.  Empty (the chip's
            zero-length answer): wait a little before asking again. */
         E.rx_next_us = now_us() + (xferred == 0u ? ETHER_RX_IDLE_US : 0u);
      }
      return true;
   }
   if (ep_addr == E.ep_out) {
      E.tx_busy = false;
      if (result == XFER_RESULT_SUCCESS)
         E.counts.tx_frames++;
      else
         E.counts.usb_errors++;
      E.tx_tail = (uint8_t)((E.tx_tail + 1u) % ETHER_TX_SLOTS);
      E.tx_count--;
      tx_kick();
      return true;
   }
   return false;
}

static void ether_close(uint8_t daddr)
{
   if (daddr != E.daddr)
      return;
   memset(&E, 0, sizeof E);
}

static const usbh_class_driver_t ether_driver = {
   .name       = "LAN7800",
   .init       = ether_init,
   .deinit     = ether_deinit,
   .open       = ether_open,
   .set_config = ether_set_config,
   .xfer_cb    = ether_xfer_cb,
   .close      = ether_close,
};

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
   *driver_count = 1;
   return &ether_driver;
}

/* ---- queries ------------------------------------------------------------ */

bool usb_ether_present(void) { return E.state != ETH_ABSENT; }
bool usb_ether_ready(void)   { return E.state == ETH_READY; }
bool usb_ether_link_up(void) { return E.state == ETH_READY && E.link_up; }
const char *usb_ether_error(void) { return E.error != NULL ? E.error : ""; }

bool usb_ether_mac(uint8_t mac[6])
{
   if (E.state != ETH_INIT && E.state != ETH_READY)
      return false;
   memcpy(mac, E.mac, 6);
   return true;
}

void usb_ether_counts(usb_ether_counts_t *out)
{
   *out = E.counts;
}
