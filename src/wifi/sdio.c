#include "sdio.h"

#include "cyw43.h"

#include "../rpi/rpi.h"
#include "../rpi/systimer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDIO_RUNTIME_MAX_FRAME_SIZE 1600u
#define SDIO_RUNTIME_MAX_RX_FRAMES_PER_POLL 8u
#define SDIO_RUNTIME_FW_CHUNKS_PER_TICK 8u
#define SDIO_RUNTIME_HIGH_CLOCK_HZ 25000000u
#define SDIO_COMMAND_TIMEOUT_US 500000u
#define SDIO_RUNTIME_POLL_TIMEOUT_US 5000u
#define SDIO_BACKPLANE_WINDOW_SIZE 0x8000u
#define SDIO_BACKPLANE_TRANSFER_MAX 512u
#define SDIO_CORE_SCAN_SIZE 512u

static sdio_probe_result_t g_sdio_probe_result;
static uint16_t g_tx_control_probe_request_id = 1u;
static uint8_t g_tx_control_probe_sequence = 0u;
static sdio_host_t g_runtime_device;
static bool g_runtime_started;
static bool g_runtime_link_up;
static uint32_t g_runtime_tx_frame_count;
static uint32_t g_runtime_rx_frame_count;
static char g_runtime_error[96];
static uint8_t g_runtime_data_sequence;
static bool g_runtime_emulator_mode;
static bool g_runtime_identify_started;
static unsigned int g_runtime_identify_attempt;
static uint32_t g_runtime_identify_deadline_us;

typedef struct {
   bool active;
   unsigned int attempt;
   uint32_t deadline_us;
} sdio_runtime_wait_state_t;

static sdio_runtime_wait_state_t g_runtime_alp_wait;
static sdio_runtime_wait_state_t g_runtime_kso_wait;

/* Bring-up state machine. The first version of the runtime did all of
   this sequentially in one blocking call which stalled the main loop
   for the entire duration of CYW43 firmware load. We now advance one
   stage per sdio_runtime_tick() so the rest of the system keeps
   running. Each stage corresponds to a chunk of work that was
   previously inline in sdio_runtime_start(). */
typedef enum {
   SDIO_RUNTIME_STAGE_IDLE = 0,
   SDIO_RUNTIME_STAGE_OPEN_HOST,
   SDIO_RUNTIME_STAGE_IDENTIFY_CARD,
   SDIO_RUNTIME_STAGE_READ_CCCR,
   SDIO_RUNTIME_STAGE_ENABLE_FUNCTIONS,
   SDIO_RUNTIME_STAGE_REQUEST_ALP,
   SDIO_RUNTIME_STAGE_READ_POWER,
   SDIO_RUNTIME_STAGE_WAKE_KSO,
   SDIO_RUNTIME_STAGE_BOOT_FIRMWARE,
   SDIO_RUNTIME_STAGE_READ_MAILBOX,
   SDIO_RUNTIME_STAGE_ACK_INTERRUPTS,
   SDIO_RUNTIME_STAGE_WRITE_INTR_MASK,
   SDIO_RUNTIME_STAGE_PREPARE_JOIN,
   SDIO_RUNTIME_STAGE_SWEEP_RX,
   SDIO_RUNTIME_STAGE_DONE,
   SDIO_RUNTIME_STAGE_ERROR
} sdio_runtime_stage_t;

static sdio_runtime_stage_t g_runtime_stage;
static void sdio_debug_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void sdio_debug_log(const char *format, ...)
{
   va_list args;
   char line[192];
   int written;

   if (!wifi_debug_enabled())
      return;

   va_start(args, format);
   written = vsnprintf(line, sizeof(line), format, args);
   va_end(args);

   if (written <= 0)
      return;

   wifi_debug_printf("WIFI-SDIO: %s\r\n", line);
}

#define SDIO_CCCR_CCCR_SDIO_REV 0x00u
#define SDIO_CCCR_IO_ENABLE 0x02u
#define SDIO_CCCR_IO_READY 0x03u
#define SDIO_CCCR_BUS_INTERFACE_CONTROL 0x07u
#define SDIO_CCCR_IO_ABORT 0x06u
#define SDIO_CCCR_FUNCTION2_INFO 0x200u
#define SDIO_FBR_BASE(function_number) ((uint32_t)(function_number) << 8)
#define SDIO_FBR_BLOCK_SIZE_LOW(function_number) (SDIO_FBR_BASE(function_number) + 0x10u)
#define SDIO_FBR_BLOCK_SIZE_HIGH(function_number) (SDIO_FBR_BASE(function_number) + 0x11u)
#define SDIO_PROBE_FUNCTION1_BLOCK_SIZE 64u
#define SDIO_PROBE_FUNCTION2_BLOCK_SIZE 512u
#define SDIO_BACKPLANE_ADDRESS_LOW 0x1000Au
#define SDIO_BACKPLANE_ADDRESS_MID 0x1000Bu
#define SDIO_BACKPLANE_ADDRESS_HIGH 0x1000Cu
#define SDIO_FUNCTION2_WATERMARK 0x10008u
#define SDIO_FRAME_CONTROL 0x1000Du
#define SDIO_CHIP_CLOCK_CSR 0x1000Eu
#define SDIO_READ_FRAME_BC_LOW 0x1001Bu
#define SDIO_READ_FRAME_BC_HIGH 0x1001Cu
#define SDIO_WAKEUP_CTRL 0x1001Eu
#define SDIO_SLEEP_CSR 0x1001Fu
#define CYW43_SDIO_CORE_BASE 0x18002000u
#define SDIO_CORE_INT_STATUS_OFFSET 0x20u
#define SDIO_CORE_INT_HOST_MASK_OFFSET 0x24u
#define SDIO_CORE_FUNCTION_INT_MASK_OFFSET 0x34u
#define SDIO_CORE_TO_SB_MAILBOX_OFFSET 0x40u
#define SDIO_CORE_SB_MBOX_DATA_OFFSET 0x48u
#define SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET 0x4Cu
#define SDIO_HOST_INTERRUPT_MASK 0x000000F0u
#define SDIO_BACKPLANE_OFFSET_MASK 0x07FFFu
#define SDIO_BACKPLANE_ACCESS_2_4B_FLAG 0x08000u
#define SDIO_FORCE_ALP 0x01u
#define SDIO_ALP_AVAIL_REQ 0x08u
#define SDIO_FORCE_HW_CLKREQ_OFF 0x20u
#define SDIO_ALP_AVAIL 0x40u
#define SDIO_OCR_READY 0x80000000u
#define SDIO_OCR_3P2_3P4 (3u << 20)
#define SDIO_FRAME_CONTROL_READ_TERMINATE 0x01u
#define SDIO_IO_ABORT_FUNCTION2 0x02u
#define SDIO_SLEEP_CSR_KEEP_WL_KSO 0x01u
#define SDIO_SLEEP_CSR_WL_DEVON 0x02u
#define SDIO_PULLUP_CONTROL 0x1000Fu
#define SDPCM_CHANNEL_MASK 0x0fu
#define SDPCM_CONTROL_CHANNEL 0u
#define SDPCM_EVENT_CHANNEL 1u
#define SDPCM_DATA_CHANNEL 2u
#define WLC_IOCTL_MAGIC 0x14e46c77u
#define AES_ENABLED 0x0004u
#define WPA_AUTH_DISABLED 0x0000u
#define WPA2_AUTH_PSK 0x0080u
#define WLC_GET_MAGIC 0u
#define WLC_GET_VERSION 1u
#define WLC_UP 2u
#define WLC_SET_INFRA 20u
#define WLC_SET_PM 86u
#define WLC_SET_AUTH 22u
#define WLC_SET_SSID 26u
#define WLC_SET_WSEC 134u
#define WLC_SET_WPA_AUTH 165u
#define WLC_SET_VAR 263u
#define WLC_GET_VAR 262u
#define WLC_SET_WSEC_PMK 268u
#define WLC_SCAN 50u
#define WLC_GET_BSSID 23u
#define WLC_DOWN 3u
#define WLC_GET_SSID 25u
#define WLC_GET_INFRA 19u
#define WLC_GET_AUTH 21u
#define WLC_GET_WSEC 133u
#define WLC_GET_WPA_AUTH 164u
#define WLC_SSID_MAX_LEN 32u
#define WSEC_MAX_PSK_LEN 64u
#define WSEC_PASSPHRASE 0x0001u
#define CYW_EAPOL_KEY_TIMEOUT 5000u
#define MFP_NONE 0u
#define MFP_CAPABLE 1u
#define SDPCM_CONTROL_EVENT_HEADER_LENGTH 12u
#define SDPCM_DATA_HEADER_LENGTH 14u
#define SDPCM_PREFIX_LENGTH 4u
#define BDC_VERSION_SHIFT 4u
#define BDC_PROTOCOL_VERSION 2u
#define CDC_HEADER_LENGTH 16u
#define CDCF_IOC_SET 0x02u
#define CDCF_IOC_ERROR 0x01u
#define CDCF_IOC_IF_MASK 0x0000f000u
#define CDCF_IOC_IF_SHIFT 12u
#define CDCF_IOC_ID_MASK 0xffff0000u
#define CDCF_IOC_ID_SHIFT 16u
#define TX_CONTROL_TEMPLATE_INTERFACE 0u
#define TX_CONTROL_TEMPLATE_MAX_PAYLOAD_LENGTH 80u
#define TX_CONTROL_PROBE_JOIN_COMMAND_COUNT 20u
/* "join" iovar value is wl_extjoin_params_t which is 70 bytes:
     wlc_ssid_t      ssid;          // 4 + 32 = 36 bytes
     wl_join_scan_t  scan_params;   // scan_type+pad+nprobes+active+passive+home = 20 bytes
     wl_assoc_params bssid+cnt+chanspec_num+chanspec_list[1] = 14 bytes
   The previous value of 68 dropped the trailing chanspec_list[0] which
   some firmware revisions reject (silently - the CDC ack still says 0)
   because the iovar size doesn't match the registered handler. */
#define TX_CONTROL_TEMPLATE_JOIN_PAYLOAD_LENGTH 70u

#define CYW43_ENUM_BASE 0x18000000u
#define CYW43_CHIPCOMMON_BASE 0x18000000u
#define CYW43_REQ_HT 0x10u
#define CYW43_HT_AVAIL 0x80u
#define CYW43_SB_PROTOCOL_VERSION 0x00000001u
#define CYW43_FRAME_INT_MASK 0x00000008u

#define CYW43_CORE_CHIPCOMMON_ID 0x800u
#define CYW43_CORE_ARM_CM3_ID 0x82au
#define CYW43_CORE_ARM_7_ID 0x817u
#define CYW43_CORE_ARM_CR4_ID 0x83eu
#define CYW43_CORE_SOCRAM_ID 0x80eu
#define CYW43_CORE_SDIO_DEV_ID 0x829u
#define CYW43_CORE_D11_ID 0x812u

#define CYW43_COREINFO_OFFSET 0x0000u
#define CYW43_CORE_IOCTRL_OFFSET 0x0408u
#define CYW43_CORE_RESETCTRL_OFFSET 0x0800u
#define CYW43_CORE_DISABLE_RESET_BIT 0x00000001u
#define CYW43_CORE_DISABLE_BITS 0x00000003u

#define CYW43_CR4_CPUHALT 0x00000020u
#define CYW43_CR4_CAP_OFFSET 0x0004u
#define CYW43_CR4_BANKIDX_OFFSET 0x0040u
#define CYW43_CR4_BANKINFO_OFFSET 0x0044u

#define CYW43_BANKIDX_OFFSET 0x0010u
#define CYW43_BANKINFO_OFFSET 0x0044u
#define CYW43_BANKPDA_OFFSET 0x0048u
#define CYW43_GPI_PULLUP_OFFSET 0x0058u
#define CYW43_GPI_PULLDOWN_OFFSET 0x005cu

#define ETHERNET_HEADER_LENGTH 14u
#define ETHER_TYPE_BRCM 0x886cu
#define BRCM_OUI0 0x00u
#define BRCM_OUI1 0x10u
#define BRCM_OUI2 0x18u
#define BRCM_EVENT_VERSION 2u
#define BRCM_EVENT_HEADER_LENGTH 10u
#define BRCM_EVENT_MSG_LENGTH 48u
#define BRCM_EVENT_MSG_ADDR_OFFSET 24u
#define BRCM_EVENT_MSG_IFNAME_OFFSET 30u
#define BRCM_EVENT_MSG_IFNAME_LENGTH 16u
#define BRCM_EVENT_MSG_IFIDX_OFFSET 46u
#define BRCM_EVENT_MSG_BSSCFGIDX_OFFSET 47u

typedef struct {
   uint16_t chip_id;
   uint8_t chip_revision;
   uint16_t arm_core;
   uint8_t socramrev;
   uint8_t sdiorev;
   uint32_t chipcommon;
   uint32_t armctl;
   uint32_t armregs;
   uint32_t socramctl;
   uint32_t socramregs;
   uint32_t sdregs;
   uint32_t d11ctl;
   uint32_t socramsize;
   uint32_t rambase;
   uint32_t reset_vector;
} sdio_chip_state_t;

static sdio_chip_state_t g_runtime_boot_chip;
static bool g_runtime_boot_fw_prepared;
static uint32_t g_runtime_boot_deadline_us;
static uint32_t g_runtime_boot_chip_id_register;
static unsigned int g_runtime_boot_wait_attempt;

typedef enum {
   SDIO_RUNTIME_BOOT_STAGE_PREPARE = 0,
   SDIO_RUNTIME_BOOT_STAGE_WAIT_HT_REQUEST,
   SDIO_RUNTIME_BOOT_STAGE_WAIT_HT_READY,
   SDIO_RUNTIME_BOOT_STAGE_WAIT_FN2_READY
} sdio_runtime_boot_stage_t;

static sdio_runtime_boot_stage_t g_runtime_boot_stage;

static const char *sdio_event_name(uint32_t event_type);
static bool sdio_event_is_link_up(uint32_t event_type,
                                  uint32_t event_status,
                                  uint32_t event_reason);
static bool sdio_event_is_link_down(uint32_t event_type);
static void sdio_runtime_set_error(const char *message);
static void sdio_runtime_boot_reset_state(void);
static int sdio_runtime_finalize_boot_stage(sdio_host_t *dev,
                                            uint8_t clock_csr,
                                            const sdio_chip_state_t *chip,
                                            uint32_t now_us);
static int sdio_runtime_complete_boot_stage(sdio_host_t *dev,
                                            sdio_probe_result_t *probe_result,
                                            const sdio_chip_state_t *chip,
                                            uint32_t chip_id_register);
static void sdio_runtime_set_host_command_error(const char *prefix,
                                                uint8_t function_number,
                                                uint32_t address,
                                                const sdio_host_result_t *host_result);
static bool sdio_card_identify(sdio_host_t *dev,
                               sdio_probe_result_t *probe_result,
                               bool report_runtime_errors);
static int sdio_runtime_card_identify_step(sdio_host_t *dev,
                                           sdio_probe_result_t *probe_result);
static int sdio_runtime_request_alp_clock_step(sdio_host_t *dev,
                                               sdio_probe_result_t *probe_result);
static int sdio_runtime_wake_with_kso_step(sdio_host_t *dev,
                                           sdio_probe_result_t *probe_result);
static bool sdio_cmd52_execute_timeout(sdio_host_t *dev, uint8_t function_number,
                                       uint32_t address, bool write,
                                       bool read_after_write, uint8_t *data,
                                       uint32_t timeout_us,
                                       sdio_cmd52_result_t *result);
static bool sdio_cmd53_execute_timeout(sdio_host_t *dev, uint8_t function_number,
                                       uint32_t address, bool write,
                                       bool block_mode,
                                       bool incrementing_address,
                                       uint16_t count, void *buffer,
                                       uint32_t block_size,
                                       uint32_t timeout_us,
                                       sdio_cmd53_result_t *result);
static bool sdio_backplane_set_window_timeout(sdio_host_t *dev, uint32_t address,
                                              uint32_t timeout_us);
static bool sdio_backplane_read_u32_timeout(sdio_host_t *dev, uint32_t address,
                                            uint32_t timeout_us,
                                            uint32_t *value);
static bool sdio_backplane_write_u32_timeout(sdio_host_t *dev, uint32_t address,
                                             uint32_t value,
                                             uint32_t timeout_us);
static bool sdio_function2_transfer_timeout(sdio_host_t *dev, bool write,
                                            uint8_t *buffer, uint16_t length,
                                            uint32_t timeout_us);
static bool sdio_backplane_set_window(sdio_host_t *dev, uint32_t address);
static bool sdio_backplane_read_u32(sdio_host_t *dev, uint32_t address, uint32_t *value);
static bool sdio_backplane_write_u32(sdio_host_t *dev, uint32_t address, uint32_t value);
static bool sdio_function1_read_byte(sdio_host_t *dev, uint32_t address, uint8_t *value);
static bool sdio_function1_write_byte(sdio_host_t *dev, uint32_t address, uint8_t value);
static bool sdio_probe_read_byte(sdio_host_t *dev, uint32_t address, uint8_t *value);
static bool sdio_probe_write_byte(sdio_host_t *dev, uint32_t address, uint8_t value);
static bool sdio_probe_set_block_size(sdio_host_t *dev, uint8_t function_number,
                                      uint16_t block_size);
static bool sdio_probe_write_interrupt_mask(sdio_host_t *dev,
                                            sdio_probe_result_t *probe_result);
static int sdio_runtime_boot_firmware(sdio_host_t *dev,
                                      sdio_probe_result_t *probe_result);
static bool sdio_probe_read_function2_registers(sdio_host_t *dev,
                                                sdio_probe_result_t *probe_result);
static bool sdio_probe_read_frame_header(sdio_host_t *dev,
                                         sdio_probe_result_t *probe_result);
static uint32_t sdio_load_u32_le(const uint8_t *src);
static void sdio_store_u16_le(uint8_t *dest, uint16_t value);
static void sdio_store_u32_le(uint8_t *dest, uint32_t value);

static const char *sdio_event_name(uint32_t event_type)
{
   switch (event_type) {
      case 0u: return "SET_SSID";
      case 3u: return "AUTH";
      case 5u: return "DEAUTH";
      case 6u: return "DEAUTH_IND";
      case 11u: return "DISASSOC";
      case 12u: return "DISASSOC_IND";
      case 16u: return "LINK";
      case 46u: return "PSK_SUP";
      default: return "OTHER";
   }
}

static bool sdio_event_is_link_up(uint32_t event_type,
                                  uint32_t event_status,
                                  uint32_t event_reason)
{
   return event_type == 16u && event_status == 0u && event_reason == 0u;
}

static bool sdio_event_is_link_down(uint32_t event_type)
{
   return event_type == 5u || event_type == 6u
      || event_type == 11u || event_type == 12u;
}

static void sdio_runtime_set_error(const char *message)
{
   if (message == NULL) {
      g_runtime_error[0] = '\0';
      return;
   }

   strlcpy(g_runtime_error, message, sizeof(g_runtime_error));
   sdio_debug_log("error: %s", g_runtime_error);
}

static void sdio_runtime_boot_reset_state(void)
{
   g_runtime_boot_fw_prepared = false;
   g_runtime_boot_deadline_us = 0u;
   g_runtime_boot_chip_id_register = 0u;
   g_runtime_boot_wait_attempt = 0u;
   g_runtime_boot_stage = SDIO_RUNTIME_BOOT_STAGE_PREPARE;
   memset(&g_runtime_boot_chip, 0, sizeof(g_runtime_boot_chip));
}

static int sdio_runtime_finalize_boot_stage(sdio_host_t *dev,
                                            uint8_t clock_csr,
                                            const sdio_chip_state_t *chip,
                                            uint32_t now_us)
{
   uint8_t io_enable = 0u;

   if (chip == NULL)
      return -1;

   /* Emulator workaround: if HT_AVAIL never appeared but the register is
      responding (has any non-zero bits), assume firmware is partially running
      and proceed. Real hardware with firmware running will set HT_AVAIL. */
   if ((clock_csr & CYW43_HT_AVAIL) == 0u) {
      if (clock_csr == 0u) {
         sdio_runtime_set_error("Timed out waiting for CYW43 HT clock");
         sdio_runtime_boot_reset_state();
         return -1;
      }
      g_runtime_emulator_mode = true;
      sdio_debug_log("emulator: HT_AVAIL not set but CSR=0x%02x; proceeding anyway",
                     (unsigned int)clock_csr);
   }

   clock_csr = (uint8_t)(clock_csr | CYW43_REQ_HT | SDIO_FORCE_ALP);
   if (!sdio_function1_write_byte(dev, SDIO_CHIP_CLOCK_CSR, clock_csr)
      || !sdio_backplane_write_u32(dev, chip->sdregs + SDIO_CORE_SB_MBOX_DATA_OFFSET,
                                   CYW43_SB_PROTOCOL_VERSION)
      || !sdio_backplane_write_u32(dev, chip->sdregs + SDIO_CORE_INT_HOST_MASK_OFFSET,
                                   CYW43_FRAME_INT_MASK)
      || !sdio_backplane_write_u32(dev, chip->sdregs + SDIO_CORE_FUNCTION_INT_MASK_OFFSET,
                                   0x00000003u)) { /* enable fn1 + fn2 interrupts */
      sdio_runtime_set_error("Failed to finalize CYW43 runtime clocking");
      sdio_runtime_boot_reset_state();
      return -1;
   }

   /* Enable function 2 (radio data path) now that the firmware is
      running. Function 2 cannot be enabled before this point because
      the firmware must initialise its SDIO DMA engine first. */
   if (!sdio_probe_read_byte(dev, SDIO_CCCR_IO_ENABLE, &io_enable)) {
      sdio_runtime_set_error("Failed to read CYW43 function enable register");
      sdio_runtime_boot_reset_state();
      return -1;
   }

   io_enable |= 0x04u; /* bit 2 = function 2 */
   if (!sdio_probe_write_byte(dev, SDIO_CCCR_IO_ENABLE, io_enable)) {
      sdio_runtime_set_error("Failed to enable CYW43 function 2");
      sdio_runtime_boot_reset_state();
      return -1;
   }

   g_runtime_boot_stage = SDIO_RUNTIME_BOOT_STAGE_WAIT_FN2_READY;
   g_runtime_boot_wait_attempt = 0u;
   g_runtime_boot_deadline_us = now_us + 1000u;
   return 0;
}

static int sdio_runtime_complete_boot_stage(sdio_host_t *dev,
                                            sdio_probe_result_t *probe_result,
                                            const sdio_chip_state_t *chip,
                                            uint32_t chip_id_register)
{
   if (chip == NULL)
      return -1;

   /* Watermark: minimum bytes in firmware TX FIFO before READ_FRAME_BC
      is updated and the host is notified. 0x08 (8 bytes) matches
      SDIO_F2_WATERMARK in cyw43-driver. */
   if (!sdio_function1_write_byte(dev, SDIO_FUNCTION2_WATERMARK, 0x08u)) {
      sdio_runtime_set_error("Failed to program CYW43 function 2 watermark");
      sdio_runtime_boot_reset_state();
      return -1;
   }

   if (probe_result != NULL) {
      probe_result->backplane_probe_success = true;
      probe_result->chipcommon_id_register = chip_id_register;
      probe_result->chip_id = chip->chip_id;
      probe_result->chip_revision = chip->chip_revision;
      probe_result->sdio_core_base = chip->sdregs;
   }

   cyw43_release_images();
   sdio_runtime_boot_reset_state();
   return 1;
}

static void sdio_runtime_set_host_command_error(const char *prefix,
                                                uint8_t function_number,
                                                uint32_t address,
                                                const sdio_host_result_t *host_result)
{
   char message[96];

   if (host_result == NULL) {
      sdio_runtime_set_error(prefix);
      return;
   }

   snprintf(message, sizeof(message), "%s fn=%u addr=0x%05lx int=0x%08lx err=0x%08lx",
            prefix,
            (unsigned int)function_number,
            (unsigned long)address,
            (unsigned long)host_result->interrupt,
            (unsigned long)host_result->error);
   sdio_runtime_set_error(message);
}

static int sdio_runtime_boot_firmware_step(sdio_host_t *dev, sdio_probe_result_t *probe_result)
{
   return sdio_runtime_boot_firmware(dev, probe_result);
}

static int sdio_runtime_card_identify_step(sdio_host_t *dev,
                                           sdio_probe_result_t *probe_result)
{
   sdio_host_command_t command;
   sdio_host_result_t host_result;
   uint32_t now_us;

   if (dev == NULL || probe_result == NULL)
      return -1;

   memset(&command, 0, sizeof(command));
   memset(&host_result, 0, sizeof(host_result));
   now_us = RPI_GetSystemTime();

   if (!g_runtime_identify_started) {
      command.command = (5u << 24) | (2u << 16);
      command.argument = 0u;
      command.timeout_us = 100000u;
      (void)sdio_host_submit(dev, &command, &host_result);

      g_runtime_identify_started = true;
      g_runtime_identify_attempt = 0u;
      g_runtime_identify_deadline_us = now_us;
   }

   if ((int32_t)(now_us - g_runtime_identify_deadline_us) < 0)
      return 0;

   command.command = (5u << 24) | (2u << 16);
   command.argument = SDIO_OCR_3P2_3P4;
   command.timeout_us = 100000u;

   if (sdio_host_submit(dev, &command, &host_result) == 0) {
      probe_result->success = true;
      probe_result->response0 = host_result.response0;
      probe_result->interrupt = host_result.interrupt;
      probe_result->error = host_result.error;
      probe_result->ocr = sdio_decode_ocr(host_result.response0);

      if (((host_result.response0 & SDIO_OCR_READY) != 0u) || (g_runtime_identify_attempt >= 2u)) {
         uint32_t rca;

         memset(&command, 0, sizeof(command));
         command.command = (3u << 24) | (2u << 16) | (1u << 19) | (1u << 20);
         command.argument = 0u;
         command.timeout_us = 100000u;
         if (sdio_host_submit(dev, &command, &host_result) != 0) {
            probe_result->interrupt = host_result.interrupt;
            probe_result->error = host_result.error;
            sdio_runtime_set_host_command_error("CMD3 failed", 0u, 0u,
                                                &host_result);
            return -1;
         }

         rca = (host_result.response0 >> 16) & 0xffffu;
         probe_result->response0 = host_result.response0;

         memset(&command, 0, sizeof(command));
         command.command = (7u << 24) | (2u << 16) | (1u << 19) | (1u << 20);
         command.argument = rca << 16;
         command.timeout_us = 100000u;
         if (sdio_host_submit(dev, &command, &host_result) != 0) {
            probe_result->interrupt = host_result.interrupt;
            probe_result->error = host_result.error;
            sdio_runtime_set_host_command_error("CMD7 failed", 0u, 0u,
                                                &host_result);
            return -1;
         }

         probe_result->response0 = host_result.response0;
         g_runtime_identify_started = false;
         return 1;
      }
   } else {
      probe_result->interrupt = host_result.interrupt;
      probe_result->error = host_result.error;
      probe_result->response0 = host_result.response0;
   }

   g_runtime_identify_attempt++;
   if (g_runtime_identify_attempt >= 5u) {
      if (!probe_result->success)
         sdio_runtime_set_host_command_error("CMD5 failed", 0u, 0u, &host_result);
      else
         sdio_runtime_set_error("Timed out waiting for SDIO OCR ready");
      return -1;
   }

   g_runtime_identify_deadline_us = now_us + 100000u;
   return 0;
}

static int sdio_runtime_request_alp_clock_step(sdio_host_t *dev,
                                               sdio_probe_result_t *probe_result)
{
   uint8_t clock_csr = 0u;
   uint8_t requested_clock_csr;
   uint32_t now_us;

   if (dev == NULL || probe_result == NULL)
      return -1;

   now_us = RPI_GetSystemTime();

   if (!g_runtime_alp_wait.active) {
      probe_result->clock_probe_attempted = true;

      if (!sdio_function1_read_byte(dev, SDIO_CHIP_CLOCK_CSR, &clock_csr))
         return -1;

      probe_result->chip_clock_csr_initial = clock_csr;
      requested_clock_csr = (uint8_t)(clock_csr | SDIO_FORCE_HW_CLKREQ_OFF
         | SDIO_ALP_AVAIL_REQ | SDIO_FORCE_ALP);
      probe_result->chip_clock_csr_requested = requested_clock_csr;

      if (!sdio_function1_write_byte(dev, SDIO_CHIP_CLOCK_CSR, requested_clock_csr))
         return -1;

      g_runtime_alp_wait.active = true;
      g_runtime_alp_wait.attempt = 0u;
      g_runtime_alp_wait.deadline_us = now_us;
   }

   if ((int32_t)(now_us - g_runtime_alp_wait.deadline_us) < 0)
      return 0;

   if (!sdio_function1_read_byte(dev, SDIO_CHIP_CLOCK_CSR, &clock_csr)) {
      memset(&g_runtime_alp_wait, 0, sizeof(g_runtime_alp_wait));
      return -1;
   }

   probe_result->chip_clock_csr_final = clock_csr;
   if ((clock_csr & SDIO_ALP_AVAIL) != 0u) {
      probe_result->clock_probe_success = true;
      memset(&g_runtime_alp_wait, 0, sizeof(g_runtime_alp_wait));
      return 1;
   }

   if (g_runtime_alp_wait.attempt >= 99u) {
      sdio_runtime_set_error("Timed out waiting for SDIO ALP clock");
      memset(&g_runtime_alp_wait, 0, sizeof(g_runtime_alp_wait));
      return -1;
   }

   g_runtime_alp_wait.attempt++;
   g_runtime_alp_wait.deadline_us = now_us + 1000u;
   return 0;
}

static int sdio_runtime_wake_with_kso_step(sdio_host_t *dev,
                                           sdio_probe_result_t *probe_result)
{
   uint8_t requested_value;
   uint8_t sleep_control_status = 0u;
   uint32_t now_us;

   if (dev == NULL || probe_result == NULL)
      return -1;

   now_us = RPI_GetSystemTime();

   requested_value = (uint8_t)(probe_result->sleep_control_status | SDIO_SLEEP_CSR_KEEP_WL_KSO);
   if (!g_runtime_kso_wait.active) {
      probe_result->kso_probe_attempted = true;
      probe_result->kso_control_requested = requested_value;

      (void)sdio_function1_write_byte(dev, SDIO_SLEEP_CSR, requested_value);
      (void)sdio_function1_write_byte(dev, SDIO_SLEEP_CSR, requested_value);

      g_runtime_kso_wait.active = true;
      g_runtime_kso_wait.attempt = 0u;
      g_runtime_kso_wait.deadline_us = now_us;
   }

   if ((int32_t)(now_us - g_runtime_kso_wait.deadline_us) < 0)
      return 0;

   if (!sdio_function1_read_byte(dev, SDIO_SLEEP_CSR, &sleep_control_status)) {
      memset(&g_runtime_kso_wait, 0, sizeof(g_runtime_kso_wait));
      return -1;
   }

   probe_result->kso_control_final = sleep_control_status;
   probe_result->sleep_control_status = sleep_control_status;
   if ((sleep_control_status & (SDIO_SLEEP_CSR_KEEP_WL_KSO | SDIO_SLEEP_CSR_WL_DEVON))
      == (SDIO_SLEEP_CSR_KEEP_WL_KSO | SDIO_SLEEP_CSR_WL_DEVON)) {
      probe_result->kso_probe_success = true;
      memset(&g_runtime_kso_wait, 0, sizeof(g_runtime_kso_wait));
      return 1;
   }

   if (g_runtime_kso_wait.attempt >= 63u) {
      sdio_runtime_set_error("Timed out waking SDIO core with KSO");
      memset(&g_runtime_kso_wait, 0, sizeof(g_runtime_kso_wait));
      return -1;
   }

   g_runtime_kso_wait.attempt++;
   g_runtime_kso_wait.deadline_us = now_us + 1000u;
   (void)sdio_function1_write_byte(dev, SDIO_SLEEP_CSR, requested_value);
   return 0;
}

static bool sdio_card_identify(sdio_host_t *dev,
                               sdio_probe_result_t *probe_result,
                               bool report_runtime_errors)
{
   sdio_host_command_t command;
   sdio_host_result_t host_result;
   unsigned int attempt;

   if (dev == NULL || probe_result == NULL)
      return false;

   memset(&command, 0, sizeof(command));
   memset(&host_result, 0, sizeof(host_result));
   /* CMD5 returns R4 which has no CRC or command index - bits 19+20 must NOT be set */
   command.command = (5u << 24) | (2u << 16);
   command.argument = 0u;
   command.timeout_us = 100000u;

   (void)sdio_host_submit(dev, &command, &host_result);

   command.argument = SDIO_OCR_3P2_3P4;
   for (attempt = 0u; attempt < 5u; ++attempt) {
      if (sdio_host_submit(dev, &command, &host_result) == 0) {
         probe_result->success = true;
         probe_result->response0 = host_result.response0;
         probe_result->interrupt = host_result.interrupt;
         probe_result->error = host_result.error;
         probe_result->ocr = sdio_decode_ocr(host_result.response0);

         /* SDIO_OCR_READY may not be set in emulator environments, but the card
            should still respond to CMD3/CMD7. Proceed after 2 retries even if
            ready bit isn't set to support emulator testing. */
         if (((host_result.response0 & SDIO_OCR_READY) != 0u) || (attempt >= 2u)) {
            uint32_t rca;

            memset(&command, 0, sizeof(command));
            command.command = (3u << 24) | (2u << 16) | (1u << 19) | (1u << 20);
            command.argument = 0u;
            command.timeout_us = 100000u;
            if (sdio_host_submit(dev, &command, &host_result) != 0) {
               probe_result->interrupt = host_result.interrupt;
               probe_result->error = host_result.error;
               if (report_runtime_errors)
                  sdio_runtime_set_host_command_error("CMD3 failed", 0u, 0u,
                                                      &host_result);
               return false;
            }
            rca = (host_result.response0 >> 16) & 0xffffu;
            probe_result->response0 = host_result.response0;

            memset(&command, 0, sizeof(command));
            command.command = (7u << 24) | (2u << 16) | (1u << 19) | (1u << 20);
            command.argument = rca << 16;
            command.timeout_us = 100000u;
            if (sdio_host_submit(dev, &command, &host_result) != 0) {
               probe_result->interrupt = host_result.interrupt;
               probe_result->error = host_result.error;
               if (report_runtime_errors)
                  sdio_runtime_set_host_command_error("CMD7 failed", 0u, 0u,
                                                      &host_result);
               return false;
            }
            probe_result->response0 = host_result.response0;
            return true;
         }
      } else {
         probe_result->interrupt = host_result.interrupt;
         probe_result->error = host_result.error;
         probe_result->response0 = host_result.response0;
      }

      usleep(100000u);
   }

   if (report_runtime_errors) {
      if (!probe_result->success)
         sdio_runtime_set_host_command_error("CMD5 failed", 0u, 0u, &host_result);
      else
         sdio_runtime_set_error("Timed out waiting for SDIO OCR ready");
   }

   return false;
}

static bool sdio_backplane_transfer_bytes(sdio_host_t *dev, bool write, uint32_t address,
                                          uint8_t *buffer, uint32_t length)
{
   while (length != 0u) {
      uint32_t window_offset = address & (SDIO_BACKPLANE_WINDOW_SIZE - 1u);
      uint32_t window_remaining = SDIO_BACKPLANE_WINDOW_SIZE - window_offset;
      uint32_t chunk = length < window_remaining ? length : window_remaining;
      uint32_t word_chunk;
      uint32_t block_chunk;

      if (!sdio_backplane_set_window(dev, address))
         return false;

      /* Big transfers must use block mode against function 1's configured
         block size (SDIO_PROBE_FUNCTION1_BLOCK_SIZE = 64). The BCM43438
         backplane returns DCRC errors when a single CMD53 byte-mode read
         covers more than a few bytes, so anything that fits a whole
         number of blocks is sent block-mode first. */
      block_chunk = (chunk / SDIO_PROBE_FUNCTION1_BLOCK_SIZE) * SDIO_PROBE_FUNCTION1_BLOCK_SIZE;
      if (block_chunk != 0u) {
         uint32_t transfer_address = window_offset | SDIO_BACKPLANE_ACCESS_2_4B_FLAG;
         uint16_t block_count =
            (uint16_t)(block_chunk / SDIO_PROBE_FUNCTION1_BLOCK_SIZE);

         if (!sdio_cmd53_execute(dev, 1u, transfer_address, write, true, true,
                                 block_count, buffer,
                                 SDIO_PROBE_FUNCTION1_BLOCK_SIZE, NULL)) {
            return false;
         }

         address += block_chunk;
         buffer += block_chunk;
         length -= block_chunk;
         chunk -= block_chunk;
         window_offset += block_chunk;
      }

      /* Sub-block remainder: still has to be 4-byte aligned to use the
         backplane 4B flag, so fall back to byte mode for the word-aligned
         remainder, then CMD52 for the trailing bytes. */
      word_chunk = chunk & ~0x3u;
      if (word_chunk != 0u) {
         uint32_t transfer_address = window_offset | SDIO_BACKPLANE_ACCESS_2_4B_FLAG;

         if (!sdio_cmd53_execute(dev, 1u, transfer_address, write, false, true,
                                 (uint16_t)word_chunk, buffer, word_chunk, NULL)) {
            return false;
         }

         address += word_chunk;
         buffer += word_chunk;
         length -= word_chunk;
         chunk -= word_chunk;
         window_offset += word_chunk;
      }

      while (chunk != 0u) {
         uint8_t value = write ? *buffer : 0u;
         uint32_t transfer_address = window_offset | SDIO_BACKPLANE_ACCESS_2_4B_FLAG;

         if (!sdio_cmd52_execute(dev, 1u, transfer_address, write, write, &value, NULL))
            return false;

         if (!write)
            *buffer = value;

         ++address;
         ++buffer;
         --length;
         --chunk;
         ++window_offset;
      }
   }

   return true;
}

static bool sdio_backplane_read_bytes(sdio_host_t *dev, uint32_t address, uint8_t *buffer,
                                      uint32_t length)
{
   if (buffer == NULL)
      return false;

   return sdio_backplane_transfer_bytes(dev, false, address, buffer, length);
}

static bool sdio_backplane_write_bytes(sdio_host_t *dev, uint32_t address, const uint8_t *buffer,
                                       uint32_t length)
{
   if (buffer == NULL)
      return false;

   return sdio_backplane_transfer_bytes(dev, true, address, (uint8_t *)(uintptr_t)buffer, length);
}

static bool sdio_backplane_disable_core(sdio_host_t *dev, uint32_t regs, uint32_t pre_reset,
                                        uint32_t io_control)
{
   uint32_t reset_control;

   if (regs == 0u)
      return false;

   if (!sdio_backplane_read_u32(dev, regs + CYW43_CORE_RESETCTRL_OFFSET, &reset_control))
      return false;

   if ((reset_control & CYW43_CORE_DISABLE_RESET_BIT) != 0u)
      return sdio_backplane_write_u32(dev, regs + CYW43_CORE_IOCTRL_OFFSET,
                                      CYW43_CORE_DISABLE_BITS | io_control);

   return sdio_backplane_write_u32(dev, regs + CYW43_CORE_IOCTRL_OFFSET,
                                   CYW43_CORE_DISABLE_BITS | pre_reset)
      && sdio_backplane_read_u32(dev, regs + CYW43_CORE_IOCTRL_OFFSET, &reset_control)
      && sdio_backplane_write_u32(dev, regs + CYW43_CORE_RESETCTRL_OFFSET,
                                  CYW43_CORE_DISABLE_RESET_BIT)
      && sdio_backplane_write_u32(dev, regs + CYW43_CORE_IOCTRL_OFFSET,
                                  CYW43_CORE_DISABLE_BITS | io_control)
      && sdio_backplane_read_u32(dev, regs + CYW43_CORE_IOCTRL_OFFSET, &reset_control);
}

static bool sdio_backplane_reset_core(sdio_host_t *dev, uint32_t regs, uint32_t pre_reset,
                                      uint32_t io_control)
{
   uint32_t reset_control;
   unsigned int attempts;

   if (!sdio_backplane_disable_core(dev, regs, pre_reset, io_control))
      return false;

   for (attempts = 0; attempts < 64u; ++attempts) {
      if (!sdio_backplane_read_u32(dev, regs + CYW43_CORE_RESETCTRL_OFFSET, &reset_control))
         return false;

      if ((reset_control & CYW43_CORE_DISABLE_RESET_BIT) == 0u)
         break;

      if (!sdio_backplane_write_u32(dev, regs + CYW43_CORE_RESETCTRL_OFFSET, 0u))
         return false;

      usleep(40u);
   }

   return sdio_backplane_write_u32(dev, regs + CYW43_CORE_IOCTRL_OFFSET, 1u | io_control)
      && sdio_backplane_read_u32(dev, regs + CYW43_CORE_IOCTRL_OFFSET, &reset_control);
}

static bool sdio_backplane_scan_cores(sdio_host_t *dev, sdio_chip_state_t *chip,
                                      uint32_t enumeration_address)
{
   uint8_t scan_buffer[SDIO_CORE_SCAN_SIZE];
   uint16_t core_id = 0u;
   uint8_t core_revision = 0u;
   uint32_t index;

   if (chip == NULL)
      return false;

   memset(scan_buffer, 0, sizeof(scan_buffer));
   if (!sdio_backplane_read_bytes(dev, enumeration_address, scan_buffer, sizeof(scan_buffer)))
      return false;

   for (index = 0u; index < sizeof(scan_buffer); index += 4u) {
      uint8_t descriptor_type = scan_buffer[index] & 0x0fu;

      if (descriptor_type == 0x0fu)
         break;

      if (descriptor_type == 0x01u) {
         if ((index + 7u) < sizeof(scan_buffer) && (scan_buffer[index + 4u] & 0x0fu) == 0x01u) {
            core_id = (uint16_t)(((uint16_t)scan_buffer[index + 1u] | ((uint16_t)scan_buffer[index + 2u] << 8))
               & 0x0fffu);
            index += 4u;
            core_revision = scan_buffer[index + 3u];
         }
         continue;
      }

      if (descriptor_type == 0x05u) {
         uint32_t address = ((uint32_t)scan_buffer[index + 1u] << 8)
            | ((uint32_t)scan_buffer[index + 2u] << 16)
            | ((uint32_t)scan_buffer[index + 3u] << 24);

         address &= ~0x0fffu;
         switch (core_id) {
            case CYW43_CORE_CHIPCOMMON_ID:
               if ((scan_buffer[index] & 0xc0u) == 0u)
                  chip->chipcommon = address;
               break;
            case CYW43_CORE_ARM_CM3_ID:
            case CYW43_CORE_ARM_7_ID:
            case CYW43_CORE_ARM_CR4_ID:
               chip->arm_core = core_id;
               if ((scan_buffer[index] & 0xc0u) != 0u) {
                  if (chip->armctl == 0u)
                     chip->armctl = address;
               } else if (chip->armregs == 0u) {
                  chip->armregs = address;
               }
               break;
            case CYW43_CORE_SOCRAM_ID:
               if ((scan_buffer[index] & 0xc0u) != 0u)
                  chip->socramctl = address;
               else if (chip->socramregs == 0u)
                  chip->socramregs = address;
               chip->socramrev = core_revision;
               break;
            case CYW43_CORE_SDIO_DEV_ID:
               if ((scan_buffer[index] & 0xc0u) == 0u)
                  chip->sdregs = address;
               chip->sdiorev = core_revision;
               break;
            case CYW43_CORE_D11_ID:
               if ((scan_buffer[index] & 0xc0u) != 0u)
                  chip->d11ctl = address;
               break;
            default:
               break;
         }
      }
   }

   return chip->chipcommon != 0u && chip->armctl != 0u && chip->d11ctl != 0u;
}

static bool sdio_backplane_scan_ram(sdio_host_t *dev, sdio_chip_state_t *chip)
{
   if (chip == NULL)
      return false;

   if (chip->arm_core == CYW43_CORE_ARM_CR4_ID) {
      uint32_t capabilities;
      uint32_t bank_index;
      uint32_t size = 0u;
      uint32_t banks;

      if (!sdio_backplane_read_u32(dev, chip->armregs + CYW43_CR4_CAP_OFFSET, &capabilities))
         return false;

      banks = ((capabilities >> 4) & 0x0fu) + (capabilities & 0x0fu);
      for (bank_index = 0u; bank_index < banks; ++bank_index) {
         uint32_t bank_info;

         if (!sdio_backplane_write_u32(dev, chip->armregs + CYW43_CR4_BANKIDX_OFFSET, bank_index)
            || !sdio_backplane_read_u32(dev, chip->armregs + CYW43_CR4_BANKINFO_OFFSET, &bank_info)) {
            return false;
         }

         size += 8192u * ((bank_info & 0x3fu) + 1u);
      }

      chip->socramsize = size;
      chip->rambase = 0x198000u;
      return true;
   }

   if (chip->socramctl == 0u || chip->socramregs == 0u || chip->socramrev <= 7u || chip->socramrev == 12u)
      return false;

   if (!sdio_backplane_reset_core(dev, chip->socramctl, 0u, 0u))
      return false;

   {
      uint32_t core_info;
      uint32_t bank_index;
      uint32_t size = 0u;
      uint32_t banks;

      if (!sdio_backplane_read_u32(dev, chip->socramregs + CYW43_COREINFO_OFFSET, &core_info))
         return false;

      banks = (core_info >> 4) & 0x0fu;
      for (bank_index = 0u; bank_index < banks; ++bank_index) {
         uint32_t bank_info;

         if (!sdio_backplane_write_u32(dev, chip->socramregs + CYW43_BANKIDX_OFFSET, bank_index)
            || !sdio_backplane_read_u32(dev, chip->socramregs + CYW43_BANKINFO_OFFSET, &bank_info)) {
            return false;
         }

         size += 8192u * ((bank_info & 0x3fu) + 1u);
      }

      chip->socramsize = size;
      chip->rambase = 0u;
      if (chip->chip_id == 43430u) {
         if (!sdio_backplane_write_u32(dev, chip->socramregs + CYW43_BANKIDX_OFFSET, 3u)
            || !sdio_backplane_write_u32(dev, chip->socramregs + CYW43_BANKPDA_OFFSET, 0u)) {
            return false;
         }
      }
   }

   return chip->socramsize != 0u;
}

static uint32_t sdio_cyw43_condense_nvram(uint8_t *buffer, uint32_t length)
{
   const uint8_t *read_ptr = buffer;
   uint8_t *write_ptr = buffer;
   const uint8_t *line_start = buffer;
   const uint8_t *end = buffer + length;
   bool skipping = false;

   while (read_ptr < end) {
      uint8_t ch = *read_ptr++;

      switch (ch) {
         case '#':
            skipping = true;
            break;
         case '\r':
            break;
         case '\n':
            if (!skipping && write_ptr != line_start)
               *write_ptr++ = '\0';
            line_start = write_ptr;
            skipping = false;
            break;
         default:
            if (!skipping)
               *write_ptr++ = ch;
            break;
      }
   }

   if (!skipping && write_ptr != line_start)
      *write_ptr++ = '\0';

   *write_ptr++ = '\0';
   while ((((uintptr_t)(write_ptr - buffer)) & 0x3u) != 0u)
      *write_ptr++ = '\0';

   return (uint32_t)(write_ptr - buffer);
}

static int sdio_runtime_boot_firmware(sdio_host_t *dev, sdio_probe_result_t *probe_result)
{
   sdio_chip_state_t chip;
   uint8_t *condensed_nvram;
   uint32_t chip_id_register;
   uint32_t enumeration_address;
   uint32_t nvram_length;
   uint32_t nvram_token;
   uint32_t firmware_offset;
   uint8_t zero_tail[4] = {0u, 0u, 0u, 0u};
   uint8_t token_buffer[4];
   uint8_t clock_csr = 0u;
   uint32_t now_us;

   if (g_cyw43_firmware_data == NULL || g_cyw43_nvram_data == NULL
      || g_cyw43_firmware_length == 0u || g_cyw43_nvram_length == 0u) {
      sdio_runtime_set_error("CYW43 images are not preloaded");
      return -1;
   }

   now_us = RPI_GetSystemTime();
   memset(&chip, 0, sizeof(chip));
   condensed_nvram = NULL;

   if (g_runtime_boot_fw_prepared) {
      chip = g_runtime_boot_chip;
      chip_id_register = g_runtime_boot_chip_id_register;

      if (g_runtime_boot_stage == SDIO_RUNTIME_BOOT_STAGE_WAIT_HT_REQUEST) {
         if ((int32_t)(now_us - g_runtime_boot_deadline_us) < 0)
            return 0;

         if (!sdio_function1_write_byte(dev, SDIO_CHIP_CLOCK_CSR, CYW43_REQ_HT)) {
            sdio_runtime_set_error("Failed to request CYW43 HT clock");
            sdio_runtime_boot_reset_state();
            return -1;
         }

         g_runtime_boot_stage = SDIO_RUNTIME_BOOT_STAGE_WAIT_HT_READY;
         g_runtime_boot_wait_attempt = 0u;
         g_runtime_boot_deadline_us = now_us;
      }

      if (g_runtime_boot_stage == SDIO_RUNTIME_BOOT_STAGE_WAIT_HT_READY) {
         if ((int32_t)(now_us - g_runtime_boot_deadline_us) < 0)
            return 0;

         if (!sdio_function1_read_byte(dev, SDIO_CHIP_CLOCK_CSR, &clock_csr)) {
            sdio_runtime_set_error("Failed to poll CYW43 HT clock");
            sdio_runtime_boot_reset_state();
            return -1;
         }

         if ((clock_csr & CYW43_HT_AVAIL) == 0u && g_runtime_boot_wait_attempt < 19u) {
            g_runtime_boot_wait_attempt++;
            g_runtime_boot_deadline_us = now_us + 50000u;
            return 0;
         }

         if ((clock_csr & CYW43_HT_AVAIL) != 0u)
            sdio_debug_log("HT clock ready after %ums",
                           (unsigned int)(g_runtime_boot_wait_attempt * 50u));
      }

      if (g_runtime_boot_stage == SDIO_RUNTIME_BOOT_STAGE_WAIT_FN2_READY) {
         uint8_t io_ready = 0u;

         if ((int32_t)(now_us - g_runtime_boot_deadline_us) < 0)
            return 0;

         if (!sdio_probe_read_byte(dev, SDIO_CCCR_IO_READY, &io_ready)) {
            sdio_runtime_set_error("Failed to poll CYW43 function 2 ready");
            sdio_runtime_boot_reset_state();
            return -1;
         }

         if ((io_ready & 0x04u) == 0u && g_runtime_boot_wait_attempt < 99u) {
            g_runtime_boot_wait_attempt++;
            g_runtime_boot_deadline_us = now_us + 1000u;
            return 0;
         }

         if ((io_ready & 0x04u) == 0u) {
            g_runtime_emulator_mode = true;
            sdio_debug_log("emulator: function 2 ready bit not set after %ums; proceeding anyway",
                           (unsigned int)g_runtime_boot_wait_attempt);
         }

         return sdio_runtime_complete_boot_stage(dev, probe_result, &chip,
                                                 chip_id_register);
      }

      return sdio_runtime_finalize_boot_stage(dev, clock_csr, &chip, now_us);
   }

   if (!sdio_backplane_read_u32(dev, CYW43_ENUM_BASE, &chip_id_register)) {
      sdio_runtime_set_error("Failed to read CYW43 chip ID");
      return -1;
   }

   chip.chip_id = (uint16_t)(chip_id_register & 0xffffu);
   chip.chip_revision = (uint8_t)((chip_id_register >> 16) & 0x0fu);
   if (chip.chip_id != 43430u) {
      sdio_runtime_set_error("Unsupported CYW43 chip ID");
      return -1;
   }

   if (!sdio_backplane_read_u32(dev, CYW43_ENUM_BASE + (63u * 4u), &enumeration_address)
      || !sdio_backplane_scan_cores(dev, &chip, enumeration_address)) {
      sdio_runtime_set_error("Failed to enumerate CYW43 backplane cores");
      return -1;
   }

   if (chip.arm_core == CYW43_CORE_ARM_CR4_ID) {
      if (!sdio_backplane_reset_core(dev, chip.armctl, CYW43_CR4_CPUHALT, CYW43_CR4_CPUHALT)) {
         sdio_runtime_set_error("Failed to halt CYW43 CR4 core");
         return -1;
      }
   } else if (!sdio_backplane_disable_core(dev, chip.armctl, 0u, 0u)) {
      sdio_runtime_set_error("Failed to halt CYW43 ARM core");
      return -1;
   }

   if (!sdio_backplane_reset_core(dev, chip.d11ctl, 0x0cu, 0x04u)
      || !sdio_backplane_scan_ram(dev, &chip)) {
      sdio_runtime_set_error("Failed to prepare CYW43 RAM");
      return -1;
   }

   if (!sdio_function1_write_byte(dev, SDIO_CHIP_CLOCK_CSR, 0u)) {
      sdio_runtime_set_error("Failed to reset CYW43 clock request");
      return -1;
   }

   usleep(10u);
   if (!sdio_function1_write_byte(dev, SDIO_CHIP_CLOCK_CSR,
                                  (uint8_t)(SDIO_FORCE_HW_CLKREQ_OFF | SDIO_ALP_AVAIL_REQ))) {
      sdio_runtime_set_error("Failed to request CYW43 ALP clock");
      return -1;
   }

   for (g_runtime_boot_wait_attempt = 0u; g_runtime_boot_wait_attempt < 100u; ++g_runtime_boot_wait_attempt) {
      if (!sdio_function1_read_byte(dev, SDIO_CHIP_CLOCK_CSR, &clock_csr)) {
         sdio_runtime_set_error("Failed to poll CYW43 ALP clock");
         return -1;
      }

      if ((clock_csr & (CYW43_HT_AVAIL | SDIO_ALP_AVAIL)) != 0u)
         break;

      usleep(10u);
   }

   if (!sdio_function1_write_byte(dev, SDIO_CHIP_CLOCK_CSR,
                                  (uint8_t)(SDIO_FORCE_HW_CLKREQ_OFF | SDIO_FORCE_ALP))) {
      sdio_runtime_set_error("Failed to force CYW43 ALP clock");
      return -1;
   }

   usleep(65u);
   if (!sdio_function1_write_byte(dev, SDIO_PULLUP_CONTROL, 0u)
      || !sdio_backplane_write_u32(dev, chip.chipcommon + CYW43_GPI_PULLUP_OFFSET, 0u)
      || !sdio_backplane_write_u32(dev, chip.chipcommon + CYW43_GPI_PULLDOWN_OFFSET, 0u)) {
      sdio_runtime_set_error("Failed to configure CYW43 pull registers");
      return -1;
   }

   if (!sdio_backplane_write_bytes(dev, chip.rambase + chip.socramsize - sizeof(zero_tail),
                                   zero_tail, sizeof(zero_tail))) {
      sdio_runtime_set_error("Failed to clear CYW43 NVRAM tail");
      return -1;
   }

   chip.reset_vector = sdio_load_u32_le(g_cyw43_firmware_data);
   for (firmware_offset = 0u; firmware_offset < g_cyw43_firmware_length; firmware_offset += SDIO_BACKPLANE_TRANSFER_MAX) {
      uint32_t chunk = g_cyw43_firmware_length - firmware_offset;

      if (chunk > SDIO_BACKPLANE_TRANSFER_MAX)
         chunk = SDIO_BACKPLANE_TRANSFER_MAX;

      if (!sdio_backplane_write_bytes(dev, chip.rambase + firmware_offset,
                                      &g_cyw43_firmware_data[firmware_offset], chunk)) {
         sdio_runtime_set_error("Failed to upload CYW43 firmware image");
         return -1;
      }
   }

   condensed_nvram = malloc(g_cyw43_nvram_length + 4u);
   if (condensed_nvram == NULL) {
      sdio_runtime_set_error("Failed to allocate CYW43 NVRAM buffer");
      return -1;
   }

   memcpy(condensed_nvram, g_cyw43_nvram_data, g_cyw43_nvram_length);
   nvram_length = sdio_cyw43_condense_nvram(condensed_nvram, g_cyw43_nvram_length);
   if (nvram_length == 0u || nvram_length + 4u >= chip.socramsize) {
      free(condensed_nvram);
      sdio_runtime_set_error("CYW43 NVRAM image is invalid");
      return -1;
   }

   if (!sdio_backplane_write_bytes(dev, chip.rambase + chip.socramsize - nvram_length - 4u,
                                   condensed_nvram, nvram_length)) {
      free(condensed_nvram);
      sdio_runtime_set_error("Failed to upload CYW43 NVRAM image");
      return -1;
   }

   free(condensed_nvram);
   nvram_token = (nvram_length / 4u) & 0xffffu;
   nvram_token |= (~nvram_token << 16);
   sdio_store_u32_le(token_buffer, nvram_token);
   if (!sdio_backplane_write_bytes(dev, chip.rambase + chip.socramsize - 4u,
                                   token_buffer, sizeof(token_buffer))) {
      sdio_runtime_set_error("Failed to write CYW43 NVRAM token");
      return -1;
   }

   if (chip.arm_core == CYW43_CORE_ARM_CR4_ID) {
      if (!sdio_backplane_write_u32(dev, chip.sdregs + SDIO_CORE_INT_STATUS_OFFSET, ~0u)) {
         sdio_runtime_set_error("Failed to clear CYW43 SDIO interrupts");
         return -1;
      }

      if (chip.reset_vector != 0u) {
         sdio_store_u32_le(token_buffer, chip.reset_vector);
         if (!sdio_backplane_write_bytes(dev, 0u, token_buffer, sizeof(token_buffer))) {
            sdio_runtime_set_error("Failed to write CYW43 reset vector");
            return -1;
         }
      }

      if (!sdio_backplane_reset_core(dev, chip.armctl, CYW43_CR4_CPUHALT, 0u)) {
         sdio_runtime_set_error("Failed to start CYW43 CR4 core");
         return -1;
      }
   } else if (!sdio_backplane_reset_core(dev, chip.armctl, 0u, 0u)) {
      sdio_runtime_set_error("Failed to start CYW43 ARM core");
      return -1;
   }

   if (!sdio_function1_write_byte(dev, SDIO_CHIP_CLOCK_CSR, 0u)) {
      sdio_runtime_set_error("Failed to clear CYW43 HT request");
      return -1;
   }

   g_runtime_boot_chip = chip;
   g_runtime_boot_chip_id_register = chip_id_register;
   g_runtime_boot_fw_prepared = true;
   g_runtime_boot_stage = SDIO_RUNTIME_BOOT_STAGE_WAIT_HT_REQUEST;
   g_runtime_boot_wait_attempt = 0u;
   g_runtime_boot_deadline_us = now_us + 1000u;
   return 0;
   return sdio_runtime_finalize_boot_stage(dev, 0u, &chip, now_us);
}

static bool sdio_tx_probe_is_join_command(wifi_sdio_tx_probe_command_t command)
{
   return command == WIFI_SDIO_TX_PROBE_COMMAND_JOIN;
}

static wifi_sdio_tx_probe_command_t sdio_tx_probe_template_command(wifi_sdio_tx_probe_command_t command)
{
   if (sdio_tx_probe_is_join_command(command))
      return WIFI_SDIO_TX_PROBE_COMMAND_JOIN;

   return command;
}

static uint8_t sdio_tx_probe_join_commands(wifi_sdio_tx_probe_command_t *commands,
                                           size_t command_capacity)
{
   const wifi_config_t *config = wifi_get_config();
   uint8_t count = 0u;

   if (commands == NULL || command_capacity < TX_CONTROL_PROBE_JOIN_COMMAND_COUNT - 1u)
      return 0u;

   /* COUNTRY iovar dropped: confirmed BCME_BADARG (-2) on this BCM43430
      firmware build for both 'XX'/-1 and 'GB'/0.  The chip's regulatory
      defaults are clearly fine - the chanspec readback in the heartbeat
      shows it on channel 1, 20 MHz, 2.4 GHz - so the iovar isn't
      necessary.  The case handler is still present in
      sdio_prepare_tx_control_payload for one-off use. */
   /* Force a clean DOWN state before any parameter setup.  Some
      firmware builds latch BSS-level params at the moment of UP and
      ignore subsequent changes; a fresh DOWN/UP cycle ensures every
      iovar we set below is committed at the next UP. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_DOWN;
   /* Order matches cyw43-driver / brcmfmac: WSEC and wsec_pmk MUST be
      committed BEFORE bsscfg:sup_wpa enables the in-firmware
      supplicant.  Turning the supplicant on with stale (zero)
      credentials is the prime suspect for "SET_SSID acked but the
      join state machine never starts and the chip stays silent
      forever" on this BCM43430 firmware build.

      INFRA is also moved ahead of UP: some BCM43430 builds latch the
      BSS type at the moment of UP and ignore later changes, so we
      now do DOWN -> set BSS-level params -> UP. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_INFRA;            /* set STA mode while DOWN */
   /* mpc=0 keeps the radio awake between UP and SET_SSID.  Default
      mpc=1 lets the firmware power-down the radio whenever it's
      idle, which causes scans/joins to fail silently because the
      radio is asleep when the join state machine tries to transmit. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_MPC_OFF;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_TXGLOM_OFF;       /* Circle: wlsetint("bus:txglom",0) - bus param, fine while DOWN */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_UP;               /* WLC_UP - radio actually comes up here */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_POWERSAVE_OFF;    /* Circle: wlcmdint(0x56,0) - PM=0 after UP */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_WSEC;             /* WSEC=AES BEFORE the supplicant */
   if (config != NULL && config->password[0] != '\0')
      commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_PMK;           /* wsec_pmk BEFORE the supplicant */
   /* WIFI_SDIO_TX_PROBE_COMMAND_MFP intentionally omitted: the BCM43430
      firmware (Pi Zero W silicon) doesn't implement Management Frame
      Protection and replies to the "mfp" iovar with BCME_UNSUPPORTED
      (-23). The error is observable in the cdc-rsp log but the worse
      side effect is that it leaves the bsscfg in a state where the
      subsequent JOIN ioctl is acked successfully yet never produces
      WLC_E_LINK / WLC_E_SET_SSID events - which is exactly the silent
      "join accepted but no association" symptom we were chasing. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA2_EAPVER;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA_TMO;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_AUTH;             /* AUTH=0 (802.11 OPEN) */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH;         /* WPA_AUTH=WPA2_AUTH_PSK */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS;
   /* Pair the per-bsscfg mask with the global "event_msgs" mask.
      Some BCM43430 firmware builds AND the two together, so without
      this the global default of all-zero gates out every event the
      per-bsscfg mask allows.  brcmfmac sets the global mask only;
      cyw43-driver sets the per-bsscfg one only - this driver tries
      both. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_GLOBAL_EVENT_MSGS;
   /* Third event-mask form: the cyw43-driver-style "event_msgs_ext"
      iovar.  Some firmware builds only honor this _ext variant for
      events to actually fire; the simpler iovars above can return
      status=0 yet have no effect. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_EXT;
   /* Read the per-bsscfg event mask back so we can prove the SET above
      actually took effect. If the response payload's 16-byte mask is
      all-zero the iovar was a silent no-op - which is the prime suspect
      for the "join accepted, chip silent" symptom we keep chasing. The
      log line is "cdc rsp cmd=262 ..." with the response bytes appended. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_VERIFY;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_ROAM_OFF;       /* Circle: wlsetint("roam_off",1) */
   /* Use WLC_SET_SSID with the full wifi_join_params_t payload as the
      trigger to actually start scanning and associating. The "join"
      iovar (WLC_SET_VAR with name="join") is recognised by the
      BCM43430 firmware's dispatcher and acked with status=0, but it
      does not actually start the join state machine on this build -
      the chip stays silent and no WLC_E_SET_SSID / WLC_E_LINK is
      ever generated. WLC_SET_SSID is the older, universally
      supported path used by brcmfmac and every Cypress production
      driver. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_SSID;
   /* Read the SSID back so the cdc-rsp logger's hex dump shows what
      the chip actually has cached.  If it echoes our SSID, the SET
      took effect at the variable level and the silent failure is
      somewhere in the join state machine.  If it returns zeros, the
      SET was silently dropped and that's where the silence comes from. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_GET_SSID;
   /* WLC_SCAN dropped: confirmed BCME_NOTUP (-4) when issued after
      SET_SSID on this firmware - the chip considers itself busy with
      the just-started join attempt and rejects the scan.  The case
      handler stays around for one-off use during further bring-up. */

   return count;
}

static uint32_t sdio_tx_probe_command_value(wifi_sdio_tx_probe_command_t command)
{
   switch (command) {
      case WIFI_SDIO_TX_PROBE_COMMAND_JOIN:
         return WLC_SET_VAR;
      case WIFI_SDIO_TX_PROBE_COMMAND_POWERSAVE_OFF:
         return WLC_SET_PM;
      case WIFI_SDIO_TX_PROBE_COMMAND_TXGLOM_OFF:
      case WIFI_SDIO_TX_PROBE_COMMAND_ROAM_OFF:
      case WIFI_SDIO_TX_PROBE_COMMAND_COUNTRY:
      case WIFI_SDIO_TX_PROBE_COMMAND_MPC_OFF:
         return WLC_SET_VAR;
      case WIFI_SDIO_TX_PROBE_COMMAND_PMK:
         return WLC_SET_WSEC_PMK;
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA:
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA2_EAPVER:
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA_TMO:
      case WIFI_SDIO_TX_PROBE_COMMAND_MFP:
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS:
      case WIFI_SDIO_TX_PROBE_COMMAND_GLOBAL_EVENT_MSGS:
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_EXT:
         return WLC_SET_VAR;
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_VERIFY:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_CHANSPEC:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_MAC:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_SUP_WPA:
         return WLC_GET_VAR;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_BSSID:
         return WLC_GET_BSSID;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WSEC:
         return WLC_GET_WSEC;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WPA_AUTH:
         return WLC_GET_WPA_AUTH;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_AUTH:
         return WLC_GET_AUTH;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_INFRA:
         return WLC_GET_INFRA;
      case WIFI_SDIO_TX_PROBE_COMMAND_SCAN:
         return WLC_SCAN;
      case WIFI_SDIO_TX_PROBE_COMMAND_WSEC:
         return WLC_SET_WSEC;
      case WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH:
         return WLC_SET_WPA_AUTH;
      case WIFI_SDIO_TX_PROBE_COMMAND_SSID:
         return WLC_SET_SSID;
      case WIFI_SDIO_TX_PROBE_COMMAND_AUTH:
         return WLC_SET_AUTH;
      case WIFI_SDIO_TX_PROBE_COMMAND_INFRA:
         return WLC_SET_INFRA;
      case WIFI_SDIO_TX_PROBE_COMMAND_UP:
         return WLC_UP;
      case WIFI_SDIO_TX_PROBE_COMMAND_DOWN:
         return WLC_DOWN;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_SSID:
         return WLC_GET_SSID;
      case WIFI_SDIO_TX_PROBE_COMMAND_MAGIC:
         return WLC_GET_MAGIC;
      case WIFI_SDIO_TX_PROBE_COMMAND_VERSION:
      default:
         return WLC_GET_VERSION;
   }
}

static uint16_t sdio_tx_probe_payload_length(wifi_sdio_tx_probe_command_t command)
{
   switch (command) {
      case WIFI_SDIO_TX_PROBE_COMMAND_JOIN:
         return TX_CONTROL_TEMPLATE_JOIN_PAYLOAD_LENGTH;
      case WIFI_SDIO_TX_PROBE_COMMAND_PMK:
         return TX_CONTROL_TEMPLATE_MAX_PAYLOAD_LENGTH;
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA:
         return (uint16_t)(sizeof("bsscfg:sup_wpa") + 8u);
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA2_EAPVER:
         return (uint16_t)(sizeof("bsscfg:sup_wpa2_eapver") + 8u);
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA_TMO:
         return (uint16_t)(sizeof("bsscfg:sup_wpa_tmo") + 8u);
      case WIFI_SDIO_TX_PROBE_COMMAND_MFP:
         return (uint16_t)(sizeof("mfp") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS:
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_VERIFY:
         /* SET payload: "bsscfg:event_msgs\0" + bsscfg_idx (4) + mask (16).
            GET request payload: "bsscfg:event_msgs\0" + bsscfg_idx (4),
            but we send the full 38 bytes anyway so the firmware has a
            buffer it can write the 16-byte response mask back into. */
         return (uint16_t)(sizeof("bsscfg:event_msgs") + 4u + 16u);
      case WIFI_SDIO_TX_PROBE_COMMAND_GLOBAL_EVENT_MSGS:
         /* "event_msgs\0" + 16-byte mask.  No bsscfg_idx for the
            global form. */
         return (uint16_t)(sizeof("event_msgs") + 16u);
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_EXT:
         /* "bsscfg:event_msgs_ext\0" + bsscfg_idx (4) + version (1) +
            cmd (1) + length (2) + mask (16). */
         return (uint16_t)(sizeof("bsscfg:event_msgs_ext") + 4u + 1u + 1u + 2u + 16u);
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_SSID:
         /* WLC_GET_SSID returns a wlc_ssid_t (4-byte length + 32-byte
            SSID = 36 bytes).  Send a 36-byte zero buffer for the chip
            to fill. */
         return 36u;
      case WIFI_SDIO_TX_PROBE_COMMAND_DOWN:
         /* WLC_DOWN takes no payload; send a single zero word. */
         return 4u;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_BSSID:
         /* WLC_GET_BSSID returns a 6-byte BSSID.  Send a 6-byte zero
            buffer for the chip to fill. */
         return 6u;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_CHANSPEC:
         /* "chanspec\0" + 4 zero bytes for the chip to write the
            current chanspec into.  GET-VAR convention. */
         return (uint16_t)(sizeof("chanspec") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_MAC:
         /* "cur_etheraddr\0" + 6 zero bytes for the chip to write the
            current MAC into. */
         return (uint16_t)(sizeof("cur_etheraddr") + 6u);
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_SUP_WPA:
         /* "bsscfg:sup_wpa\0" + bsscfg_idx (4) + 4-byte response slot.
            Per-bsscfg iovar - same shape as bsscfg:event_msgs but with
            a 4-byte u32 value instead of a 16-byte mask. */
         return (uint16_t)(sizeof("bsscfg:sup_wpa") + 4u + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WSEC:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WPA_AUTH:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_AUTH:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_INFRA:
         /* WLC_GET_* ioctls return a u32; send a 4-byte zero buffer
            for the chip to fill. */
         return 4u;
      case WIFI_SDIO_TX_PROBE_COMMAND_SCAN:
         /* wl_scan_params (packed):
              wlc_ssid_t  ssid     36   (length=0 => broadcast scan)
              ether_addr  bssid     6   (FF:FF:FF:FF:FF:FF = any)
              int8_t      bss_type  1   (2 = ANY)
              int8_t      scan_type 1   (0 = active)
              int32_t     nprobes   4   (-1 = default)
              int32_t     active_t  4   (-1)
              int32_t     passive_t 4   (-1)
              int32_t     home_t    4   (-1)
              int32_t     channel_n 4   (0 = scan all channels of the
                                          configured regulatory domain)
              chanspec_t  chans[1]  2   (unused when channel_num=0)
            Total 66 bytes packed. */
         return 66u;
      case WIFI_SDIO_TX_PROBE_COMMAND_SSID:
         /* wlc_ssid_t (36 bytes): length(LE u32) + SSID bytes (up to 32).
            cyw43-driver shape - exactly what WLC_SET_SSID expects.
            We previously sent the 50-byte wl_join_params_le shape
            (wlc_ssid + assoc_params with broadcast bssid + chanspec_num=0)
            on the theory the chip would interpret the trailing scan
            params; on this BCM43430 build the firmware silently
            ignored them, so the extra bytes were doing nothing.
            The right way to add scan/channel hints is the dedicated
            "join" iovar (WIFI_SDIO_TX_PROBE_COMMAND_JOIN below). */
         return 36u;
      case WIFI_SDIO_TX_PROBE_COMMAND_WSEC:
      case WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH:
      case WIFI_SDIO_TX_PROBE_COMMAND_AUTH:
      case WIFI_SDIO_TX_PROBE_COMMAND_INFRA:
      case WIFI_SDIO_TX_PROBE_COMMAND_POWERSAVE_OFF:
         return 4u;
      case WIFI_SDIO_TX_PROBE_COMMAND_TXGLOM_OFF:
         return (uint16_t)(sizeof("bus:txglom") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_ROAM_OFF:
         return (uint16_t)(sizeof("roam_off") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_MPC_OFF:
         return (uint16_t)(sizeof("mpc") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_COUNTRY:
         /* "country\0" + wl_country_t (12 bytes: country_abbrev[4] +
            int32_t rev + ccode[4]) */
         return (uint16_t)(sizeof("country") + 12u);
      case WIFI_SDIO_TX_PROBE_COMMAND_UP:
         return 0u;
      case WIFI_SDIO_TX_PROBE_COMMAND_MAGIC:
      case WIFI_SDIO_TX_PROBE_COMMAND_VERSION:
      default:
         return 4u;
   }
}

static bool sdio_tx_probe_is_set_ioctl(wifi_sdio_tx_probe_command_t command)
{
   return command == WIFI_SDIO_TX_PROBE_COMMAND_UP
         || command == WIFI_SDIO_TX_PROBE_COMMAND_INFRA
         || command == WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA
         || command == WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA2_EAPVER
         || command == WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA_TMO
         || command == WIFI_SDIO_TX_PROBE_COMMAND_AUTH
         || command == WIFI_SDIO_TX_PROBE_COMMAND_MFP
         || command == WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS
         || command == WIFI_SDIO_TX_PROBE_COMMAND_GLOBAL_EVENT_MSGS
         || command == WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_EXT
         || command == WIFI_SDIO_TX_PROBE_COMMAND_DOWN
         || command == WIFI_SDIO_TX_PROBE_COMMAND_JOIN
         || command == WIFI_SDIO_TX_PROBE_COMMAND_SSID
         || command == WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH
         || command == WIFI_SDIO_TX_PROBE_COMMAND_WSEC
         || command == WIFI_SDIO_TX_PROBE_COMMAND_PMK
         || command == WIFI_SDIO_TX_PROBE_COMMAND_POWERSAVE_OFF
         || command == WIFI_SDIO_TX_PROBE_COMMAND_TXGLOM_OFF
         || command == WIFI_SDIO_TX_PROBE_COMMAND_ROAM_OFF
         || command == WIFI_SDIO_TX_PROBE_COMMAND_COUNTRY
         || command == WIFI_SDIO_TX_PROBE_COMMAND_SCAN
         || command == WIFI_SDIO_TX_PROBE_COMMAND_MPC_OFF;
}

static uint32_t sdio_load_u32_le(const uint8_t *src)
{
   if (src == NULL)
      return 0u;

   return (uint32_t)src[0]
      | ((uint32_t)src[1] << 8)
      | ((uint32_t)src[2] << 16)
      | ((uint32_t)src[3] << 24);
}

static uint32_t sdio_tx_probe_payload_word0(wifi_sdio_tx_probe_command_t command)
{
   switch (command) {
      case WIFI_SDIO_TX_PROBE_COMMAND_INFRA:
         return 1u;
      case WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH:
      {
         const wifi_config_t *config = wifi_get_config();

         return (config != NULL && config->password[0] != '\0') ? WPA2_AUTH_PSK : WPA_AUTH_DISABLED;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_WSEC:
      {
         const wifi_config_t *config = wifi_get_config();

         return (config != NULL && config->password[0] != '\0') ? AES_ENABLED : 0u;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA:
      {
         const wifi_config_t *config = wifi_get_config();

         return (config != NULL && config->password[0] != '\0') ? 1u : 0u;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_MFP:
      {
         const wifi_config_t *config = wifi_get_config();

         return (config != NULL && config->password[0] != '\0') ? MFP_CAPABLE : MFP_NONE;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_PMK:
      {
         const wifi_config_t *config = wifi_get_config();
         uint32_t password_length = 0u;

         if (config != NULL)
            password_length = (uint32_t)strnlen(config->password, WSEC_MAX_PSK_LEN);

         return password_length | ((uint32_t)WSEC_PASSPHRASE << 16);
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_AUTH:
      case WIFI_SDIO_TX_PROBE_COMMAND_MAGIC:
      case WIFI_SDIO_TX_PROBE_COMMAND_VERSION:
      case WIFI_SDIO_TX_PROBE_COMMAND_UP:
      case WIFI_SDIO_TX_PROBE_COMMAND_SSID:
      case WIFI_SDIO_TX_PROBE_COMMAND_JOIN:
      default:
         return 0u;
   }
}

static void sdio_prepare_tx_control_iovar_u32_payload(sdio_probe_result_t *probe_result,
                                                      const char *name,
                                                      uint32_t value)
{
   size_t name_length;

   if (probe_result == NULL || name == NULL)
      return;

   name_length = strlen(name) + 1u;
   memcpy(probe_result->tx_control_template_payload_bytes, name, name_length);
   sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length], value);
}

static void sdio_prepare_tx_control_iovar_u32_u32_payload(sdio_probe_result_t *probe_result,
                                                          const char *name,
                                                          uint32_t value0,
                                                          uint32_t value1)
{
   size_t name_length;

   if (probe_result == NULL || name == NULL)
      return;

   name_length = strlen(name) + 1u;
   memcpy(probe_result->tx_control_template_payload_bytes, name, name_length);
   sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length], value0);
   sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length + 4u], value1);
}

static void sdio_prepare_tx_control_event_msgs_payload(sdio_probe_result_t *probe_result)
{
   static const uint8_t disabled_events[] = { 20u, 40u, 44u, 54u, 71u, 124u };
   uint8_t *value;
   uint8_t *event_mask;
   size_t index;
   size_t name_length;

   if (probe_result == NULL)
      return;

   /* On the BCM43430 firmware build the global "event_msgs" iovar is
      accepted but events for the primary bsscfg stay gated by a
      separate per-bsscfg mask that defaults to all-zero. We've already
      seen this pattern with the WPA supplicant ("bsscfg:sup_wpa" etc.);
      "bsscfg:event_msgs" is the matching per-bsscfg event mask iovar.
      Without it the chip silently accepts every ioctl and joins, but
      never tells us about WLC_E_SET_SSID, WLC_E_AUTH or WLC_E_LINK -
      which is exactly the silence we've been chasing.

      Layout (cyfitter / brcmfmac convention):
        "bsscfg:event_msgs\0"   (18 bytes)
        bsscfg_idx (uint32 LE)  (4 bytes)  - 0 = primary
        event_mask (16 bytes)              - all 1s except disabled_events
   */
   name_length = sizeof("bsscfg:event_msgs");
   memcpy(probe_result->tx_control_template_payload_bytes,
          "bsscfg:event_msgs", name_length);
   value = &probe_result->tx_control_template_payload_bytes[name_length];

   /* bsscfg_idx = 0 (primary) */
   sdio_store_u32_le(value, 0u);

   event_mask = &value[4];
   memset(event_mask, 0xffu, 16u);

   for (index = 0u; index < sizeof(disabled_events) / sizeof(disabled_events[0]); ++index)
      event_mask[disabled_events[index] / 8u] &= (uint8_t)~(1u << (disabled_events[index] % 8u));
}

static void sdio_prepare_tx_control_join_payload(sdio_probe_result_t *probe_result)
{
   const wifi_config_t *config;
   size_t name_length;
   size_t ssid_length;

   if (probe_result == NULL)
      return;

   config = wifi_get_config();
   name_length = sizeof("join");
   ssid_length = 0u;
   if (config != NULL)
      ssid_length = strnlen(config->ssid, WLC_SSID_MAX_LEN);

   memcpy(probe_result->tx_control_template_payload_bytes, "join", name_length);
   sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length], (uint32_t)ssid_length);
   if (config != NULL && ssid_length > 0u)
      memcpy(&probe_result->tx_control_template_payload_bytes[name_length + 4u], config->ssid, ssid_length);

   /* Scan parameters (wl_join_scan_params): scan_type(1)+pad(3)+nprobes(4)+
      active_time(4)+passive_time(4)+home_time(4) = 20 bytes at payload[36].
      scan_type = 0xFF (-1) = use firmware default scan type (Circle: put4(p, 0xff)).
      All times = -1 means "use firmware defaults". */
   probe_result->tx_control_template_payload_bytes[name_length + 36u] = 0xFFu; /* scan_type */
   sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length + 40u], 0xffffffffu);
   sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length + 44u], 0xffffffffu);
   sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length + 48u], 0xffffffffu);
   sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length + 52u], 0xffffffffu);
   memset(&probe_result->tx_control_template_payload_bytes[name_length + 56u], 0xffu, 6u);
   sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length + 64u], 0u);
}

static void sdio_prepare_tx_control_payload(sdio_probe_result_t *probe_result,
                                            wifi_sdio_tx_probe_command_t command)
{
   uint32_t payload_word0;

   if (probe_result == NULL)
      return;

   memset(probe_result->tx_control_template_payload_bytes, 0,
          sizeof(probe_result->tx_control_template_payload_bytes));
   payload_word0 = sdio_tx_probe_payload_word0(command);

   switch (command) {
      case WIFI_SDIO_TX_PROBE_COMMAND_JOIN:
         sdio_prepare_tx_control_join_payload(probe_result);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_SSID:
      {
         const wifi_config_t *config = wifi_get_config();
         size_t ssid_length = 0u;
         uint8_t *p = probe_result->tx_control_template_payload_bytes;

         if (config != NULL)
            ssid_length = strnlen(config->ssid, WLC_SSID_MAX_LEN);

         /* Log the SSID we're about to send so a follow-up GET_SSID
            readback of all-zero is unambiguous: it means the chip
            silently dropped a real SSID, NOT that we sent nothing.
            ssid_length=0 here means the cmdline didn't supply a
            wifi_ssid= property and there's no point even rebooting. */
         sdio_debug_log("SET_SSID prep len=%u ssid=\"%s\"",
                        (unsigned)ssid_length,
                        (config != NULL && ssid_length > 0u) ? config->ssid : "(empty)");

         /* wlc_ssid_t (36 bytes) - cyw43-driver shape.
              [0..3]   length (LE u32)
              [4..35]  SSID bytes (zero-padded). */
         sdio_store_u32_le(&p[0], (uint32_t)ssid_length);
         if (config != NULL && ssid_length > 0u)
            memcpy(&p[4], config->ssid, ssid_length);
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_PMK:
      {
         const wifi_config_t *config = wifi_get_config();
         size_t password_length = 0u;

         if (config != NULL)
            password_length = strnlen(config->password, WSEC_MAX_PSK_LEN);

         probe_result->tx_control_template_payload_bytes[0] = (uint8_t)(password_length & 0xffu);
         probe_result->tx_control_template_payload_bytes[1] = (uint8_t)((password_length >> 8) & 0xffu);
         probe_result->tx_control_template_payload_bytes[2] = (uint8_t)(WSEC_PASSPHRASE & 0xffu);
         probe_result->tx_control_template_payload_bytes[3] = (uint8_t)((WSEC_PASSPHRASE >> 8) & 0xffu);
         if (config != NULL && password_length > 0u)
            memcpy(&probe_result->tx_control_template_payload_bytes[4], config->password, password_length);
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA:
         sdio_prepare_tx_control_iovar_u32_u32_payload(probe_result,
                                                       "bsscfg:sup_wpa",
                                                       0u,
                                                       payload_word0);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA2_EAPVER:
         sdio_prepare_tx_control_iovar_u32_u32_payload(probe_result,
                                                       "bsscfg:sup_wpa2_eapver",
                                                       0u,
                                                       0xffffffffu);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA_TMO:
         sdio_prepare_tx_control_iovar_u32_u32_payload(probe_result,
                                                       "bsscfg:sup_wpa_tmo",
                                                       0u,
                                                       CYW_EAPOL_KEY_TIMEOUT);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_MFP:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "mfp", payload_word0);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS:
         sdio_prepare_tx_control_event_msgs_payload(probe_result);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_GLOBAL_EVENT_MSGS:
      {
         /* Global event mask - same 16-byte mask as bsscfg:event_msgs
            but with the iovar name "event_msgs" (no prefix, no
            bsscfg_idx).  Some BCM43430 firmware builds AND the global
            mask with the per-bsscfg one, so leaving it at its all-zero
            default silently drops every event the bsscfg form lets
            through. */
         static const uint8_t disabled_events[] = { 20u, 40u, 44u, 54u, 71u, 124u };
         uint8_t *event_mask;
         size_t name_length = sizeof("event_msgs");
         size_t i;

         memcpy(probe_result->tx_control_template_payload_bytes,
                "event_msgs", name_length);
         event_mask = &probe_result->tx_control_template_payload_bytes[name_length];
         memset(event_mask, 0xffu, 16u);
         for (i = 0u; i < sizeof(disabled_events) / sizeof(disabled_events[0]); ++i)
            event_mask[disabled_events[i] / 8u] &= (uint8_t)~(1u << (disabled_events[i] % 8u));
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_EXT:
      {
         /* Extended event-mask iovar with cyw43-driver framing:
              [0..21]   "bsscfg:event_msgs_ext\0"  (22 bytes)
              [22..25]  bsscfg_idx (LE u32)        (4 bytes)
              [26]      version = 1                (EVENTMSGS_VER)
              [27]      cmd = 1                    (1 = SET, 0 = GET)
              [28..29]  length = 16 (LE u16)       (mask byte length)
              [30..45]  mask                       (16 bytes)
            Total 46 bytes.  Some firmware builds only act on this
            extended form even though the simpler iovars return
            status=0. */
         static const uint8_t disabled_events[] = { 20u, 40u, 44u, 54u, 71u, 124u };
         uint8_t *p = probe_result->tx_control_template_payload_bytes;
         uint8_t *value;
         uint8_t *event_mask;
         size_t name_length = sizeof("bsscfg:event_msgs_ext");
         size_t i;

         memcpy(p, "bsscfg:event_msgs_ext", name_length);
         value = &p[name_length];
         /* bsscfg_idx = 0 */
         sdio_store_u32_le(&value[0], 0u);
         /* version, cmd, length(LE u16) */
         value[4] = 1u;          /* version = EVENTMSGS_VER */
         value[5] = 1u;          /* cmd = SET */
         sdio_store_u16_le(&value[6], 16u);
         /* mask */
         event_mask = &value[8];
         memset(event_mask, 0xffu, 16u);
         for (i = 0u; i < sizeof(disabled_events) / sizeof(disabled_events[0]); ++i)
            event_mask[disabled_events[i] / 8u] &= (uint8_t)~(1u << (disabled_events[i] % 8u));
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_SSID:
         /* 36-byte zero buffer.  Chip writes wlc_ssid_t (length + ssid
            bytes) in its CDC response - the cdc-rsp logger's GET-VAR
            hex dump path is keyed on cmd == WLC_GET_VAR, so we tag a
            secondary log line of our own from the existing logger
            (kept here as a no-op; the response will still be visible
            via the regular cdc-rsp line and the chip's reply payload
            will follow on an "fn2: drop" or zero-length CDC, which
            we'll eyeball). */
         /* Buffer is already zeroed by the memset at function start. */
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_VERIFY:
      {
         /* GET-VAR readback: send "bsscfg:event_msgs\0" + bsscfg_idx,
            and leave the trailing 16 bytes zero so the firmware has
            space to write the current per-bsscfg event mask in its
            CDC response. The cdc-rsp logger dumps those bytes when
            cmd == WLC_GET_VAR (262). */
         size_t name_length = sizeof("bsscfg:event_msgs");

         memcpy(probe_result->tx_control_template_payload_bytes,
                "bsscfg:event_msgs", name_length);
         /* bsscfg_idx = 0 (primary) */
         sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length], 0u);
         /* Trailing 16 bytes already zeroed by the memset at the top
            of this function - that's where the chip writes the mask. */
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_BSSID:
         /* 6 zero bytes - chip writes the current BSSID here in its
            CDC response.  All-zero response means "not associated". */
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_CHANSPEC:
      {
         /* GET-VAR "chanspec" - returns the chip's current chanspec
            (4-byte LE).  All-zero response on this firmware build
            means "no channel selected" (chip never picked one). */
         size_t name_length = sizeof("chanspec");

         memcpy(probe_result->tx_control_template_payload_bytes,
                "chanspec", name_length);
         /* Trailing 4 bytes already zeroed by the memset at the top
            of this function - that's where the chip writes the value. */
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_MAC:
      {
         /* GET-VAR "cur_etheraddr" - returns the chip's current MAC
            (6 bytes).  All-zero response means NVRAM didn't apply
            during firmware boot, which would also explain a silent
            radio (PA/antenna calibration lives in NVRAM). */
         size_t name_length = sizeof("cur_etheraddr");

         memcpy(probe_result->tx_control_template_payload_bytes,
                "cur_etheraddr", name_length);
         /* Trailing 6 bytes already zeroed by the memset at top. */
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_SUP_WPA:
      {
         /* GET-VAR "bsscfg:sup_wpa\0" + bsscfg_idx(4) + 4-byte response slot.
            Chip writes the current bsscfg:sup_wpa u32 value (1=enabled,
            0=disabled) into the trailing 4 bytes.  All-zero readback
            means our SET to enable the in-firmware supplicant was
            silently dropped. */
         size_t name_length = sizeof("bsscfg:sup_wpa");

         memcpy(probe_result->tx_control_template_payload_bytes,
                "bsscfg:sup_wpa", name_length);
         /* bsscfg_idx = 0 (primary) */
         sdio_store_u32_le(&probe_result->tx_control_template_payload_bytes[name_length], 0u);
         /* Trailing 4 bytes already zeroed by the memset at top. */
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WSEC:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WPA_AUTH:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_AUTH:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_INFRA:
         /* WLC_GET_* ioctls write a u32 into the 4-byte response slot.
            Buffer already zeroed by the memset at the top of this
            function. */
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_SCAN:
      {
         /* wl_scan_params - "scan all channels, any AP, active probe".
            Diagnostic only: if WLC_E_ESCAN_RESULT events fire after
            this we know the radio works and the post-SET_SSID silence
            is specific to the join state machine. */
         uint8_t *p = probe_result->tx_control_template_payload_bytes;

         /* wlc_ssid_t at [0..35]: length=0 => broadcast scan.
            All bytes already zero from the memset at function start. */

         /* bssid at [36..41] = FF:FF:FF:FF:FF:FF (any AP) */
         memset(&p[36], 0xffu, 6u);

         /* bss_type at [42] = 2 (DOT11_BSSTYPE_ANY) */
         p[42] = 2u;
         /* scan_type at [43] = 0 (active) */
         p[43] = 0u;

         /* nprobes / active_time / passive_time / home_time = -1
            => "use firmware default".  Stored LE at [44..59]. */
         sdio_store_u32_le(&p[44], 0xffffffffu);
         sdio_store_u32_le(&p[48], 0xffffffffu);
         sdio_store_u32_le(&p[52], 0xffffffffu);
         sdio_store_u32_le(&p[56], 0xffffffffu);

         /* channel_num at [60..63] = 0 => scan all channels.
            chanspec_list[1] at [64..65] = 0 (unused when num=0).
            Both already zeroed. */
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_TXGLOM_OFF:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "bus:txglom", 0u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_ROAM_OFF:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "roam_off", 1u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_MPC_OFF:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "mpc", 0u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_COUNTRY:
      {
         /* "country\0" + wl_country_t {
              int8_t country_abbrev[4];   // "GB\0\0"
              int32_t rev;                //  0   (use revision 0 - the
                                          //       previous code used -1
                                          //       which the BCM43430
                                          //       firmware on the Pi
                                          //       Zero W rejects with
                                          //       BCME_BADARG, leaving
                                          //       the regulatory subsystem
                                          //       in a state where the
                                          //       chip silently refuses
                                          //       to scan or transmit)
              int8_t ccode[4];            // "GB\0\0"
            }
            "GB" matches the United Kingdom regulatory domain.  This
            iovar must be set while the chip is DOWN, so it is the very
            first command in the join sequence (before WLC_UP). */
         size_t name_length = sizeof("country");
         uint8_t *value;

         memcpy(probe_result->tx_control_template_payload_bytes, "country", name_length);
         value = &probe_result->tx_control_template_payload_bytes[name_length];
         value[0] = 'G';
         value[1] = 'B';
         value[2] = 0u;
         value[3] = 0u;
         sdio_store_u32_le(&value[4], 0u); /* rev = 0 */
         value[8]  = 'G';
         value[9]  = 'B';
         value[10] = 0u;
         value[11] = 0u;
         break;
      }
      default:
         if (probe_result->tx_control_template_payload_length >= 4u)
            sdio_store_u32_le(probe_result->tx_control_template_payload_bytes, payload_word0);
         break;
   }

   probe_result->tx_control_template_payload_word0 =
      sdio_load_u32_le(probe_result->tx_control_template_payload_bytes);
}

static uint16_t sdio_next_tx_probe_request_id(void)
{
   uint16_t request_id = g_tx_control_probe_request_id;

   ++g_tx_control_probe_request_id;
   if (g_tx_control_probe_request_id == 0u)
      g_tx_control_probe_request_id = 1u;

   return request_id;
}

static uint8_t sdio_next_tx_probe_sequence(void)
{
   uint8_t sequence = g_tx_control_probe_sequence;

   ++g_tx_control_probe_sequence;
   return sequence;
}

static void sdio_fill_cmd52_result(sdio_cmd52_result_t *result,
                                   const sdio_host_result_t *host_result)
{
   if (result == NULL || host_result == NULL)
      return;

   result->success = host_result->success;
   result->response0 = host_result->response0;
   result->interrupt = host_result->interrupt;
   result->error = host_result->error;
   result->data = (uint8_t)(host_result->response0 & 0xffu);
}

static void sdio_fill_cmd53_result(sdio_cmd53_result_t *result,
                                   const sdio_host_result_t *host_result)
{
   if (result == NULL || host_result == NULL)
      return;

   result->success = host_result->success;
   result->response0 = host_result->response0;
   result->interrupt = host_result->interrupt;
   result->error = host_result->error;
}

static void sdio_prepare_tx_control_template(sdio_probe_result_t *probe_result,
                                             wifi_sdio_tx_probe_command_t command)
{
   uint32_t cdc_flags;
   uint16_t request_id;
   uint16_t payload_length;
   uint8_t sequence;

   if (probe_result == NULL)
      return;

   request_id = sdio_next_tx_probe_request_id();
   sequence = sdio_next_tx_probe_sequence();
   payload_length = sdio_tx_probe_payload_length(command);

   probe_result->tx_control_template_frame_size = (uint16_t)(SDPCM_CONTROL_EVENT_HEADER_LENGTH
      + CDC_HEADER_LENGTH + payload_length);
   probe_result->tx_control_template_frame_size_complement = (uint16_t)~probe_result->tx_control_template_frame_size;
   probe_result->tx_control_template_sequence = sequence;
   probe_result->tx_control_template_channel_and_flags = SDPCM_CONTROL_CHANNEL;
   probe_result->tx_control_template_next_length = 0u;
   probe_result->tx_control_template_header_length = SDPCM_CONTROL_EVENT_HEADER_LENGTH;
   probe_result->tx_control_template_wireless_flow_control = 0u;
   probe_result->tx_control_template_bus_data_credit = 0u;
   probe_result->tx_control_template_command = sdio_tx_probe_command_value(command);
   probe_result->tx_control_template_payload_length = payload_length;
   probe_result->tx_control_template_request_id = request_id;
   probe_result->tx_control_template_interface = TX_CONTROL_TEMPLATE_INTERFACE;
   probe_result->tx_control_template_cdc_length = (uint32_t)payload_length;
   cdc_flags = ((uint32_t)request_id << CDCF_IOC_ID_SHIFT)
      | ((uint32_t)TX_CONTROL_TEMPLATE_INTERFACE << CDCF_IOC_IF_SHIFT);
   if (sdio_tx_probe_is_set_ioctl(command))
      cdc_flags |= CDCF_IOC_SET;
   probe_result->tx_control_template_cdc_flags = cdc_flags;
   probe_result->tx_control_template_cdc_status = 0u;
   sdio_prepare_tx_control_payload(probe_result, command);
   probe_result->tx_control_template_ready = true;
}

static void sdio_store_u16_le(uint8_t *dest, uint16_t value)
{
   if (dest == NULL)
      return;

   dest[0] = (uint8_t)(value & 0xffu);
   dest[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void sdio_store_u32_le(uint8_t *dest, uint32_t value)
{
   if (dest == NULL)
      return;

   dest[0] = (uint8_t)(value & 0xffu);
   dest[1] = (uint8_t)((value >> 8) & 0xffu);
   dest[2] = (uint8_t)((value >> 16) & 0xffu);
   dest[3] = (uint8_t)((value >> 24) & 0xffu);
}

static bool sdio_function2_transfer_timeout(sdio_host_t *dev, bool write, uint8_t *buffer,
                                            uint16_t length, uint32_t timeout_us)
{
   uint16_t remaining = length;
   uint8_t *cursor = buffer;

   while (remaining > 0u) {
      uint16_t count = remaining;
      bool block_mode = false;
      uint32_t block_size = count;

      if (count > SDIO_PROBE_FUNCTION2_BLOCK_SIZE) {
         uint16_t block_count = (uint16_t)(count / SDIO_PROBE_FUNCTION2_BLOCK_SIZE);

         if (block_count > 511u)
            block_count = 511u;

         count = block_count;
         block_mode = true;
         block_size = SDIO_PROBE_FUNCTION2_BLOCK_SIZE;
         remaining = (uint16_t)(remaining - (uint16_t)(block_count * SDIO_PROBE_FUNCTION2_BLOCK_SIZE));
         if (!sdio_cmd53_execute_timeout(dev, 2u, 0u, write, true, false, count,
                                         cursor, block_size, timeout_us, NULL)) {
            return false;
         }
         cursor += block_count * SDIO_PROBE_FUNCTION2_BLOCK_SIZE;
         continue;
      }

      remaining = 0u;
      if (!sdio_cmd53_execute_timeout(dev, 2u, 0u, write, block_mode, false, count,
                                      cursor, block_size, timeout_us, NULL)) {
         return false;
      }
   }

   return true;
}

static bool sdio_function2_transfer(sdio_host_t *dev, bool write, uint8_t *buffer,
                                    uint16_t length)
{
   return sdio_function2_transfer_timeout(dev, write, buffer, length,
                                          SDIO_COMMAND_TIMEOUT_US);
}

static bool sdio_runtime_abort_function2_read(sdio_host_t *dev)
{
   return sdio_probe_write_byte(dev, SDIO_CCCR_IO_ABORT, SDIO_IO_ABORT_FUNCTION2)
      && sdio_function1_write_byte(dev, SDIO_FRAME_CONTROL, SDIO_FRAME_CONTROL_READ_TERMINATE);
}

/* Decode an event_msg that sits directly after the SDPCM header (i.e.
   no Ethernet header, no BRCM event header, no OUI). The BCM43430
   firmware build that ships with the Pi Zero W uses this older "bare"
   envelope on the SDPCM event channel: the only thing between the
   SDPCM payload start and event_msg is nothing at all. event_msg fields
   are big-endian on the wire. */
static void sdio_runtime_note_event_bare(const uint8_t *payload, uint16_t payload_length)
{
   uint32_t event_type;
   uint32_t event_status;
   uint32_t event_reason;

   if (payload == NULL || payload_length < BRCM_EVENT_MSG_LENGTH)
      return;

   /* event_msg layout (offsets relative to payload start, all big-endian):
        [0-1]   version
        [2-3]   flags
        [4-7]   event_type
        [8-11]  status
        [12-15] reason
        [16-19] auth_type
        [20-23] datalen
        ... */
   event_type = ((uint32_t)payload[4] << 24)
              | ((uint32_t)payload[5] << 16)
              | ((uint32_t)payload[6] << 8)
              | (uint32_t)payload[7];
   event_status = ((uint32_t)payload[8] << 24)
                | ((uint32_t)payload[9] << 16)
                | ((uint32_t)payload[10] << 8)
                | (uint32_t)payload[11];
   event_reason = ((uint32_t)payload[12] << 24)
                | ((uint32_t)payload[13] << 16)
                | ((uint32_t)payload[14] << 8)
                | (uint32_t)payload[15];

   /* Sanity check: real event_type values are < 256. If we picked the
      wrong envelope we'd see garbage here - bail rather than poison
      g_runtime_link_up. */
   if (event_type > 255u)
      return;

   g_sdio_probe_result.sdpcm_brcm_event_type = event_type;
   g_sdio_probe_result.sdpcm_brcm_event_status = event_status;
   g_sdio_probe_result.sdpcm_brcm_event_reason = event_reason;

   sdio_debug_log("event %s type=%lu status=%lu reason=%lu (bare)",
                  sdio_event_name(event_type),
                  (unsigned long)event_type,
                  (unsigned long)event_status,
                  (unsigned long)event_reason);

   if (sdio_event_is_link_up(event_type, event_status, event_reason))
      g_runtime_link_up = true;

   if (sdio_event_is_link_down(event_type))
      g_runtime_link_up = false;
}

static void sdio_runtime_note_event(const uint8_t *frame, uint16_t frame_length,
                                    uint16_t ethernet_offset)
{
   uint16_t event_offset;
   uint16_t message_offset;
   uint32_t event_type;
   uint32_t event_status;
   uint32_t event_reason;

   if (frame == NULL || frame_length < (uint16_t)(ethernet_offset + ETHERNET_HEADER_LENGTH + BRCM_EVENT_HEADER_LENGTH + BRCM_EVENT_MSG_LENGTH))
      return;

   event_offset = (uint16_t)(ethernet_offset + ETHERNET_HEADER_LENGTH);
   if (frame[event_offset + 5u] != BRCM_OUI0
      || frame[event_offset + 6u] != BRCM_OUI1
      || frame[event_offset + 7u] != BRCM_OUI2) {
      sdio_debug_log("ev: skip oui=%02x:%02x:%02x ver=%u",
                     (unsigned)frame[event_offset + 5u],
                     (unsigned)frame[event_offset + 6u],
                     (unsigned)frame[event_offset + 7u],
                     (unsigned)frame[event_offset + 4u]);
      return;
   }

   message_offset = (uint16_t)(event_offset + BRCM_EVENT_HEADER_LENGTH);
   event_type = ((uint32_t)frame[message_offset + 4u] << 24)
      | ((uint32_t)frame[message_offset + 5u] << 16)
      | ((uint32_t)frame[message_offset + 6u] << 8)
      | (uint32_t)frame[message_offset + 7u];
   event_status = ((uint32_t)frame[message_offset + 8u] << 24)
      | ((uint32_t)frame[message_offset + 9u] << 16)
      | ((uint32_t)frame[message_offset + 10u] << 8)
      | (uint32_t)frame[message_offset + 11u];
   event_reason = ((uint32_t)frame[message_offset + 12u] << 24)
      | ((uint32_t)frame[message_offset + 13u] << 16)
      | ((uint32_t)frame[message_offset + 14u] << 8)
      | (uint32_t)frame[message_offset + 15u];

   g_sdio_probe_result.sdpcm_brcm_event_type = event_type;
   g_sdio_probe_result.sdpcm_brcm_event_status = event_status;
   g_sdio_probe_result.sdpcm_brcm_event_reason = event_reason;

   sdio_debug_log("event %s type=%lu status=%lu reason=%lu",
                  sdio_event_name(event_type),
                  (unsigned long)event_type,
                  (unsigned long)event_status,
                  (unsigned long)event_reason);

   if (sdio_event_is_link_up(event_type, event_status, event_reason))
      g_runtime_link_up = true;

   if (sdio_event_is_link_down(event_type))
      g_runtime_link_up = false;
}

/* sdio_runtime_complete_read_ethernet_frame - process a frame whose 4-byte
   SDPCM hw-tag header has already been read into hwtag[].  Reads the rest of
   the frame from fn2, handles BRCM async events internally, and delivers
   Ethernet payload frames to the caller. */
static bool sdio_runtime_complete_read_ethernet_frame_timeout(sdio_host_t *dev,
                                                              const uint16_t hwtag[2],
                                                              uint8_t *frame,
                                                              uint16_t frame_capacity,
                                                              uint16_t *frame_length,
                                                              uint32_t timeout_us)
{
   uint8_t frame_buffer[SDIO_RUNTIME_MAX_FRAME_SIZE];
   uint16_t total_length;
   uint8_t channel;
   uint8_t header_length;
   uint8_t bdc_index;
   uint16_t ethernet_length;
   uint16_t ethertype;

   if (frame_length != NULL)
      *frame_length = 0u;

   total_length = hwtag[0];
   if ((uint16_t)(hwtag[0] ^ hwtag[1]) != (uint16_t)0xffffu || total_length < 12u
      || total_length > (uint16_t)sizeof(frame_buffer)) {
      /* Bad header: could be stale fn2 data, empty FIFO, or emulator returning dummy data.
         Don't spam logs with every bad frame in emulator mode. */
      (void)sdio_runtime_abort_function2_read(dev);
      return false;
   }

   memset(frame_buffer, 0, sizeof(frame_buffer));
   memcpy(frame_buffer, hwtag, 4u);
   if (!sdio_function2_transfer_timeout(dev, false, &frame_buffer[4],
                                        (uint16_t)(total_length - 4u),
                                        timeout_us)) {
      sdio_runtime_set_error("Failed to read SDPCM frame body");
      return false;
   }

   channel = (uint8_t)(frame_buffer[5] & SDPCM_CHANNEL_MASK);
   header_length = frame_buffer[7];

   /* Control-channel responses (channel 0) carry the chip's reply to
      every ioctl we sent. The CDC header sits right after the SDPCM
      header and its status word at offset header_length+12 tells us
      whether the firmware accepted the command. Logging this is the
      only way to see ioctls failing silently during the join sequence
      (e.g. unsupported iovar, bad WPA params, wrong WSEC). */
   if (channel == SDPCM_CONTROL_CHANNEL
      && total_length >= (uint16_t)(header_length + CDC_HEADER_LENGTH)) {
      uint32_t cdc_cmd = sdio_load_u32_le(&frame_buffer[header_length]);
      uint32_t cdc_flags = sdio_load_u32_le(&frame_buffer[header_length + 8u]);
      uint32_t cdc_status = sdio_load_u32_le(&frame_buffer[header_length + 12u]);

      sdio_debug_log("cdc rsp cmd=%lu flags=0x%08lx status=%lu%s",
                     (unsigned long)cdc_cmd,
                     (unsigned long)cdc_flags,
                     (unsigned long)cdc_status,
                     (cdc_flags & CDCF_IOC_ERROR) ? " ERROR" : "");

      /* GET-style responses carry the chip's return value in the bytes
         after the CDC header.  Dump up to 38 of those bytes so we can
         see what the chip actually has cached - critical for
         confirming whether SETs above were applied or silently dropped.
         WLC_GET_VAR covers iovar reads (bsscfg:event_msgs, chanspec,
         cur_etheraddr); WLC_GET_SSID and WLC_GET_BSSID are direct
         commands with their own return payloads. */
      if ((cdc_cmd == WLC_GET_VAR || cdc_cmd == WLC_GET_SSID || cdc_cmd == WLC_GET_BSSID
           || cdc_cmd == WLC_GET_WSEC || cdc_cmd == WLC_GET_WPA_AUTH
           || cdc_cmd == WLC_GET_AUTH || cdc_cmd == WLC_GET_INFRA)
          && !(cdc_flags & CDCF_IOC_ERROR)) {
         uint16_t payload_offset = (uint16_t)(header_length + CDC_HEADER_LENGTH);
             uint16_t payload_bytes = (uint16_t)(total_length - payload_offset);
         uint16_t dump_count = payload_bytes;
         char dump_buf[3 * 38 + 1];
         uint16_t i;
         char *cursor = dump_buf;

         if (dump_count > 38u)
            dump_count = 38u;

         for (i = 0u; i < dump_count; ++i) {
            static const char hex[] = "0123456789abcdef";
            uint8_t b = frame_buffer[payload_offset + i];

            *cursor++ = hex[(b >> 4) & 0xfu];
            *cursor++ = hex[b & 0xfu];
            *cursor++ = ' ';
         }
         *cursor = '\0';

         sdio_debug_log("cdc get-var len=%u: %s",
                        (unsigned)payload_bytes, dump_buf);
      }
      return false;
   }

   if (channel == SDPCM_EVENT_CHANNEL) {
      /* Async events arrive on EVENT_CHANNEL (1). The envelope used
         depends on firmware build:
           Modern (cyw43439, brcmfmac): Ethernet hdr (14) + BRCM event
             hdr (10) + event_msg (48), ethertype=0x886c, OUI=00:10:18.
           Legacy (BCM43430 on Pi Zero W): event_msg (48) directly,
             with no wrapper.
         Try the modern envelope first; if its ethertype check fails,
         fall back to decoding the payload as a bare event_msg. */
      uint16_t payload_length = (total_length > header_length)
         ? (uint16_t)(total_length - header_length) : 0u;

      if (payload_length >= 14u) {
         uint16_t ev_ethertype =
            (uint16_t)(((uint16_t)frame_buffer[header_length + 12u] << 8)
               | (uint16_t)frame_buffer[header_length + 13u]);
         if (ev_ethertype == ETHER_TYPE_BRCM) {
            sdio_runtime_note_event(frame_buffer, total_length, (uint16_t)header_length);
            return false;
         }
      }

      /* Fall through to bare event_msg path. */
      if (payload_length >= BRCM_EVENT_MSG_LENGTH) {
         sdio_runtime_note_event_bare(&frame_buffer[header_length], payload_length);
      } else {
         sdio_debug_log("ev: short ch=1 hdr=%u tot=%u",
                        (unsigned)header_length,
                        (unsigned)total_length);
      }
      return false; /* event consumed internally - no Ethernet frame for caller */
   }

   if (channel != SDPCM_DATA_CHANNEL || header_length < SDPCM_DATA_HEADER_LENGTH
      || total_length <= (uint16_t)(header_length + 4u)) {
      /* Frame on a channel we don't recognise (3..15) or malformed
         data frame. Worth surfacing during debug because it usually
         means we lost SDPCM framing - e.g. our hwtag read landed
         partway through a previous frame. */
      sdio_debug_log("fn2: drop ch=%u hdr=%u tot=%u",
                     (unsigned)channel,
                     (unsigned)header_length,
                     (unsigned)total_length);
      return false;
   }

   bdc_index = (uint8_t)(header_length);
   ethertype = (uint16_t)(((uint16_t)frame_buffer[header_length + 12u + 4u] << 8)
      | (uint16_t)frame_buffer[header_length + 13u + 4u]);
   ethernet_length = (uint16_t)(total_length - header_length - 4u);

   if (ethertype == ETHER_TYPE_BRCM) {
      /* DATA_CHANNEL BRCM events have a 4-byte BDC header before the Ethernet frame. */
      sdio_runtime_note_event(frame_buffer, total_length, (uint16_t)(header_length + 4u));
      return false;
   }

   if ((uint8_t)(frame_buffer[bdc_index] >> BDC_VERSION_SHIFT) != BDC_PROTOCOL_VERSION)
      return false;

   if (frame == NULL || frame_capacity < ethernet_length) {
      sdio_runtime_set_error("Ethernet frame exceeds receive buffer");
      return false;
   }

   memcpy(frame, &frame_buffer[header_length + 4u], ethernet_length);
   if (frame_length != NULL)
      *frame_length = ethernet_length;
   ++g_runtime_rx_frame_count;
   return true;
}

static bool sdio_runtime_complete_read_ethernet_frame(sdio_host_t *dev,
                                                      const uint16_t hwtag[2],
                                                      uint8_t *frame,
                                                      uint16_t frame_capacity,
                                                      uint16_t *frame_length)
{
   return sdio_runtime_complete_read_ethernet_frame_timeout(dev, hwtag, frame,
                                                            frame_capacity,
                                                            frame_length,
                                                            SDIO_COMMAND_TIMEOUT_US);
}

/* Drain any pending fn2 frames (CDC responses, events, control replies)
   between back-to-back control sends. The BCM43430 firmware queues a
   CDC response for every ioctl; without us reading them, fn2 RX fills,
   the firmware stalls, and later commands in the join sequence
   (including the JOIN ioctl itself) never get processed - which leaves
   the chip silent and link_up stuck at 0. Returns the number of frames
   actually consumed so the caller can verify each ioctl produced a
   response (a missing response means the chip silently dropped the
   command). */
static uint8_t sdio_drain_fn2_responses(sdio_host_t *dev)
{
   uint8_t scratch[SDIO_RUNTIME_MAX_FRAME_SIZE];
   uint8_t i;
   uint8_t consumed = 0u;

   for (i = 0u; i < SDIO_RUNTIME_MAX_RX_FRAMES_PER_POLL; ++i) {
      uint16_t hwtag[2] = { 0u, 0u };
      uint16_t scratch_length = 0u;

      if (!sdio_function2_transfer(dev, false, (uint8_t *)hwtag,
                                   (uint16_t)sizeof(hwtag)))
         break; /* CMD53 error - bail */

      if (hwtag[0] == 0u && hwtag[1] == 0u)
         break; /* fn2 FIFO empty */

      /* Consume the rest of the frame. complete_read_ethernet_frame
         logs control-channel CDC responses (channel 0) including any
         CDCF_IOC_ERROR set by the firmware, and processes event-channel
         frames into g_runtime_link_up via sdio_runtime_note_event. */
      (void)sdio_runtime_complete_read_ethernet_frame(dev, hwtag, scratch,
                                                       (uint16_t)sizeof(scratch),
                                                       &scratch_length);
      ++consumed;
   }

   return consumed;
}

static bool sdio_probe_send_single_tx_control_template_timeout(sdio_host_t *dev,
                                                               sdio_probe_result_t *probe_result,
                                                               uint32_t timeout_us)
{
   uint8_t tx_frame[SDPCM_CONTROL_EVENT_HEADER_LENGTH + CDC_HEADER_LENGTH + TX_CONTROL_TEMPLATE_MAX_PAYLOAD_LENGTH];
   sdio_cmd53_result_t cmd53_result;

   if (dev == NULL || probe_result == NULL || !probe_result->tx_control_template_ready)
      return false;

   memset(tx_frame, 0, sizeof(tx_frame));
   memset(&cmd53_result, 0, sizeof(cmd53_result));
   probe_result->tx_control_probe_attempted = true;

   sdio_store_u16_le(&tx_frame[0], probe_result->tx_control_template_frame_size);
   sdio_store_u16_le(&tx_frame[2], probe_result->tx_control_template_frame_size_complement);
   tx_frame[4] = probe_result->tx_control_template_sequence;
   tx_frame[5] = probe_result->tx_control_template_channel_and_flags;
   tx_frame[6] = probe_result->tx_control_template_next_length;
   tx_frame[7] = probe_result->tx_control_template_header_length;
   tx_frame[8] = probe_result->tx_control_template_wireless_flow_control;
   tx_frame[9] = probe_result->tx_control_template_bus_data_credit;
   sdio_store_u32_le(&tx_frame[12], probe_result->tx_control_template_command);
   sdio_store_u32_le(&tx_frame[16], probe_result->tx_control_template_cdc_length);
   sdio_store_u32_le(&tx_frame[20], probe_result->tx_control_template_cdc_flags);
   sdio_store_u32_le(&tx_frame[24], probe_result->tx_control_template_cdc_status);
   if (probe_result->tx_control_template_payload_length > 0u)
      memcpy(&tx_frame[28], probe_result->tx_control_template_payload_bytes,
             probe_result->tx_control_template_payload_length);

   if (!sdio_cmd53_execute_timeout(dev, 2u, 0u, true, false, false,
                                   probe_result->tx_control_template_frame_size, tx_frame,
                                   (uint32_t)probe_result->tx_control_template_frame_size,
                                   timeout_us, &cmd53_result)) {
      probe_result->tx_control_probe_response0 = cmd53_result.response0;
      probe_result->tx_control_probe_interrupt = cmd53_result.interrupt;
      probe_result->tx_control_probe_error = cmd53_result.error;
      return false;
   }

   probe_result->tx_control_probe_response0 = cmd53_result.response0;
   probe_result->tx_control_probe_interrupt = cmd53_result.interrupt;
   probe_result->tx_control_probe_error = cmd53_result.error;
   probe_result->tx_control_probe_success = true;
   return true;
}

static bool sdio_probe_send_single_tx_control_template(sdio_host_t *dev,
                                                       sdio_probe_result_t *probe_result)
{
   return sdio_probe_send_single_tx_control_template_timeout(dev, probe_result,
                                                             SDIO_COMMAND_TIMEOUT_US);
}

static bool sdio_probe_send_tx_control_template(sdio_host_t *dev,
                                                sdio_probe_result_t *probe_result,
                                                wifi_sdio_tx_probe_command_t command)
{
   wifi_sdio_tx_probe_command_t join_commands[TX_CONTROL_PROBE_JOIN_COMMAND_COUNT];
   uint8_t command_count;
   uint8_t index;

   if (dev == NULL || probe_result == NULL)
      return false;

   probe_result->tx_control_probe_multi_step = sdio_tx_probe_is_join_command(command);
   probe_result->tx_control_probe_steps_requested = 0u;
   probe_result->tx_control_probe_steps_completed = 0u;
   probe_result->tx_control_probe_last_command = 0u;
   probe_result->tx_control_probe_last_request_id = 0u;
   probe_result->tx_control_probe_last_sequence = 0u;

   if (!probe_result->tx_control_probe_multi_step)
      return sdio_probe_send_single_tx_control_template(dev, probe_result);

   command_count = sdio_tx_probe_join_commands(join_commands, sizeof(join_commands) / sizeof(join_commands[0]));
   probe_result->tx_control_probe_steps_requested = command_count;
   if (command_count == 0u)
      return false;

   for (index = 0u; index < command_count; ++index) {
      sdio_prepare_tx_control_template(probe_result, join_commands[index]);
      probe_result->tx_control_probe_last_command = probe_result->tx_control_template_command;
      probe_result->tx_control_probe_last_request_id = probe_result->tx_control_template_request_id;
      probe_result->tx_control_probe_last_sequence = probe_result->tx_control_template_sequence;
      if (!sdio_probe_send_single_tx_control_template(dev, probe_result))
         return false;
      probe_result->tx_control_probe_steps_completed = (uint8_t)(index + 1u);

      /* Give the firmware time to parse this ioctl, then drain its
         CDC response from fn2 before sending the next one. Without
         this pair the BCM43430's RX FIFO backs up after a handful of
         ioctls and the JOIN never executes - matching the symptom
         where event_type/event_status stay at 0 and the link never
         comes up. 10 ms is comfortably above the BCM43430's worst
         case ack latency at 25 MHz; 5 ms turned out to race with
         iovars that have to touch SOC memory (PMK, event_msgs). */
      {
         uint8_t consumed;

         usleep(10000u);
         consumed = sdio_drain_fn2_responses(dev);
         sdio_debug_log("ioctl step=%u cmd=%lu drained=%u",
                        (unsigned)(index + 1u),
                        (unsigned long)probe_result->tx_control_template_command,
                        (unsigned)consumed);
      }
   }

   return true;
}

static bool sdio_probe_sweep_rx_frames(sdio_host_t *dev,
                                       sdio_probe_result_t *probe_result,
                                       uint8_t max_frames)
{
   uint8_t frame_index;

   if (dev == NULL || probe_result == NULL)
      return false;

   probe_result->rx_frame_sweep_attempted = true;
   probe_result->rx_frame_sweep_limit = max_frames;
   probe_result->rx_frames_decoded = 0u;
   probe_result->rx_frame_sweep_more_pending = false;

   for (frame_index = 0u; frame_index < max_frames; ++frame_index) {
      if (!sdio_probe_read_function2_registers(dev, probe_result))
         return false;

      if (probe_result->read_frame_byte_count == 0u)
         break;

      if (!sdio_probe_read_frame_header(dev, probe_result))
         return false;

      if (!probe_result->frame_header_probe_success)
         return false;

      if (probe_result->frame_header_size == 0u && probe_result->frame_header_size_complement == 0u)
         break;

      probe_result->rx_frames_decoded = (uint8_t)(frame_index + 1u);
   }

   {
      bool reached_limit = probe_result->rx_frames_decoded == max_frames && max_frames != 0u;
   if (reached_limit) {
      if (!sdio_probe_read_function2_registers(dev, probe_result))
         return false;
      probe_result->rx_frame_sweep_more_pending = probe_result->read_frame_byte_count != 0u;
   }
   }

   probe_result->rx_frame_sweep_success = true;
   return true;
}

static bool sdio_probe_read_tx_post_state(sdio_host_t *dev,
                                          sdio_probe_result_t *probe_result)
{
   uint8_t read_frame_byte_count_low = 0;
   uint8_t read_frame_byte_count_high = 0;
   uint32_t int_status = 0;
   uint32_t to_sb_mailbox = 0;
   uint32_t to_host_mailbox_data = 0;

   if (dev == NULL || probe_result == NULL)
      return false;

   probe_result->tx_control_post_state_probe_attempted = true;
   if (!sdio_function1_read_byte(dev, SDIO_READ_FRAME_BC_LOW, &read_frame_byte_count_low)
      || !sdio_function1_read_byte(dev, SDIO_READ_FRAME_BC_HIGH, &read_frame_byte_count_high)
      || !sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_STATUS_OFFSET, &int_status)
      || !sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_TO_SB_MAILBOX_OFFSET, &to_sb_mailbox)
      || !sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET, &to_host_mailbox_data)) {
      return false;
   }

   probe_result->tx_control_post_read_frame_byte_count = (uint16_t)((uint16_t)read_frame_byte_count_low
      | ((uint16_t)read_frame_byte_count_high << 8));
   probe_result->tx_control_post_int_status = int_status;
   probe_result->tx_control_post_to_sb_mailbox = to_sb_mailbox;
   probe_result->tx_control_post_to_host_mailbox_data = to_host_mailbox_data;
   probe_result->tx_control_post_state_probe_success = true;
   return true;
}

bool sdio_function_is_valid(uint8_t function_number)
{
   return function_number <= 7u;
}

uint32_t sdio_cmd52_argument(uint8_t function_number, uint32_t address, bool write,
                             bool read_after_write, uint8_t data)
{
   if (!sdio_function_is_valid(function_number))
      return 0;

   return ((uint32_t)(write ? 1u : 0u) << 31)
      | ((uint32_t)function_number << 28)
      | ((uint32_t)(read_after_write ? 1u : 0u) << 27)
      | ((address & 0x1FFFFu) << 9)
      | (uint32_t)data;
}

uint32_t sdio_cmd53_argument(uint8_t function_number, uint32_t address, bool write,
                             bool block_mode, bool incrementing_address,
                             uint16_t count)
{
   if (!sdio_function_is_valid(function_number))
      return 0;

   /* The count field is only 9 bits (bits 0..8). The SDIO spec uses
      count_field == 0 to mean "512" for byte mode (and "infinite"/"max"
      for block mode), so any caller passing count == 512 must end up with
      a zero count_field. Without the mask the high bit of 0x200 spilled
      into bit 9 of the encoded argument - which sits in the *address*
      field - and shifted the SDIO target address up by one, causing the
      chip to return data offset by one byte and the host to see a
      DCRC_ERR on the read. */
   return ((uint32_t)(write ? 1u : 0u) << 31)
      | ((uint32_t)function_number << 28)
      | ((uint32_t)(block_mode ? 1u : 0u) << 27)
      | ((uint32_t)(incrementing_address ? 1u : 0u) << 26)
      | ((address & 0x1FFFFu) << 9)
      | ((uint32_t)count & 0x1FFu);
}

sdio_ocr_info_t sdio_decode_ocr(uint32_t raw_ocr)
{
   sdio_ocr_info_t info;

   info.raw_ocr = raw_ocr;
   info.function_count = (uint8_t)((raw_ocr >> 28) & 0x7u);
   info.memory_present = ((raw_ocr >> 27) & 0x1u) != 0u;
   info.supports_1p8v = ((raw_ocr >> 24) & 0x1u) != 0u;

   return info;
}

static bool sdio_cmd52_execute_timeout(sdio_host_t *dev, uint8_t function_number,
                                       uint32_t address, bool write,
                                       bool read_after_write, uint8_t *data,
                                       uint32_t timeout_us,
                                       sdio_cmd52_result_t *result)
{
   sdio_host_command_t command;
   sdio_host_result_t host_result;
   uint8_t value = 0;

   if (!sdio_function_is_valid(function_number))
      return false;

   if (data != NULL)
      value = *data;

   /* CMD52 returns R5: 48-bit, CRC checked, command index checked. */
   command.command = (52u << 24) | (2u << 16) | (1u << 19) | (1u << 20);
   command.argument = sdio_cmd52_argument(function_number, address, write,
                                          read_after_write, value);
   command.timeout_us = timeout_us;
   command.buffer = NULL;
   command.block_size = 0;
   command.blocks_to_transfer = 0;

   if (sdio_host_submit(dev, &command, &host_result) != 0) {
      sdio_fill_cmd52_result(result, &host_result);
      sdio_runtime_set_host_command_error("CMD52 failed", function_number, address,
                                          &host_result);
      return false;
   }

   sdio_fill_cmd52_result(result, &host_result);
   if (data != NULL)
      *data = result != NULL ? result->data : (uint8_t)(host_result.response0 & 0xffu);

   return true;
}

bool sdio_cmd52_execute(sdio_host_t *dev, uint8_t function_number,
                        uint32_t address, bool write, bool read_after_write,
                        uint8_t *data, sdio_cmd52_result_t *result)
{
   return sdio_cmd52_execute_timeout(dev, function_number, address, write,
                                     read_after_write, data,
                                     SDIO_COMMAND_TIMEOUT_US, result);
}

static bool sdio_cmd53_execute_timeout(sdio_host_t *dev, uint8_t function_number,
                                       uint32_t address, bool write,
                                       bool block_mode,
                                       bool incrementing_address,
                                       uint16_t count, void *buffer,
                                       uint32_t block_size,
                                       uint32_t timeout_us,
                                       sdio_cmd53_result_t *result)
{
   sdio_host_command_t command;
   sdio_host_result_t host_result;

   if (!sdio_function_is_valid(function_number) || count == 0u)
      return false;

   /* CMD53 returns R5: 48-bit, CRC checked, command index checked.
      Transfer Mode bits (low 8 bits of CMDTM) used:
        bit 0  TM_DMA_EN     - leave 0, we drain via PIO not SDMA;
                               setting it makes the controller wait for
                               a DMA transfer that never starts.
        bit 1  TM_BLKCNT_EN  - tells the controller to honour the
                               BLKCNT field of BLKSIZECNT so the
                               multi-block transfer terminates after
                               exactly N blocks. Without this set with
                               TM_MULTI_BLOCK, the controller goes into
                               "infinite stream" mode and waits for a
                               CMD12 stop-transmission that an SDIO
                               CMD53 never sends - hence no interrupts
                               fire at all.
        bit 4  TM_DAT_DIR    - 0 = write, 1 = read.
        bit 5  TM_MULTI_BLOCK- set only for block-mode transfers carrying
                               more than one block, so the controller
                               expects a per-block CRC framing. */
   {
      uint32_t tm_bits = (write ? 0u : (1u << 4));
      if (buffer != NULL)
         tm_bits |= (1u << 21);
      if (block_mode && count > 1u)
         tm_bits |= (1u << 5) | (1u << 1);

      command.command = (53u << 24) | (2u << 16) | (1u << 19) | (1u << 20)
         | tm_bits;
   }
   command.argument = sdio_cmd53_argument(function_number, address, write,
                                          block_mode, incrementing_address,
                                          count);
   command.timeout_us = timeout_us;
   command.buffer = buffer;
   command.block_size = block_mode ? block_size : count;
   command.blocks_to_transfer = block_mode ? count : 1u;

   if (sdio_host_submit(dev, &command, &host_result) != 0) {
      sdio_fill_cmd53_result(result, &host_result);
      sdio_runtime_set_host_command_error("CMD53 failed", function_number, address,
                                          &host_result);
      return false;
   }

   sdio_fill_cmd53_result(result, &host_result);
   return true;
}

bool sdio_cmd53_execute(sdio_host_t *dev, uint8_t function_number,
                        uint32_t address, bool write, bool block_mode,
                        bool incrementing_address, uint16_t count, void *buffer,
                        uint32_t block_size, sdio_cmd53_result_t *result)
{
   return sdio_cmd53_execute_timeout(dev, function_number, address, write,
                                     block_mode, incrementing_address, count,
                                     buffer, block_size,
                                     SDIO_COMMAND_TIMEOUT_US, result);
}

static bool sdio_probe_read_byte(sdio_host_t *dev, uint32_t address,
                                 uint8_t *value)
{
   return sdio_cmd52_execute(dev, 0, address, false, false, value, NULL);
}

static bool sdio_probe_write_byte(sdio_host_t *dev, uint32_t address,
                                  uint8_t value)
{
   return sdio_cmd52_execute(dev, 0, address, true, true, &value, NULL);
}

static bool sdio_probe_set_block_size(sdio_host_t *dev,
                                      uint8_t function_number,
                                      uint16_t block_size)
{
   uint8_t block_size_low = (uint8_t)(block_size & 0xffu);
   uint8_t block_size_high = (uint8_t)((block_size >> 8) & 0xffu);

   return sdio_probe_write_byte(dev, SDIO_FBR_BLOCK_SIZE_LOW(function_number), block_size_low)
      && sdio_probe_write_byte(dev, SDIO_FBR_BLOCK_SIZE_HIGH(function_number), block_size_high);
}

static bool sdio_probe_enable_functions(sdio_host_t *dev,
                                        sdio_probe_result_t *probe_result)
{
   uint8_t requested_io_enable;
   uint8_t fn1_ready_mask;
   uint8_t io_ready = 0;
   unsigned int attempts;

   if (probe_result == NULL)
      return false;

   /* Log OCR to diagnose emulator encoding */
   sdio_debug_log("enable_functions: OCR=0x%08lx fn_count=%u raw_response=0x%08lx",
                  (unsigned long)probe_result->ocr.raw_ocr,
                  (unsigned int)probe_result->ocr.function_count,
                  (unsigned long)probe_result->response0);

   /* Emulator workaround: if function count isn't encoded but response is
      non-zero, assume standard 2-function card (fn1=backplane, fn2=data).
      Real hardware will also have bits [30:28] set correctly. */
   if (probe_result->ocr.function_count < 2u && probe_result->response0 != 0u) {
      sdio_debug_log("enable_functions: working around emulator OCR encoding - accepting 2 functions");
      probe_result->ocr.function_count = 2u;
   }

   if (probe_result->ocr.function_count < 2u) {
      sdio_runtime_set_error("SDIO card reported fewer than 2 functions");
      return false;
   }

   /* Enable function 1 (backplane access) here. Function 2 (radio data
      path) cannot become ready until the WiFi firmware has been loaded
      over function 1, which happens later in sdio_runtime_boot_firmware,
      so we explicitly only request fn1 at this stage and only poll for
      fn1 ready. The fn2 enable is performed after firmware boot. */
   requested_io_enable = (uint8_t)(probe_result->io_enable | 0x02u);
   fn1_ready_mask = 0x02u;
   probe_result->requested_io_enable = requested_io_enable;
   probe_result->function_setup_attempted = true;

   if (!sdio_probe_write_byte(dev, SDIO_CCCR_IO_ENABLE, requested_io_enable))
      return false;

   for (attempts = 0; attempts < 100u; ++attempts) {
      if (!sdio_probe_read_byte(dev, SDIO_CCCR_IO_READY, &io_ready))
         return false;

      if ((io_ready & fn1_ready_mask) == fn1_ready_mask)
         break;

      usleep(1000u);
   }

   if ((io_ready & fn1_ready_mask) != fn1_ready_mask) {
      sdio_runtime_set_error("Timed out waiting for SDIO function 1 ready");
      return false;
   }

   if (!sdio_probe_read_byte(dev, SDIO_CCCR_IO_ENABLE, &probe_result->configured_io_enable))
      return false;

   probe_result->configured_io_ready = io_ready;

   if (!sdio_probe_set_block_size(dev, 1u, SDIO_PROBE_FUNCTION1_BLOCK_SIZE)
      || !sdio_probe_set_block_size(dev, 2u, SDIO_PROBE_FUNCTION2_BLOCK_SIZE))
      return false;

   probe_result->function1_block_size = SDIO_PROBE_FUNCTION1_BLOCK_SIZE;
   probe_result->function2_block_size = SDIO_PROBE_FUNCTION2_BLOCK_SIZE;
   probe_result->function_setup_success = true;
   return true;
}

static bool sdio_backplane_set_window_timeout(sdio_host_t *dev, uint32_t address,
                                              uint32_t timeout_us)
{
   uint8_t addr_low = (uint8_t)((address >> 8) & 0xffu);
   uint8_t addr_mid = (uint8_t)((address >> 16) & 0xffu);
   uint8_t addr_high = (uint8_t)((address >> 24) & 0xffu);

   return sdio_cmd52_execute_timeout(dev, 1u, SDIO_BACKPLANE_ADDRESS_LOW,
                                     true, true, &addr_low, timeout_us, NULL)
      && sdio_cmd52_execute_timeout(dev, 1u, SDIO_BACKPLANE_ADDRESS_MID,
                                    true, true, &addr_mid, timeout_us, NULL)
      && sdio_cmd52_execute_timeout(dev, 1u, SDIO_BACKPLANE_ADDRESS_HIGH,
                                    true, true, &addr_high, timeout_us, NULL);
}

static bool sdio_backplane_set_window(sdio_host_t *dev, uint32_t address)
{
   return sdio_backplane_set_window_timeout(dev, address,
                                            SDIO_COMMAND_TIMEOUT_US);
}

static bool sdio_backplane_read_u32_timeout(sdio_host_t *dev, uint32_t address,
                                            uint32_t timeout_us,
                                            uint32_t *value)
{
   uint32_t local_value = 0;
   uint32_t transfer_address;

   if (value == NULL)
      return false;

   if (!sdio_backplane_set_window_timeout(dev, address, timeout_us))
      return false;

   transfer_address = (address & SDIO_BACKPLANE_OFFSET_MASK) | SDIO_BACKPLANE_ACCESS_2_4B_FLAG;
   if (!sdio_cmd53_execute_timeout(dev, 1u, transfer_address, false, false, true,
                                   (uint16_t)sizeof(local_value), &local_value,
                                   (uint32_t)sizeof(local_value), timeout_us, NULL)) {
      return false;
   }

   *value = local_value;
   return true;
}

static bool sdio_backplane_read_u32(sdio_host_t *dev, uint32_t address, uint32_t *value)
{
   return sdio_backplane_read_u32_timeout(dev, address, SDIO_COMMAND_TIMEOUT_US,
                                          value);
}

static bool sdio_backplane_write_u32_timeout(sdio_host_t *dev, uint32_t address,
                                             uint32_t value,
                                             uint32_t timeout_us)
{
   uint32_t local_value = value;
   uint32_t transfer_address;

   if (!sdio_backplane_set_window_timeout(dev, address, timeout_us))
      return false;

   transfer_address = (address & SDIO_BACKPLANE_OFFSET_MASK) | SDIO_BACKPLANE_ACCESS_2_4B_FLAG;
   return sdio_cmd53_execute_timeout(dev, 1u, transfer_address, true, false, true,
                                     (uint16_t)sizeof(local_value), &local_value,
                                     (uint32_t)sizeof(local_value), timeout_us, NULL);
}

static bool sdio_backplane_write_u32(sdio_host_t *dev, uint32_t address, uint32_t value)
{
   return sdio_backplane_write_u32_timeout(dev, address, value,
                                           SDIO_COMMAND_TIMEOUT_US);
}

static bool sdio_function1_read_byte(sdio_host_t *dev, uint32_t address, uint8_t *value)
{
   return sdio_cmd52_execute(dev, 1u, address, false, false, value, NULL);
}

static bool sdio_function1_write_byte(sdio_host_t *dev, uint32_t address, uint8_t value)
{
   return sdio_cmd52_execute(dev, 1u, address, true, true, &value, NULL);
}

static bool sdio_probe_request_alp_clock(sdio_host_t *dev,
                                         sdio_probe_result_t *probe_result)
{
   uint8_t clock_csr = 0;
   uint8_t requested_clock_csr;
   unsigned int attempts;

   if (probe_result == NULL)
      return false;

   probe_result->clock_probe_attempted = true;

   if (!sdio_function1_read_byte(dev, SDIO_CHIP_CLOCK_CSR, &clock_csr))
      return false;

   probe_result->chip_clock_csr_initial = clock_csr;
   requested_clock_csr = (uint8_t)(clock_csr | SDIO_FORCE_HW_CLKREQ_OFF | SDIO_ALP_AVAIL_REQ | SDIO_FORCE_ALP);
   probe_result->chip_clock_csr_requested = requested_clock_csr;

   if (!sdio_function1_write_byte(dev, SDIO_CHIP_CLOCK_CSR, requested_clock_csr))
      return false;

   for (attempts = 0; attempts < 100u; ++attempts) {
      if (!sdio_function1_read_byte(dev, SDIO_CHIP_CLOCK_CSR, &clock_csr))
         return false;

      if ((clock_csr & SDIO_ALP_AVAIL) != 0u)
         break;

      usleep(1000u);
   }

   probe_result->chip_clock_csr_final = clock_csr;
   if ((clock_csr & SDIO_ALP_AVAIL) == 0u) {
      sdio_runtime_set_error("Timed out waiting for SDIO ALP clock");
      return false;
   }

   probe_result->clock_probe_success = true;
   return true;
}

static bool sdio_probe_read_power_registers(sdio_host_t *dev,
                                            sdio_probe_result_t *probe_result)
{
   uint8_t wakeup_control = 0;
   uint8_t sleep_control_status = 0;

   if (probe_result == NULL)
      return false;

   if (!sdio_function1_read_byte(dev, SDIO_WAKEUP_CTRL, &wakeup_control)
      || !sdio_function1_read_byte(dev, SDIO_SLEEP_CSR, &sleep_control_status)) {
      return false;
   }

   probe_result->wakeup_control = wakeup_control;
   probe_result->sleep_control_status = sleep_control_status;
   probe_result->power_probe_success = true;
   return true;
}

static bool sdio_probe_wake_with_kso(sdio_host_t *dev,
                                     sdio_probe_result_t *probe_result)
{
   uint8_t requested_value;
   uint8_t sleep_control_status = 0;
   unsigned int attempts;

   if (probe_result == NULL)
      return false;

   probe_result->kso_probe_attempted = true;
   requested_value = (uint8_t)(probe_result->sleep_control_status | SDIO_SLEEP_CSR_KEEP_WL_KSO);
   probe_result->kso_control_requested = requested_value;

   (void) sdio_function1_write_byte(dev, SDIO_SLEEP_CSR, requested_value);
   (void) sdio_function1_write_byte(dev, SDIO_SLEEP_CSR, requested_value);

   for (attempts = 0; attempts < 64u; ++attempts) {
      if (!sdio_function1_read_byte(dev, SDIO_SLEEP_CSR, &sleep_control_status))
         return false;

      if ((sleep_control_status & (SDIO_SLEEP_CSR_KEEP_WL_KSO | SDIO_SLEEP_CSR_WL_DEVON))
         == (SDIO_SLEEP_CSR_KEEP_WL_KSO | SDIO_SLEEP_CSR_WL_DEVON)) {
         break;
      }

      usleep(1000u);
      (void) sdio_function1_write_byte(dev, SDIO_SLEEP_CSR, requested_value);
   }

   probe_result->kso_control_final = sleep_control_status;
   probe_result->sleep_control_status = sleep_control_status;

   if ((sleep_control_status & (SDIO_SLEEP_CSR_KEEP_WL_KSO | SDIO_SLEEP_CSR_WL_DEVON))
      != (SDIO_SLEEP_CSR_KEEP_WL_KSO | SDIO_SLEEP_CSR_WL_DEVON)) {
      sdio_runtime_set_error("Timed out waking SDIO core with KSO");
      return false;
   }

   probe_result->kso_probe_success = true;
   return true;
}

static bool sdio_probe_read_mailbox_registers(sdio_host_t *dev,
                                              sdio_probe_result_t *probe_result)
{
   uint32_t int_status;
   uint32_t int_host_mask;
   uint32_t to_sb_mailbox;
   uint32_t to_host_mailbox_data;

   if (probe_result == NULL)
      return false;

   probe_result->sdio_core_base = CYW43_SDIO_CORE_BASE;

   if (!sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_STATUS_OFFSET, &int_status)
      || !sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_HOST_MASK_OFFSET, &int_host_mask)
      || !sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_TO_SB_MAILBOX_OFFSET, &to_sb_mailbox)
      || !sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET, &to_host_mailbox_data)) {
      return false;
   }

   probe_result->sdio_int_status = int_status;
   probe_result->sdio_int_host_mask = int_host_mask;
   probe_result->sdio_to_sb_mailbox = to_sb_mailbox;
   probe_result->sdio_to_host_mailbox_data = to_host_mailbox_data;
   probe_result->mailbox_probe_success = true;
   return true;
}

static bool sdio_probe_ack_interrupts(sdio_host_t *dev,
                                      sdio_probe_result_t *probe_result)
{
   uint32_t ack_value;
   uint32_t int_status_after_ack;
   unsigned int hmb_round;
   unsigned int no_hmb_count;

   if (probe_result == NULL)
      return false;

   ack_value = probe_result->sdio_int_status & SDIO_HOST_INTERRUPT_MASK;
   probe_result->sdio_interrupt_ack_value = ack_value;

   if (ack_value != 0u) {
      /* Clear the I_HMB_SW bits captured in the mailbox snapshot. */
      probe_result->interrupt_ack_attempted = true;
      if (!sdio_backplane_write_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_STATUS_OFFSET, ack_value)
         || !sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_STATUS_OFFSET, &int_status_after_ack)) {
         return false;
      }
      probe_result->sdio_int_status_after_ack = int_status_after_ack;
      probe_result->interrupt_ack_success = true;

      /* The snapshot clearing above only acks the INT_STATUS bit but does NOT
         send SMB_INT_ACK to TO_SB_MAILBOX.  The firmware is now stalled waiting
         for SMB_INT_ACK before it will proceed to DEVREADY→FWREADY (round 2).
         Send it now so the firmware can continue. */
      (void)sdio_backplane_write_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_TO_SB_MAILBOX_OFFSET,
                                     0x00000002u); /* SMB_INT_ACK */
   }

   /* SDPCM version handshake: after the host writes CYW43_SB_PROTOCOL_VERSION to
      TO_SB_MAILBOX the firmware replies with two I_HMB_SW interrupts:
        Round 1: I_HMB_SW2 (0x40) + HMB_DATA = 0x00040008  (DEVREADY, bit 3)
        Round 2: I_HMB_SW3 (0x80) + HMB_DATA = 0x00040002  (FWREADY,  bit 1)
      Each round requires the host to: ack INT_STATUS, read HMB_DATA, write
      SMB_INT_ACK to TO_SB_MAILBOX.  Without completing round 2 the firmware
      will never deliver frames or events on fn2.

      If ack_value was non-zero above, round 1 already happened (INT_STATUS bit
      was in the snapshot) and we just sent its SMB_INT_ACK.  The loop below
      waits for round 2 (and also handles round 1 if it wasn't in the snapshot). */
   no_hmb_count = 0u;
   for (hmb_round = 0u; hmb_round < 30u; ++hmb_round) {
      uint32_t int_status = 0u;
      usleep(10000u); /* 10 ms between polls */
      if (!sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_STATUS_OFFSET,
                                   &int_status))
         break;
      if (int_status & SDIO_HOST_INTERRUPT_MASK) {
         uint32_t hmb_data = 0u;
         no_hmb_count = 0u;
         (void)sdio_backplane_write_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_STATUS_OFFSET,
                                        int_status);
         (void)sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET,
                                       &hmb_data);
         (void)sdio_backplane_write_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_TO_SB_MAILBOX_OFFSET,
                                        0x00000002u); /* SMB_INT_ACK */
         if (hmb_data & 0x00000002u) /* FWREADY - firmware fully up */
            break;
      } else {
         if (++no_hmb_count >= 3u)
            break; /* 30 ms with no activity - handshake done or firmware not responding */
      }
   }

   return true;
}

static bool sdio_probe_write_interrupt_mask(sdio_host_t *dev,
                                           sdio_probe_result_t *probe_result)
{
   uint32_t int_host_mask_after_write;

   if (probe_result == NULL)
      return false;

   probe_result->interrupt_mask_write_attempted = true;
   probe_result->sdio_int_host_mask_requested = SDIO_HOST_INTERRUPT_MASK;
   if (!sdio_backplane_write_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_HOST_MASK_OFFSET,
                                 SDIO_HOST_INTERRUPT_MASK)
      || !sdio_backplane_read_u32(dev, CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_HOST_MASK_OFFSET,
                                  &int_host_mask_after_write)) {
      return false;
   }

   probe_result->sdio_int_host_mask_after_write = int_host_mask_after_write;
   if (int_host_mask_after_write != SDIO_HOST_INTERRUPT_MASK) {
      sdio_runtime_set_error("SDIO interrupt mask readback mismatch");
      return false;
   }

   probe_result->interrupt_mask_write_success = true;
   return true;
}

static bool sdio_probe_read_function2_registers(sdio_host_t *dev,
                                                sdio_probe_result_t *probe_result)
{
   uint8_t function2_info = 0;
   uint8_t function2_watermark = 0;
   uint8_t read_frame_byte_count_low = 0;
   uint8_t read_frame_byte_count_high = 0;

   if (probe_result == NULL)
      return false;

   if (!sdio_probe_read_byte(dev, SDIO_CCCR_FUNCTION2_INFO, &function2_info)
      || !sdio_function1_read_byte(dev, SDIO_FUNCTION2_WATERMARK, &function2_watermark)
      || !sdio_function1_read_byte(dev, SDIO_READ_FRAME_BC_LOW, &read_frame_byte_count_low)
      || !sdio_function1_read_byte(dev, SDIO_READ_FRAME_BC_HIGH, &read_frame_byte_count_high)) {
      return false;
   }

   probe_result->function2_info = function2_info;
   probe_result->function2_watermark = function2_watermark;
   probe_result->read_frame_byte_count = (uint16_t)((uint16_t)read_frame_byte_count_low
      | ((uint16_t)read_frame_byte_count_high << 8));
   probe_result->function2_probe_success = true;
   return true;
}

static bool sdio_probe_abort_function2_read(sdio_host_t *dev,
                                            sdio_probe_result_t *probe_result)
{
   if (probe_result == NULL)
      return false;

   probe_result->frame_read_abort_attempted = true;
   if (!sdio_probe_write_byte(dev, SDIO_CCCR_IO_ABORT, SDIO_IO_ABORT_FUNCTION2)
      || !sdio_function1_write_byte(dev, SDIO_FRAME_CONTROL, SDIO_FRAME_CONTROL_READ_TERMINATE)) {
      return false;
   }

   probe_result->frame_read_abort_success = true;
   return true;
}

static bool sdio_probe_read_post_header_prefix(sdio_host_t *dev,
                                               sdio_probe_result_t *probe_result)
{
   uint8_t prefix_buffer[16];
   uint8_t prefix_offset;
   uint8_t read_count;

   if (probe_result == NULL)
      return false;

   if (!probe_result->sdpcm_header_sane)
      return true;

   probe_result->sdpcm_post_header_probe_attempted = true;
   prefix_offset = 0u;
   read_count = SDPCM_PREFIX_LENGTH;
   if (probe_result->sdpcm_channel == SDPCM_DATA_CHANNEL) {
      prefix_offset = (uint8_t)(probe_result->sdpcm_expected_header_length - SDPCM_CONTROL_EVENT_HEADER_LENGTH);
      read_count = (uint8_t)(prefix_offset + SDPCM_PREFIX_LENGTH);
   } else if (probe_result->sdpcm_channel == SDPCM_CONTROL_CHANNEL) {
      read_count = CDC_HEADER_LENGTH;
      probe_result->sdpcm_cdc_header_probe_attempted = true;
   }

   probe_result->sdpcm_post_header_bytes_requested = read_count;
   if (probe_result->frame_header_size < (uint16_t)(probe_result->sdpcm_expected_header_length + read_count))
      return true;

   memset(prefix_buffer, 0, sizeof(prefix_buffer));
   if (!sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                           read_count, prefix_buffer,
                           (uint32_t)read_count, NULL)) {
      return false;
   }

   probe_result->sdpcm_post_header_prefix0 = prefix_buffer[prefix_offset + 0u];
   probe_result->sdpcm_post_header_prefix1 = prefix_buffer[prefix_offset + 1u];
   probe_result->sdpcm_post_header_prefix2 = prefix_buffer[prefix_offset + 2u];
   probe_result->sdpcm_post_header_prefix3 = prefix_buffer[prefix_offset + 3u];
   probe_result->sdpcm_post_header_probe_success = true;

   if (probe_result->sdpcm_channel == SDPCM_DATA_CHANNEL) {
      uint8_t ethertype_buffer[128];
      uint16_t bytes_after_bdc;
      uint16_t ethertype_read_count;
      uint16_t brcm_event_read_count;
      uint16_t brcm_event_msg_read_count;

      probe_result->sdpcm_bdc_flags = probe_result->sdpcm_post_header_prefix0;
      probe_result->sdpcm_bdc_priority = probe_result->sdpcm_post_header_prefix1;
      probe_result->sdpcm_bdc_flags2 = probe_result->sdpcm_post_header_prefix2;
      probe_result->sdpcm_bdc_data_offset = probe_result->sdpcm_post_header_prefix3;
      probe_result->sdpcm_bdc_version = (uint8_t)(probe_result->sdpcm_bdc_flags >> BDC_VERSION_SHIFT);
      probe_result->sdpcm_bdc_version_valid = probe_result->sdpcm_bdc_version == BDC_PROTOCOL_VERSION;
      probe_result->sdpcm_bdc_data_offset_bytes = (uint8_t)(probe_result->sdpcm_bdc_data_offset << 2);
      bytes_after_bdc = (uint16_t)(probe_result->frame_header_size - probe_result->sdpcm_expected_header_length - SDPCM_PREFIX_LENGTH);
      probe_result->sdpcm_bdc_data_offset_sane = probe_result->sdpcm_bdc_data_offset_bytes <= bytes_after_bdc;
      probe_result->sdpcm_bdc_header_decoded = true;
      probe_result->sdpcm_data_ethertype_probe_attempted = true;
      ethertype_read_count = (uint16_t)probe_result->sdpcm_bdc_data_offset_bytes + ETHERNET_HEADER_LENGTH;
      if (probe_result->sdpcm_bdc_version_valid
         && probe_result->sdpcm_bdc_data_offset_sane
         && ethertype_read_count <= bytes_after_bdc
         && ethertype_read_count <= (uint16_t)sizeof(ethertype_buffer)) {
         memset(ethertype_buffer, 0, sizeof(ethertype_buffer));
         if (sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                                ethertype_read_count, ethertype_buffer,
                                (uint32_t)ethertype_read_count, NULL)) {
            probe_result->sdpcm_data_ethertype = (uint16_t)(((uint16_t)ethertype_buffer[probe_result->sdpcm_bdc_data_offset_bytes + 12u] << 8)
               | (uint16_t)ethertype_buffer[probe_result->sdpcm_bdc_data_offset_bytes + 13u]);
            probe_result->sdpcm_data_ethertype_probe_success = true;
            if (probe_result->sdpcm_data_ethertype == ETHER_TYPE_BRCM) {
               probe_result->sdpcm_brcm_event_probe_attempted = true;
               brcm_event_read_count = (uint16_t)probe_result->sdpcm_bdc_data_offset_bytes
                  + ETHERNET_HEADER_LENGTH + BRCM_EVENT_HEADER_LENGTH;
               if (brcm_event_read_count <= bytes_after_bdc
                  && brcm_event_read_count <= (uint16_t)sizeof(ethertype_buffer)) {
                  memset(ethertype_buffer, 0, sizeof(ethertype_buffer));
                  if (sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                                         brcm_event_read_count, ethertype_buffer,
                                         (uint32_t)brcm_event_read_count, NULL)) {
                     uint16_t event_offset = (uint16_t)probe_result->sdpcm_bdc_data_offset_bytes + ETHERNET_HEADER_LENGTH;

                     probe_result->sdpcm_brcm_event_subtype = (uint16_t)(((uint16_t)ethertype_buffer[event_offset + 0u] << 8)
                        | (uint16_t)ethertype_buffer[event_offset + 1u]);
                     probe_result->sdpcm_brcm_event_length = (uint16_t)(((uint16_t)ethertype_buffer[event_offset + 2u] << 8)
                        | (uint16_t)ethertype_buffer[event_offset + 3u]);
                     probe_result->sdpcm_brcm_event_version = ethertype_buffer[event_offset + 4u];
                     probe_result->sdpcm_brcm_event_oui0 = ethertype_buffer[event_offset + 5u];
                     probe_result->sdpcm_brcm_event_oui1 = ethertype_buffer[event_offset + 6u];
                     probe_result->sdpcm_brcm_event_oui2 = ethertype_buffer[event_offset + 7u];
                     probe_result->sdpcm_brcm_event_usr_subtype = (uint16_t)(((uint16_t)ethertype_buffer[event_offset + 8u] << 8)
                        | (uint16_t)ethertype_buffer[event_offset + 9u]);
                     probe_result->sdpcm_brcm_event_oui_match = probe_result->sdpcm_brcm_event_oui0 == BRCM_OUI0
                        && probe_result->sdpcm_brcm_event_oui1 == BRCM_OUI1
                        && probe_result->sdpcm_brcm_event_oui2 == BRCM_OUI2;
                     probe_result->sdpcm_brcm_event_version_valid = probe_result->sdpcm_brcm_event_version == BRCM_EVENT_VERSION;
                     probe_result->sdpcm_brcm_event_probe_success = true;
                     probe_result->sdpcm_brcm_event_msg_probe_attempted = true;
                     brcm_event_msg_read_count = (uint16_t)probe_result->sdpcm_bdc_data_offset_bytes
                        + ETHERNET_HEADER_LENGTH + BRCM_EVENT_HEADER_LENGTH + BRCM_EVENT_MSG_LENGTH;
                     if (probe_result->sdpcm_brcm_event_oui_match
                        && probe_result->sdpcm_brcm_event_version_valid
                        && brcm_event_msg_read_count <= bytes_after_bdc
                        && brcm_event_msg_read_count <= (uint16_t)sizeof(ethertype_buffer)) {
                        memset(ethertype_buffer, 0, sizeof(ethertype_buffer));
                        if (sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                                               brcm_event_msg_read_count, ethertype_buffer,
                                               (uint32_t)brcm_event_msg_read_count, NULL)) {
                           uint16_t event_msg_offset = (uint16_t)probe_result->sdpcm_bdc_data_offset_bytes
                              + ETHERNET_HEADER_LENGTH + BRCM_EVENT_HEADER_LENGTH;

                           probe_result->sdpcm_brcm_event_msg_version = (uint16_t)(((uint16_t)ethertype_buffer[event_msg_offset + 0u] << 8)
                              | (uint16_t)ethertype_buffer[event_msg_offset + 1u]);
                           probe_result->sdpcm_brcm_event_msg_flags = (uint16_t)(((uint16_t)ethertype_buffer[event_msg_offset + 2u] << 8)
                              | (uint16_t)ethertype_buffer[event_msg_offset + 3u]);
                           probe_result->sdpcm_brcm_event_type = ((uint32_t)ethertype_buffer[event_msg_offset + 4u] << 24)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 5u] << 16)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 6u] << 8)
                              | (uint32_t)ethertype_buffer[event_msg_offset + 7u];
                           probe_result->sdpcm_brcm_event_status = ((uint32_t)ethertype_buffer[event_msg_offset + 8u] << 24)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 9u] << 16)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 10u] << 8)
                              | (uint32_t)ethertype_buffer[event_msg_offset + 11u];
                           probe_result->sdpcm_brcm_event_reason = ((uint32_t)ethertype_buffer[event_msg_offset + 12u] << 24)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 13u] << 16)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 14u] << 8)
                              | (uint32_t)ethertype_buffer[event_msg_offset + 15u];
                           probe_result->sdpcm_brcm_event_auth_type = ((uint32_t)ethertype_buffer[event_msg_offset + 16u] << 24)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 17u] << 16)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 18u] << 8)
                              | (uint32_t)ethertype_buffer[event_msg_offset + 19u];
                           probe_result->sdpcm_brcm_event_datalen = ((uint32_t)ethertype_buffer[event_msg_offset + 20u] << 24)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 21u] << 16)
                              | ((uint32_t)ethertype_buffer[event_msg_offset + 22u] << 8)
                              | (uint32_t)ethertype_buffer[event_msg_offset + 23u];
                           memcpy(probe_result->sdpcm_brcm_event_addr,
                                  &ethertype_buffer[event_msg_offset + BRCM_EVENT_MSG_ADDR_OFFSET],
                                  sizeof(probe_result->sdpcm_brcm_event_addr));
                           memcpy(probe_result->sdpcm_brcm_event_ifname,
                                  &ethertype_buffer[event_msg_offset + BRCM_EVENT_MSG_IFNAME_OFFSET],
                                  BRCM_EVENT_MSG_IFNAME_LENGTH);
                           probe_result->sdpcm_brcm_event_ifname[BRCM_EVENT_MSG_IFNAME_LENGTH] = '\0';
                           probe_result->sdpcm_brcm_event_ifname_truncated = memchr(probe_result->sdpcm_brcm_event_ifname,
                              '\0', BRCM_EVENT_MSG_IFNAME_LENGTH) == NULL;
                           probe_result->sdpcm_brcm_event_ifidx = ethertype_buffer[event_msg_offset + BRCM_EVENT_MSG_IFIDX_OFFSET];
                           probe_result->sdpcm_brcm_event_bsscfgidx = ethertype_buffer[event_msg_offset + BRCM_EVENT_MSG_BSSCFGIDX_OFFSET];
                           probe_result->sdpcm_brcm_event_payload_bytes_available = (uint32_t)bytes_after_bdc
                              - (uint32_t)(probe_result->sdpcm_bdc_data_offset_bytes + ETHERNET_HEADER_LENGTH + BRCM_EVENT_HEADER_LENGTH + BRCM_EVENT_MSG_LENGTH);
                           probe_result->sdpcm_brcm_event_msg_datalen_sane = probe_result->sdpcm_brcm_event_datalen
                              <= probe_result->sdpcm_brcm_event_payload_bytes_available;
                           if (probe_result->sdpcm_brcm_event_count == 0u) {
                              probe_result->sdpcm_brcm_event_first_type = probe_result->sdpcm_brcm_event_type;
                              probe_result->sdpcm_brcm_event_first_status = probe_result->sdpcm_brcm_event_status;
                              probe_result->sdpcm_brcm_event_first_reason = probe_result->sdpcm_brcm_event_reason;
                           }
                           if (probe_result->sdpcm_brcm_event_count != 0xffu)
                              ++probe_result->sdpcm_brcm_event_count;
                           probe_result->sdpcm_brcm_event_msg_probe_success = true;
                        }
                     }
                  }
               }
            }
         }
      }
   } else if (probe_result->sdpcm_channel == SDPCM_CONTROL_CHANNEL) {
      uint8_t control_payload_word[4];
      uint8_t control_payload_word1[4];

      probe_result->sdpcm_cdc_cmd_prefix = (uint32_t)probe_result->sdpcm_post_header_prefix0
         | ((uint32_t)probe_result->sdpcm_post_header_prefix1 << 8)
         | ((uint32_t)probe_result->sdpcm_post_header_prefix2 << 16)
         | ((uint32_t)probe_result->sdpcm_post_header_prefix3 << 24);
      probe_result->sdpcm_cdc_length = (uint32_t)prefix_buffer[4]
         | ((uint32_t)prefix_buffer[5] << 8)
         | ((uint32_t)prefix_buffer[6] << 16)
         | ((uint32_t)prefix_buffer[7] << 24);
      probe_result->sdpcm_cdc_flags = (uint32_t)prefix_buffer[8]
         | ((uint32_t)prefix_buffer[9] << 8)
         | ((uint32_t)prefix_buffer[10] << 16)
         | ((uint32_t)prefix_buffer[11] << 24);
      probe_result->sdpcm_cdc_status = (uint32_t)prefix_buffer[12]
         | ((uint32_t)prefix_buffer[13] << 8)
         | ((uint32_t)prefix_buffer[14] << 16)
         | ((uint32_t)prefix_buffer[15] << 24);
      probe_result->sdpcm_cdc_request_length = (uint16_t)(probe_result->sdpcm_cdc_length >> 16);
      probe_result->sdpcm_cdc_response_length = (uint16_t)(probe_result->sdpcm_cdc_length & 0xffffu);
      probe_result->sdpcm_cdc_payload_bytes_available = (uint16_t)(probe_result->frame_header_size
         - probe_result->sdpcm_expected_header_length - CDC_HEADER_LENGTH);
      probe_result->sdpcm_cdc_response_length_sane = probe_result->sdpcm_cdc_response_length
         <= probe_result->sdpcm_cdc_payload_bytes_available;
      probe_result->sdpcm_cdc_interface = (uint8_t)((probe_result->sdpcm_cdc_flags & CDCF_IOC_IF_MASK) >> CDCF_IOC_IF_SHIFT);
      probe_result->sdpcm_cdc_request_id = (uint16_t)((probe_result->sdpcm_cdc_flags & CDCF_IOC_ID_MASK) >> CDCF_IOC_ID_SHIFT);
      probe_result->sdpcm_cdc_header_probe_success = true;
      probe_result->sdpcm_cdc_prefix_decoded = true;

      if (probe_result->sdpcm_cdc_response_length_sane
         && probe_result->sdpcm_cdc_payload_bytes_available >= sizeof(control_payload_word)) {
         probe_result->sdpcm_cdc_payload_word0_probe_attempted = true;
         memset(control_payload_word, 0, sizeof(control_payload_word));
         if (sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                                (uint16_t)sizeof(control_payload_word), control_payload_word,
                                (uint32_t)sizeof(control_payload_word), NULL)) {
            probe_result->sdpcm_cdc_payload_word0 = (uint32_t)control_payload_word[0]
               | ((uint32_t)control_payload_word[1] << 8)
               | ((uint32_t)control_payload_word[2] << 16)
               | ((uint32_t)control_payload_word[3] << 24);
            probe_result->sdpcm_cdc_payload_word0_probe_success = true;
            if (probe_result->sdpcm_cdc_cmd_prefix == WLC_GET_MAGIC) {
               probe_result->sdpcm_cdc_payload_word0_magic_valid =
                  probe_result->sdpcm_cdc_payload_word0 == WLC_IOCTL_MAGIC;
            }
         }
      }

      if (probe_result->sdpcm_cdc_response_length_sane
         && probe_result->sdpcm_cdc_payload_bytes_available >= (sizeof(control_payload_word) + sizeof(control_payload_word1))) {
         probe_result->sdpcm_cdc_payload_word1_probe_attempted = true;
         memset(control_payload_word1, 0, sizeof(control_payload_word1));
         if (sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                                (uint16_t)sizeof(control_payload_word1), control_payload_word1,
                                (uint32_t)sizeof(control_payload_word1), NULL)) {
            probe_result->sdpcm_cdc_payload_word1 = (uint32_t)control_payload_word1[0]
               | ((uint32_t)control_payload_word1[1] << 8)
               | ((uint32_t)control_payload_word1[2] << 16)
               | ((uint32_t)control_payload_word1[3] << 24);
            probe_result->sdpcm_cdc_payload_word1_probe_success = true;
         }
      }
   }

   return true;
}

static bool sdio_probe_read_frame_header(sdio_host_t *dev,
                                         sdio_probe_result_t *probe_result)
{
   uint16_t hwtag[2];
   uint8_t sdpcm_header[8];

   if (probe_result == NULL)
      return false;

   memset(hwtag, 0, sizeof(hwtag));
   memset(sdpcm_header, 0, sizeof(sdpcm_header));
   probe_result->frame_header_probe_attempted = true;

   if (!sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                           (uint16_t)sizeof(hwtag), hwtag,
                           (uint32_t)sizeof(hwtag), NULL)) {
      return false;
   }

   probe_result->frame_header_size = hwtag[0];
   probe_result->frame_header_size_complement = hwtag[1];
   probe_result->frame_header_probe_success = true;

   if ((hwtag[0] == 0u && hwtag[1] == 0u))
      return true;

   probe_result->frame_header_valid = (uint16_t)(hwtag[0] ^ hwtag[1]) == (uint16_t)0xffffu;
   if (probe_result->frame_header_valid && hwtag[0] >= 12u) {
      if (sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                             (uint16_t)sizeof(sdpcm_header), sdpcm_header,
                             (uint32_t)sizeof(sdpcm_header), NULL)) {
         probe_result->sdpcm_sequence = sdpcm_header[0];
         probe_result->sdpcm_channel_and_flags = sdpcm_header[1];
         probe_result->sdpcm_channel = (uint8_t)(sdpcm_header[1] & SDPCM_CHANNEL_MASK);
         probe_result->sdpcm_next_length = sdpcm_header[2];
         probe_result->sdpcm_header_length = sdpcm_header[3];
         probe_result->sdpcm_wireless_flow_control = sdpcm_header[4];
         probe_result->sdpcm_bus_data_credit = sdpcm_header[5];
         probe_result->sdpcm_channel_known = probe_result->sdpcm_channel == SDPCM_CONTROL_CHANNEL
            || probe_result->sdpcm_channel == SDPCM_EVENT_CHANNEL
            || probe_result->sdpcm_channel == SDPCM_DATA_CHANNEL;
         if (probe_result->sdpcm_channel == SDPCM_DATA_CHANNEL)
            probe_result->sdpcm_expected_header_length = SDPCM_DATA_HEADER_LENGTH;
         else
            probe_result->sdpcm_expected_header_length = SDPCM_CONTROL_EVENT_HEADER_LENGTH;
         probe_result->sdpcm_header_length_expected = probe_result->sdpcm_channel_known
            && probe_result->sdpcm_header_length == probe_result->sdpcm_expected_header_length;
         probe_result->sdpcm_header_sane = probe_result->sdpcm_channel_known
            && probe_result->sdpcm_header_length >= 12u
            && probe_result->sdpcm_header_length <= hwtag[0];
         probe_result->sdpcm_header_read_success = true;
         (void) sdio_probe_read_post_header_prefix(dev, probe_result);
      }
   }

   return sdio_probe_abort_function2_read(dev, probe_result);
}

bool sdio_probe_card(bool tx_control_probe_enabled,
                     wifi_sdio_tx_probe_command_t tx_control_probe_command,
                     sdio_probe_result_t *result)
{
   sdio_host_t device;
   const wifi_config_t *config = wifi_get_config();
   uint8_t revision = 0;
   uint8_t io_enable = 0;
   uint8_t io_ready = 0;
   uint8_t bus_interface_control = 0;
   uint8_t rx_sweep_limit = 4u;

   if (config != NULL && config->sdio_rx_sweep_limit != 0u)
      rx_sweep_limit = config->sdio_rx_sweep_limit;

   memset(&g_sdio_probe_result, 0, sizeof(g_sdio_probe_result));
   g_sdio_probe_result.attempted = true;
   sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                    sdio_tx_probe_template_command(tx_control_probe_command));

   if (sdio_host_open(&device) != 0) {
      if (result != NULL)
         *result = g_sdio_probe_result;
      return false;
   }

   if (!sdio_card_identify(&device, &g_sdio_probe_result, false)) {
      if (result != NULL)
         *result = g_sdio_probe_result;
      return false;
   }

   if (sdio_probe_read_byte(&device, SDIO_CCCR_CCCR_SDIO_REV, &revision)
      && sdio_probe_read_byte(&device, SDIO_CCCR_IO_ENABLE, &io_enable)
      && sdio_probe_read_byte(&device, SDIO_CCCR_IO_READY, &io_ready)
      && sdio_probe_read_byte(&device, SDIO_CCCR_BUS_INTERFACE_CONTROL, &bus_interface_control)) {
      g_sdio_probe_result.cccr_read_success = true;
      g_sdio_probe_result.cccr_revision = (uint8_t)((revision >> 4) & 0x0fu);
      g_sdio_probe_result.sd_revision = (uint8_t)(revision & 0x0fu);
      g_sdio_probe_result.io_enable = io_enable;
      g_sdio_probe_result.io_ready = io_ready;
      g_sdio_probe_result.bus_interface_control = bus_interface_control;
      (void) sdio_probe_enable_functions(&device, &g_sdio_probe_result);
      (void) sdio_probe_request_alp_clock(&device, &g_sdio_probe_result);
      (void) sdio_probe_read_power_registers(&device, &g_sdio_probe_result);
      (void) sdio_probe_wake_with_kso(&device, &g_sdio_probe_result);
      (void) sdio_probe_read_mailbox_registers(&device, &g_sdio_probe_result);
      (void) sdio_probe_ack_interrupts(&device, &g_sdio_probe_result);
      (void) sdio_probe_write_interrupt_mask(&device, &g_sdio_probe_result);
      (void) sdio_probe_read_function2_registers(&device, &g_sdio_probe_result);
      if (tx_control_probe_enabled) {
         (void) sdio_probe_send_tx_control_template(&device, &g_sdio_probe_result,
                                                    tx_control_probe_command);
         (void) sdio_probe_read_tx_post_state(&device, &g_sdio_probe_result);
         (void) sdio_probe_sweep_rx_frames(&device, &g_sdio_probe_result, rx_sweep_limit);
      } else {
         (void) sdio_probe_read_frame_header(&device, &g_sdio_probe_result);
      }

      if (sdio_backplane_read_u32(&device, CYW43_CHIPCOMMON_BASE, &g_sdio_probe_result.chipcommon_id_register)) {
         g_sdio_probe_result.backplane_probe_success = true;
         g_sdio_probe_result.chip_id = (uint16_t)(g_sdio_probe_result.chipcommon_id_register & 0xffffu);
         g_sdio_probe_result.chip_revision = (uint8_t)((g_sdio_probe_result.chipcommon_id_register >> 16) & 0x0fu);
      }
   }

   if (result != NULL)
      *result = g_sdio_probe_result;

   return true;
}

const sdio_probe_result_t *sdio_get_probe_result(void)
{
   return &g_sdio_probe_result;
}

static bool sdio_runtime_finalize_error(const char *fallback_message)
{
   if (g_runtime_error[0] == '\0' && fallback_message != NULL)
      sdio_runtime_set_error(fallback_message);
   g_runtime_stage = SDIO_RUNTIME_STAGE_ERROR;
   return false;
}

bool sdio_runtime_start(void)
{
   /* Reset state and arm the bring-up state machine. The actual work
      runs in sdio_runtime_tick() which is called from the main poll
      loop, so this returns immediately. */
   memset(&g_runtime_device, 0, sizeof(g_runtime_device));
   memset(&g_sdio_probe_result, 0, sizeof(g_sdio_probe_result));
   g_sdio_probe_result.attempted = true;
   g_runtime_started = false;
   g_runtime_link_up = false;
   g_runtime_tx_frame_count = 0u;
   g_runtime_rx_frame_count = 0u;
   g_runtime_data_sequence = 0u;
   g_runtime_emulator_mode = false;
   g_runtime_identify_started = false;
   g_runtime_identify_attempt = 0u;
   g_runtime_identify_deadline_us = 0u;
   memset(&g_runtime_alp_wait, 0, sizeof(g_runtime_alp_wait));
   memset(&g_runtime_kso_wait, 0, sizeof(g_runtime_kso_wait));
   sdio_runtime_boot_reset_state();
   sdio_runtime_set_error(NULL);
   sdio_debug_log("runtime start");

   /* Kick host-open immediately so WL_REG_ON settle can overlap with
      firmware/NVRAM file loading in the WiFi boot stage. */
   if (sdio_host_open_start(&g_runtime_device) != 0)
      return sdio_runtime_finalize_error("Failed to start WiFi SDIO host open");

   g_runtime_stage = SDIO_RUNTIME_STAGE_OPEN_HOST;
   return true;
}

bool sdio_runtime_tick(void)
{
   const wifi_config_t *config;

   switch (g_runtime_stage) {
      case SDIO_RUNTIME_STAGE_OPEN_HOST:
      {
         int open_result;

         open_result = sdio_host_open_poll(&g_runtime_device);
         if (open_result < 0) {
            const char *host_error = sdio_host_last_error();

            if (host_error != NULL && host_error[0] != '\0')
               sdio_runtime_set_error(host_error);
            return sdio_runtime_finalize_error("Failed to open WiFi SDIO host");
         }

         if (open_result == 0)
            return true;

         g_runtime_stage = SDIO_RUNTIME_STAGE_IDENTIFY_CARD;
         return true;
      }

      case SDIO_RUNTIME_STAGE_IDENTIFY_CARD:
      {
         int identify_result = sdio_runtime_card_identify_step(&g_runtime_device,
                                                               &g_sdio_probe_result);

         if (identify_result < 0)
            return sdio_runtime_finalize_error(NULL);
         if (identify_result == 0)
            return true;
         g_runtime_stage = SDIO_RUNTIME_STAGE_READ_CCCR;
         return true;
      }

      case SDIO_RUNTIME_STAGE_READ_CCCR:
         if (!sdio_probe_read_byte(&g_runtime_device, SDIO_CCCR_CCCR_SDIO_REV,
                                   &g_sdio_probe_result.cccr_revision)
            || !sdio_probe_read_byte(&g_runtime_device, SDIO_CCCR_IO_ENABLE,
                                     &g_sdio_probe_result.io_enable)
            || !sdio_probe_read_byte(&g_runtime_device, SDIO_CCCR_IO_READY,
                                     &g_sdio_probe_result.io_ready)
            || !sdio_probe_read_byte(&g_runtime_device, SDIO_CCCR_BUS_INTERFACE_CONTROL,
                                     &g_sdio_probe_result.bus_interface_control)) {
            return sdio_runtime_finalize_error("WiFi SDIO CCCR read failed");
         }
         g_runtime_stage = SDIO_RUNTIME_STAGE_ENABLE_FUNCTIONS;
         return true;

      case SDIO_RUNTIME_STAGE_ENABLE_FUNCTIONS:
         if (!sdio_probe_enable_functions(&g_runtime_device, &g_sdio_probe_result))
            return sdio_runtime_finalize_error("WiFi SDIO function enable failed");
         g_runtime_stage = SDIO_RUNTIME_STAGE_REQUEST_ALP;
         return true;

      case SDIO_RUNTIME_STAGE_REQUEST_ALP:
      {
         int alp_result = sdio_runtime_request_alp_clock_step(&g_runtime_device,
                                                              &g_sdio_probe_result);

         if (alp_result < 0)
            return sdio_runtime_finalize_error("WiFi SDIO ALP clock request failed");
         if (alp_result == 0)
            return true;
         g_runtime_stage = SDIO_RUNTIME_STAGE_READ_POWER;
         return true;
      }

      case SDIO_RUNTIME_STAGE_READ_POWER:
         if (!sdio_probe_read_power_registers(&g_runtime_device, &g_sdio_probe_result))
            return sdio_runtime_finalize_error("WiFi SDIO power register read failed");
         g_runtime_stage = SDIO_RUNTIME_STAGE_WAKE_KSO;
         return true;

      case SDIO_RUNTIME_STAGE_WAKE_KSO:
      {
         int kso_result = sdio_runtime_wake_with_kso_step(&g_runtime_device,
                                                          &g_sdio_probe_result);

         if (kso_result < 0)
            return sdio_runtime_finalize_error("WiFi SDIO KSO wake failed");
         if (kso_result == 0)
            return true;
         if (sdio_host_set_clock(&g_runtime_device, SDIO_RUNTIME_HIGH_CLOCK_HZ, NULL) != 0)
            return sdio_runtime_finalize_error("WiFi SDIO high-speed clock switch failed");
         sdio_debug_log("controller setup complete io_enable=0x%02x io_ready=0x%02x block1=%u block2=%u",
                        (unsigned int)g_sdio_probe_result.configured_io_enable,
                        (unsigned int)g_sdio_probe_result.configured_io_ready,
                        (unsigned int)g_sdio_probe_result.function1_block_size,
                        (unsigned int)g_sdio_probe_result.function2_block_size);
         g_runtime_stage = SDIO_RUNTIME_STAGE_BOOT_FIRMWARE;
         return true;
      }

      case SDIO_RUNTIME_STAGE_BOOT_FIRMWARE:
      {
         int fw_step_result = sdio_runtime_boot_firmware_step(&g_runtime_device,
                                                              &g_sdio_probe_result);

         if (fw_step_result < 0)
            return sdio_runtime_finalize_error("WiFi firmware boot failed");
         if (fw_step_result == 0)
            return true;
         g_runtime_stage = SDIO_RUNTIME_STAGE_READ_MAILBOX;
         return true;
      }

      case SDIO_RUNTIME_STAGE_READ_MAILBOX:
         if (!sdio_probe_read_mailbox_registers(&g_runtime_device, &g_sdio_probe_result))
            return sdio_runtime_finalize_error("WiFi SDIO mailbox read failed");
         g_runtime_stage = SDIO_RUNTIME_STAGE_ACK_INTERRUPTS;
         return true;

      case SDIO_RUNTIME_STAGE_ACK_INTERRUPTS:
         if (!sdio_probe_ack_interrupts(&g_runtime_device, &g_sdio_probe_result))
            return sdio_runtime_finalize_error("WiFi SDIO interrupt ack failed");
         g_runtime_stage = SDIO_RUNTIME_STAGE_WRITE_INTR_MASK;
         return true;

      case SDIO_RUNTIME_STAGE_WRITE_INTR_MASK:
         if (!sdio_probe_write_interrupt_mask(&g_runtime_device, &g_sdio_probe_result))
            return sdio_runtime_finalize_error("WiFi SDIO interrupt mask write failed");

         sdio_debug_log("== STAGE_PREPARE_JOIN: firmware booted chip=%u rev=%u sdio_core=0x%08lx ==",
                        (unsigned int)g_sdio_probe_result.chip_id,
                        (unsigned int)g_sdio_probe_result.chip_revision,
                        (unsigned long)g_sdio_probe_result.sdio_core_base);

         g_runtime_started = true;
         g_sdio_probe_result.success = true;
         g_runtime_stage = SDIO_RUNTIME_STAGE_PREPARE_JOIN;
         return true;

      case SDIO_RUNTIME_STAGE_PREPARE_JOIN:
         config = wifi_get_config();
         if (config == NULL || config->ssid[0] == '\0') {
            /* No SSID configured - stop here, leave the runtime up so
               other code (lwip, webserver) can still query state. */
            g_runtime_stage = SDIO_RUNTIME_STAGE_DONE;
            sdio_debug_log("== STAGE_DONE: runtime ready (no SSID configured) ==");
            return false;
         }

         if (g_runtime_emulator_mode) {
            g_runtime_stage = SDIO_RUNTIME_STAGE_DONE;
            sdio_debug_log("== STAGE_DONE: emulator mode, skipping join burst to keep polling responsive ==");
            return false;
         }

         sdio_debug_log("== STAGE_SWEEP_RX: starting join sequence ==");
         /* Send all 12 join commands (UP, INFRA, SUP_WPA, ..., JOIN) to
            the firmware via the 12-step control channel sequence. */
         sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                          WIFI_SDIO_TX_PROBE_COMMAND_JOIN);
         if (!sdio_probe_send_tx_control_template(&g_runtime_device,
                                                   &g_sdio_probe_result,
                                                   WIFI_SDIO_TX_PROBE_COMMAND_JOIN)) {
            sdio_debug_log("join command sequence failed at step %u/%u",
                           (unsigned int)g_sdio_probe_result.tx_control_probe_steps_completed,
                           (unsigned int)g_sdio_probe_result.tx_control_probe_steps_requested);
         } else {
            sdio_debug_log("join command sequence sent (%u steps)",
                           (unsigned int)g_sdio_probe_result.tx_control_probe_steps_completed);
         }
         g_runtime_stage = SDIO_RUNTIME_STAGE_SWEEP_RX;
         return true;

      case SDIO_RUNTIME_STAGE_SWEEP_RX:
         sdio_debug_log("== ENTERING SWEEP_RX ==");
         config = wifi_get_config();
         {
            uint8_t sweep_limit =
               (config != NULL && config->sdio_rx_sweep_limit != 0u)
                  ? config->sdio_rx_sweep_limit : 4u;
            (void)sdio_probe_sweep_rx_frames(&g_runtime_device, &g_sdio_probe_result,
                                             sweep_limit);
         }
         (void)sdio_probe_read_tx_post_state(&g_runtime_device, &g_sdio_probe_result);

         if (g_sdio_probe_result.tx_control_probe_steps_requested > 0u) {
            sdio_debug_log("join sequence complete: %u/%u steps, last_cmd=0x%08lx, result: event_type=%lu event_status=%lu",
                           (unsigned int)g_sdio_probe_result.tx_control_probe_steps_completed,
                           (unsigned int)g_sdio_probe_result.tx_control_probe_steps_requested,
                           (unsigned long)g_sdio_probe_result.tx_control_probe_last_command,
                           (unsigned long)g_sdio_probe_result.sdpcm_brcm_event_type,
                           (unsigned long)g_sdio_probe_result.sdpcm_brcm_event_status);
         }

         sdio_debug_log("== EXITING SWEEP_RX -> STAGE_DONE: link_up=%u ==", 
                        sdio_event_is_link_up(g_sdio_probe_result.sdpcm_brcm_event_type,
                                              g_sdio_probe_result.sdpcm_brcm_event_status,
                                              g_sdio_probe_result.sdpcm_brcm_event_reason)
                           ? 1u : 0u);

         /* WLC_E_LINK = 16, status = 0, reason = 0 indicates the chip
            successfully associated. Anything else leaves the link down
            for now; the lwip layer will keep polling and the next
            sweep ticks will pick up async events. */
          if (sdio_event_is_link_up(g_sdio_probe_result.sdpcm_brcm_event_type,
                              g_sdio_probe_result.sdpcm_brcm_event_status,
                              g_sdio_probe_result.sdpcm_brcm_event_reason)) {
            g_runtime_link_up = true;
         }

         g_runtime_stage = SDIO_RUNTIME_STAGE_DONE;
         return false;

      case SDIO_RUNTIME_STAGE_IDLE:
      case SDIO_RUNTIME_STAGE_DONE:
      case SDIO_RUNTIME_STAGE_ERROR:
      default:
         return false;
   }
}

bool sdio_runtime_started(void)
{
   return g_runtime_started;
}

bool sdio_runtime_link_is_up(void)
{
   return g_runtime_started && g_runtime_link_up;
}

bool sdio_runtime_send_ethernet_frame(const uint8_t *frame, uint16_t frame_length)
{
   uint8_t tx_frame[SDIO_RUNTIME_MAX_FRAME_SIZE];
   uint16_t total_length;

   if (!g_runtime_started || frame == NULL || frame_length == 0u) {
      sdio_runtime_set_error("WiFi SDIO runtime is not ready for transmit");
      return false;
   }

   total_length = (uint16_t)(18u + frame_length);
   if (total_length > (uint16_t)sizeof(tx_frame)) {
      sdio_runtime_set_error("Ethernet frame exceeds SDIO transmit buffer");
      return false;
   }

   memset(tx_frame, 0, sizeof(tx_frame));
   sdio_store_u16_le(&tx_frame[0], total_length);
   sdio_store_u16_le(&tx_frame[2], (uint16_t)~total_length);
   tx_frame[4] = g_runtime_data_sequence++;
   tx_frame[5] = SDPCM_DATA_CHANNEL;
   tx_frame[6] = 0u;
   tx_frame[7] = SDPCM_DATA_HEADER_LENGTH;
   tx_frame[8] = 0u;
   tx_frame[9] = 0u;
   tx_frame[14] = (uint8_t)(BDC_PROTOCOL_VERSION << BDC_VERSION_SHIFT);
   memcpy(&tx_frame[18], frame, frame_length);

   if (!sdio_function2_transfer(&g_runtime_device, true, tx_frame, total_length)) {
      sdio_runtime_set_error("Failed to write Ethernet frame over SDIO");
      return false;
   }

   ++g_runtime_tx_frame_count;
   return true;
}

void sdio_runtime_poll_events(void)
{
   uint8_t dummy_frame[SDIO_RUNTIME_MAX_FRAME_SIZE];
   uint16_t dummy_length;
   uint8_t i;

   if (!g_runtime_started)
      return;

   /* Drain any pending BRCM async event frames from the chip (WLC_E_LINK,
      WLC_E_SET_SSID, etc.). sdio_runtime_poll_ethernet_frame processes
      events as a side effect (updating g_runtime_link_up), so we just
      need to drive the read loop without surfacing the frames. */
   for (i = 0u; i < SDIO_RUNTIME_MAX_RX_FRAMES_PER_POLL; ++i) {
      dummy_length = 0u;
      if (!sdio_runtime_poll_ethernet_frame(dummy_frame, sizeof(dummy_frame), &dummy_length))
         break;
   }
}

bool sdio_runtime_poll_ethernet_frame(uint8_t *frame, uint16_t frame_capacity,
                                      uint16_t *frame_length)
{
   uint8_t frame_index;
   uint16_t hwtag[2];

   if (frame_length != NULL)
      *frame_length = 0u;

   if (!g_runtime_started)
      return false;

   /* Hard fail-safe for emulator runs: never issue SDIO I/O from the runtime
      poll path. Emulator transport/register behavior is incomplete and can
      occasionally block on command paths; keeping this branch side-effect-free
      guarantees the main system poll loop remains responsive. */
   if (g_runtime_emulator_mode) {
      static uint32_t s_emu_poll_count;

      if ((s_emu_poll_count % 10000000u) == 0u) {
         sdio_debug_log("poll heartbeat (emu-safe): %lu polls, link_up=%u, rx_frames=%lu",
                        (unsigned long)s_emu_poll_count,
                        (unsigned)(g_runtime_link_up ? 1u : 0u),
                        (unsigned long)g_runtime_rx_frame_count);
      }

      ++s_emu_poll_count;
      return false;
   }

   /* Ack any pending mailbox interrupts once per poll cycle, before reading
      from fn2.  This is the correct order: clear the interrupt line first so
      the firmware can re-assert it when the next frame arrives.
      Note: we do NOT gate on READ_FRAME_BC here because the BCM43430 does not
      reliably update that register.  Instead we read 4 bytes directly from fn2
      (cyw43-driver approach) and treat 0x0000/0x0000 as "no frame". */
   {
      static uint32_t s_poll_count;

      uint32_t int_status = 0u;
      if (sdio_backplane_read_u32_timeout(&g_runtime_device,
                        CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_STATUS_OFFSET,
                        SDIO_RUNTIME_POLL_TIMEOUT_US,
                        &int_status)) {
         /* Log every poll where INT_STATUS is non-zero (means
            something actually happened) plus the first few empty
            polls for orientation. The chip is supposed to set
            FRAME_IND for every event/data frame; if we see only one
            FRAME_IND right after boot and then nothing for seconds,
            the radio is silent for real - not just rate-limited
            polling on our side. */
         if (int_status != 0u || s_poll_count < 4u) {
            sdio_debug_log("poll[%lu] int=0x%08lx ack=0x%08lx",
                           (unsigned long)s_poll_count,
                           (unsigned long)int_status,
                           (unsigned long)int_status);
         }
         if (int_status != 0u) {
            if (!g_runtime_emulator_mode) {
               /* Clear ALL INT_STATUS bits (write-1-to-clear), not just the
                  0xF0 mailbox bits.  Bits such as I_SBINT (0x20000000) and
                  I_HMB_DATA (0x00020000) must also be acked; leaving them
                  set can cause the firmware to stall before sending events.
                  This matches the cyw43-driver approach of writing back the
                  full int_status value. */
               (void)sdio_backplane_write_u32_timeout(&g_runtime_device,
                                                      CYW43_SDIO_CORE_BASE + SDIO_CORE_INT_STATUS_OFFSET,
                                                      int_status,
                                                      SDIO_RUNTIME_POLL_TIMEOUT_US);
               /* Per Circle ether4330.c intwait(): read Hostmboxdata and send
                  SMB_INT_ACK only on MailboxInt (bit 7 = 0x80).  Sending the
                  ACK spuriously on FrameInt-only events confuses the firmware. */
               if (int_status & 0x80u) {
                  uint32_t hmb_data = 0u;
                  (void)sdio_backplane_read_u32_timeout(&g_runtime_device,
                                                        CYW43_SDIO_CORE_BASE + SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET,
                                                        SDIO_RUNTIME_POLL_TIMEOUT_US,
                                                        &hmb_data);
                  (void)sdio_backplane_write_u32_timeout(&g_runtime_device,
                                                         CYW43_SDIO_CORE_BASE + SDIO_CORE_TO_SB_MAILBOX_OFFSET,
                                                         0x00000002u,
                                                         SDIO_RUNTIME_POLL_TIMEOUT_US); /* SMB_INT_ACK */
                  /* HMB is the chip's other notification path (separate
                     from fn2 frames). Log every HMB hit - they carry
                     state notifications like firmware-ready, deep-sleep
                     transitions, and on some chips early association
                     events. */
                  sdio_debug_log("poll[%lu] hmb=0x%08lx",
                                 (unsigned long)s_poll_count,
                                 (unsigned long)hmb_data);
               }
            }
         }
      }
      /* Heartbeat every 1000 polls so it's obvious polling is still
         running even when the chip has gone silent. wifi_lwip_poll
         is registered as a periodic poll so this fires roughly every
         1-2 seconds depending on the main loop tick rate. */
      if ((s_poll_count % 1000u) == 999u) {
         sdio_debug_log("poll heartbeat: %lu polls, link_up=%u, rx_frames=%lu",
                        (unsigned long)(s_poll_count + 1u),
                        (unsigned)(g_runtime_link_up ? 1u : 0u),
                        (unsigned long)g_runtime_rx_frame_count);

         /* While the link is still down, ask the chip directly for its
            current BSSID, chanspec, and MAC.  Responses arrive on the
            next few poll cycles and get logged via the cdc-rsp +
            get-var hex dump path.  Diagnostic value:
              BSSID    - all-zero / NOTASSOCIATED == not joined
              chanspec - shows the chip's home channel
              MAC      - all-zero == NVRAM didn't apply at boot
                         (which would also explain a silent radio,
                          since PA / antenna config lives in NVRAM). */
         if (!g_runtime_link_up && !g_runtime_emulator_mode) {
            sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                             WIFI_SDIO_TX_PROBE_COMMAND_GET_BSSID);
            (void)sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                                     &g_sdio_probe_result,
                                                                     SDIO_RUNTIME_POLL_TIMEOUT_US);
            sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                             WIFI_SDIO_TX_PROBE_COMMAND_GET_CHANSPEC);
            (void)sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                                     &g_sdio_probe_result,
                                                                     SDIO_RUNTIME_POLL_TIMEOUT_US);
            sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                             WIFI_SDIO_TX_PROBE_COMMAND_GET_MAC);
            (void)sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                                     &g_sdio_probe_result,
                                                                     SDIO_RUNTIME_POLL_TIMEOUT_US);
            /* WPA / security setup readbacks - prove that the SETs in
               the join sequence actually committed.  Each GET prints
               via the cdc-rsp + get-var hex dump path.  Expected
               for a WPA2-PSK join:
                  GET_INFRA      -> cdc get-var len=4: 01 00 00 00
                  GET_AUTH       -> cdc get-var len=4: 00 00 00 00 (open)
                  GET_WSEC       -> cdc get-var len=4: 04 00 00 00 (AES)
                  GET_WPA_AUTH   -> cdc get-var len=4: 80 00 00 00 (WPA2_AUTH_PSK)
                  GET_SUP_WPA    -> cdc get-var len=22: 01 00 00 00 .. (sup_wpa=1)
               Anything else points at the corresponding SET being a
               silent no-op. */
            sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                             WIFI_SDIO_TX_PROBE_COMMAND_GET_INFRA);
            (void)sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                                     &g_sdio_probe_result,
                                                                     SDIO_RUNTIME_POLL_TIMEOUT_US);
            sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                             WIFI_SDIO_TX_PROBE_COMMAND_GET_AUTH);
            (void)sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                                     &g_sdio_probe_result,
                                                                     SDIO_RUNTIME_POLL_TIMEOUT_US);
            sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                             WIFI_SDIO_TX_PROBE_COMMAND_GET_WSEC);
            (void)sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                                     &g_sdio_probe_result,
                                                                     SDIO_RUNTIME_POLL_TIMEOUT_US);
            sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                             WIFI_SDIO_TX_PROBE_COMMAND_GET_WPA_AUTH);
            (void)sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                                     &g_sdio_probe_result,
                                                                     SDIO_RUNTIME_POLL_TIMEOUT_US);
            sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                             WIFI_SDIO_TX_PROBE_COMMAND_GET_SUP_WPA);
            (void)sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                                     &g_sdio_probe_result,
                                                                     SDIO_RUNTIME_POLL_TIMEOUT_US);
         }
      }
      ++s_poll_count;
   }

   /* Read up to MAX_RX_FRAMES_PER_POLL frames from fn2. Each iteration peeks
      at the 4-byte SDPCM header: if 0x0000/0x0000 the FIFO is empty and we
      stop, otherwise we pass the already-consumed header to the completion
      path to process the rest of the frame. */
   for (frame_index = 0u; frame_index < SDIO_RUNTIME_MAX_RX_FRAMES_PER_POLL; ++frame_index) {
      hwtag[0] = 0u;
      hwtag[1] = 0u;
      if (!sdio_function2_transfer_timeout(&g_runtime_device, false, (uint8_t *)hwtag,
                         (uint16_t)sizeof(hwtag),
                         SDIO_RUNTIME_POLL_TIMEOUT_US))
         break; /* CMD53 error */

      if (hwtag[0] == 0u && hwtag[1] == 0u)
         break; /* fn2 FIFO empty */

      /* There is a frame: feed the already-consumed 4-byte header forward
         to the completion path which reads the rest and processes it. */
      if (sdio_runtime_complete_read_ethernet_frame_timeout(&g_runtime_device, hwtag,
                                 frame, frame_capacity,
                                 frame_length,
                                 SDIO_RUNTIME_POLL_TIMEOUT_US))
         return true;
      /* else: BRCM event was processed or error; try next frame */
   }

   return false;
}

const char *sdio_runtime_last_error(void)
{
   return g_runtime_error;
}

sdio_runtime_status_t sdio_runtime_get_status(void)
{
   sdio_runtime_status_t status;

   status.started = g_runtime_started;
   status.link_up = g_runtime_link_up;
   status.tx_frames = g_runtime_tx_frame_count;
   status.rx_frames = g_runtime_rx_frame_count;
   return status;
}