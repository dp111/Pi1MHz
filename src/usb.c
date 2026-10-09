/*
  usb
*/
#include "usb/tusb_config.h"
// Short names so these resolve via the -isystem TinyUSB dirs (third-party,
// warnings suppressed). Do NOT change back to explicit relative paths.
#include "usb.h"
#include "usb/mtp_fs.h"
#include "chainboot.h"
#include "usb_storage.h"
#include "BeebSCSI/filesystem.h"
#include <bsp/board_api.h>
#include "rpi/interrupts.h"
#include "Pi1MHz.h"
#include "rpi/mailbox.h"
#include <strings.h>
#include "rpi/base.h"
#include "rpi/info.h"
#include "config.h"

// Power device IDs for mailbox
#define POWER_DEVICE_USB_HCD    3   // USB Host Controller Device


size_t board_get_unique_id(uint8_t id[], size_t max_len)
{
  if ((id == NULL) || (max_len == 0)) {
    return 0;
  }

  const rpi_mailbox_property_t* serial = RPI_PropertyGetWord(TAG_GET_BOARD_SERIAL, 0);
  if (serial == NULL) {
    return 0;
  }

  const size_t uid_len = 8;
  size_t copy_len = (max_len < uid_len) ? max_len : uid_len;

  memcpy(id, serial->data.buffer_32, copy_len);
  return copy_len;

}

/* A combination of interfaces must have a unique product id, since PC will save device driver after the first plug.
 * Same VID/PID with different interface e.g MSC (first), then CDC (later) will possibly cause system error on PC.
 *
 * Auto ProductID layout's Bitmap:
 *   [MSB]  MTP | VENDOR | MIDI | HID | MSC | CDC [LSB]
 */
#define PID_MAP(itf, n)  ((CFG_TUD_##itf) ? (1 << (n)) : 0)
#define USB_PID           (0x4000 | PID_MAP(CDC, 0) | PID_MAP(MSC, 1) | PID_MAP(HID, 2) | \
                           PID_MAP(MIDI, 3) | PID_MAP(VENDOR, 4) | PID_MAP(MTP, 5))

#define USB_VID   0xCafe
#define USB_BCD   0x0200

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum
{
  ITF_NUM_MTP = 0,
  ITF_NUM_TOTAL
};

#if defined(TUD_ENDPOINT_ONE_DIRECTION_ONLY)
  // MCUs that don't support a same endpoint number with different direction IN and OUT defined in tusb_mcu.h
  //    e.g EP1 OUT & EP1 IN cannot exist together
  #define EPNUM_MTP_EVT     0x81
  #define EPNUM_MTP_OUT     0x03
  #define EPNUM_MTP_IN      0x82
#else
  #define EPNUM_MTP_EVT     0x81
  #define EPNUM_MTP_OUT     0x02
  #define EPNUM_MTP_IN      0x82
#endif

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MTP_DESC_LEN)

// full speed configuration
const uint8_t desc_fs_configuration[] = {
  // Config number, interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
  // Interface number, string index, EP event, EP event size, EP event polling, EP Out & EP In address, EP size
  TUD_MTP_DESCRIPTOR(ITF_NUM_MTP, 4, EPNUM_MTP_EVT, 64, 1, EPNUM_MTP_OUT, EPNUM_MTP_IN, 64),
};

#if TUD_OPT_HIGH_SPEED
// Per USB specs: high speed capable device must report device_qualifier and other_speed_configuration

// high speed configuration
static uint8_t const desc_hs_configuration[] = {
  // Config number, interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
  // Interface number, string index, EP event, EP event size, EP event polling, EP Out & EP In address, EP size
  TUD_MTP_DESCRIPTOR(ITF_NUM_MTP, 4, EPNUM_MTP_EVT, 64, 1, EPNUM_MTP_OUT, EPNUM_MTP_IN, 512),
};

// other speed configuration
static uint8_t desc_other_speed_config[CONFIG_TOTAL_LEN];

// device qualifier is mostly similar to device descriptor since we don't change configuration based on speed
static tusb_desc_device_qualifier_t const desc_device_qualifier = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = USB_BCD,

    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0x00
};

// Invoked when received GET DEVICE QUALIFIER DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete.
// device_qualifier descriptor describes information about a high-speed capable device that would
// change if the device were operating at the other speed. If not highspeed capable stall this request.
uint8_t const *tud_descriptor_device_qualifier_cb(void) {
  return (uint8_t const *) &desc_device_qualifier;
}

// Invoked when received GET OTHER SEED CONFIGURATION DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
// Configuration descriptor in the other speed e.g if high speed then this is for full speed and vice versa
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index) {
  (void) index; // for multiple configurations

  // if link speed is high return fullspeed config, and vice versa
  // Note: the descriptor type is OTHER_SPEED_CONFIG instead of CONFIG
  memcpy(desc_other_speed_config,
         (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_fs_configuration : desc_hs_configuration,
         CONFIG_TOTAL_LEN);
  desc_other_speed_config[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
  return desc_other_speed_config;
}

#endif // highspeed

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
const uint8_t*tud_descriptor_configuration_cb(uint8_t index) {
  (void) index; // for multiple configurations
#if TUD_OPT_HIGH_SPEED
  // Although we are highspeed, host may be fullspeed.
  return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_hs_configuration : desc_fs_configuration;
#else
  return desc_fs_configuration;
#endif
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// String Descriptor Index
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_MTP,
};

// array of pointer to string descriptors
static char const *string_desc_arr[] =
{
  (const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
  "TinyUsb",                     // 1: Manufacturer
  "TinyUsb Device",              // 2: Product
  NULL,                          // 3: Serials will use unique ID if possible
  "TinyUSB MTP",                 // 4: MTP Interface
};

static uint16_t _desc_str[32 + 1];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  size_t chr_count;

  switch ( index ) {
    case STRID_LANGID:
      memcpy(&_desc_str[1], string_desc_arr[0], 2);
      chr_count = 1;
      break;

    case STRID_SERIAL:
      chr_count = board_usb_get_serial(_desc_str + 1, 32);
      break;

    default:
      // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
      // https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors

      if ( !(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) ) {
        return NULL;
      }

      const char *str = string_desc_arr[index];
      // Cap at max char
      chr_count = strlen(str);
      const size_t max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1; // -1 for string type
      if ( chr_count > max_count ) {
        chr_count = max_count;
      }

      // Convert ASCII string into UTF-16
      for ( size_t i = 0; i < chr_count; i++ ) {
        _desc_str[1 + i] = str[i];
      }
      break;
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

  return _desc_str;
}



/* The port's role, chosen once at boot.  On a board whose port is behind
   its own hub (Pi 1/2/3 Model B) it can only be a host, whatever the config
   says.  Elsewhere usb_mode= in Pi1MHz.cfg picks:
     host    a USB mouse for the Beeb (usb_mouse.c) and a flash drive
             (usb_storage.c), directly or via a hub
             (the default)
     device  MTP to a computer
     auto    host when the OTG ID pin is grounded - an OTG adapter - and
             device when it is not. */
static bool s_usb_host;
static bool s_usb_initialised;     /* usb_init has posted the power-up */
static bool s_usb_started;         /* usb_boot_task has started the stack */

bool usb_is_host(void)
{
  return s_usb_host;
}

/* The OTG connector-ID status, GOTGCTL bit 16: 0 when the ID pin is grounded
   (an OTG adapter: this end is the host). */
#define USB_GOTGCTL   (*(volatile uint32_t *)(PERIPHERAL_BASE + 0x980000u))
#define GOTGCTL_CIDSTS (1u << 16)

static bool usb_choose_host(void)
{
  if (board_usb_behind_hub())
    return true;
  const char *mode = config_get("usb_mode");
  if (mode != NULL && strcasecmp(mode, "device") == 0)
    return false;
  if (mode != NULL && strcasecmp(mode, "auto") == 0)
    return (USB_GOTGCTL & GOTGCTL_CIDSTS) == 0u;
  return true;
}

/* Host mode is polled, never interrupt driven: with the controller's
   interrupt output enabled, the first interrupt after the port is powered
   resets the whole board (MEASURED on a Zero 2 W, chain-booted kernels;
   never tried from a cold boot - presumably the VideoCore, which sees the
   same IRQ, as nothing on the ARM side runs or is reported).
   So usb_boot_task turns that output off, and the controller's interrupt
   work is done from here, every USB_HOST_POLL_US - a mouse needs nothing
   faster, and in slave mode the receive FIFO is still emptied promptly. */
#define USB_GAHBCFG      (*(volatile uint32_t *)(PERIPHERAL_BASE + 0x980008u))
#define GAHBCFG_GINT_BIT (1u << 0)

#define USB_HOST_POLL_US 250u
static void usb_host_work(void) {
    tuh_int_handler(BOARD_TUH_RHPORT, false);
    tuh_task();
    usb_storage_poll();    /* FatFs work for a drive that came or went */
}

static void usb_host_task(void) {
    static uint32_t last_us;
    if ((uint32_t)(Pi1MHz_now_us - last_us) < USB_HOST_POLL_US)
        return;
    last_us = Pi1MHz_now_us;
    usb_host_work();
    chainboot_poll();      /* a kernel.now PUT's restart (no MTP in host mode) */
}

static void usb_task(void) {
    tud_task();
    /* A received kernel.now is flashed from here rather than from inside the
       MTP callback - see chainboot.c. */
    chainboot_poll();
    mtp_fs_cache_poll();
}


/* Second half of usb_init, run from the poll loop.
 *
 * Powering the USB host controller takes the VideoCore ~527 ms, and waiting
 * for it in init held up the whole poll loop - and so the first SCSI service
 * to the Beeb - by that long, on a machine whose ADFS writes have no
 * timeout.  The request is posted in usb_init and collected here instead, so
 * the loop is already running and serving while the domain comes up.
 *
 * tusb_init must not touch the controller before the domain is up: doing so
 * leaves USB dead with no error, which is what the reply gates.  The wait is
 * split rather than removed.
 */
static void usb_boot_task(void)
{
  if (!RPI_PropertyReplyWaiting())
     return;                       /* still powering - try again next pass */

  RPI_PropertySettle();            /* the answer is here; this will not block */

  s_usb_host = usb_choose_host();
  tusb_rhport_init_t port_init = {
    .role = s_usb_host ? TUSB_ROLE_HOST : TUSB_ROLE_DEVICE,
    .speed = TUSB_SPEED_AUTO
  };
  tusb_init(BOARD_TUD_RHPORT, &port_init);

  if (s_usb_host) {
    /* Polled - see usb_host_task.  hcd_init never enabled the controller's
       output (usb/tinyusb-hcd-polled.patch), and TinyUSB's hcd_int_enable
       on every tuh_task leaves the ARM's IRQ 9 alone (dwc2_int_set in
       usb/broadcom/interrupts.h), so this stays as set here: */
    USB_GAHBCFG &= ~GAHBCFG_GINT_BIT;
    RPI_GetIrqController()->Disable_IRQs_1 = (1 << 9);
  } else {
    // Enable USB IRQ (IRQ #9 in Enable_IRQs_1)
    // The IRQ handler is already attached in IRQHandler_main() - see Pi1MHz.c
    RPI_GetIrqController()->Enable_IRQs_1 = (1 << 9);
  }

  /* Swap in the steady-state callback, in place: no later pass then tests
     whether start-up has finished. */
  Pi1MHz_Replace_Poll(usb_boot_task, s_usb_host ? usb_host_task : usb_task, "usb");
  s_usb_started = true;
}

/* One step of USB work from inside a wait, outside the poll loop: the power-
   on storage decision waiting for a flash drive (usb_storage_wait_for_drive).
   The port is brought up first if the boot half has not run yet.  False when
   there is nothing to wait for: USB never started, or the port is a device.
   No chainboot_poll here: a restart must not begin inside another's wait. */
bool usb_service(void)
{
  if (!s_usb_initialised)
    return false;
  if (!s_usb_started) {
    usb_boot_task();
    return true;
  }
  if (!s_usb_host)
    return false;
  usb_host_work();
  return true;
}

void usb_init(uint8_t instance , uint8_t address) {
  /* The host/device choice in usb_boot_task needs the board revision, and
     there, a poll callback, only the short mailbox bound is allowed: ask it
     here with the full bound, while boot can afford the wait.  First, while
     the property buffer is free. */
  board_revision_prime();
  /* Posted, not waited for - see usb_boot_task. */
  RPI_PropertySetWord(TAG_SET_POWER_STATE, POWER_DEVICE_USB_HCD, 0x00000003);

  Pi1MHz_Register_Poll(usb_boot_task, "usb-boot");
  s_usb_initialised = true;
  filesystemRegisterEject(mtp_fs_eject, mtp_fs_inserted);
}
