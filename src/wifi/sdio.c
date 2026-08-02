#include "sdio.h"

#include "cyw43.h"

#include "../rpi/rpi.h"
#include "../rpi/systimer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDIO_RUNTIME_MAX_FRAME_SIZE 1600u
/* Frame buffers carry a block of headroom beyond the longest frame they will
   accept, because sdio_function2_transfer_timeout() rounds a transfer up to a
   whole number of 512-byte blocks to keep it to a single CMD53.  The padded
   read of a maximum-length frame body is round_up(1596, 512) = 2048 bytes
   placed 4 bytes in, so the buffer must hold 2052. */
#define SDIO_RUNTIME_FRAME_BUFFER_SIZE (SDIO_RUNTIME_MAX_FRAME_SIZE + 512u)
#define SDIO_RUNTIME_MAX_RX_FRAMES_PER_POLL 8u
#define SDIO_RUNTIME_FW_CHUNKS_PER_TICK 8u
#define SDIO_RUNTIME_HIGH_CLOCK_HZ 25000000u
#define SDIO_RUNTIME_HIGH_SPEED_CLOCK_HZ 50000000u
/* Longest the chip's mailbox interrupt may go unserviced while frames keep
   arriving.  Only a floor: normally it is serviced the moment the FIFO
   empties. */
#define SDIO_INT_SERVICE_INTERVAL_US 20000u
/* Resync wait when nothing is arriving to reopen the credit window.  Short,
   because on an idle link the alternative is holding the frame until it is
   discarded.  A resync only clears a stale flow-control stop (it never
   touches the sequence number), so at worst this is one gate re-check per
   interval. */
#define SDPCM_TX_STALL_IDLE_RESYNC_US 20000u
/* How recently a successful transfer counts as proof the bus is still awake.
   Well inside the chip's idle-before-sleep window. */
#define SDIO_BUS_AWAKE_ASSUME_US 2000u
/* Worst-case CPU a single CMD52/CMD53 may busy-wait for completion.  On
   healthy hardware a command completes in microseconds; this cap only
   bites when the chip is unresponsive.  It was 500 ms, long enough to
   stall the cooperative 1 MHz poll loop for half a second on a wedged
   chip.  100 ms still leaves ~100x margin over real completion times
   while bounding how long one tick can hog the CPU; the runtime bring-up
   steps already re-issue across ticks, so a genuine slow path retries on
   the next poll rather than spinning here. */
#define SDIO_COMMAND_TIMEOUT_US 100000u
#define SDIO_RUNTIME_POLL_TIMEOUT_US 5000u
#define SDIO_BACKPLANE_WINDOW_SIZE 0x8000u
#define SDIO_BACKPLANE_TRANSFER_MAX 512u
/* Number of SDIO_BACKPLANE_TRANSFER_MAX-byte firmware chunks pushed per
   sdio_runtime_tick() during the boot download.  32 * 512 B = 16 KiB per
   tick keeps each tick to a couple of milliseconds while still finishing
   a ~400 KB image in well under a second of wall-clock boot time. */
#define SDIO_RUNTIME_BOOT_FW_CHUNKS_PER_TICK 32u
#define SDIO_CORE_SCAN_SIZE 512u

static sdio_probe_result_t g_sdio_probe_result;
static uint16_t g_tx_control_probe_request_id = 1u;
/* THE SDPCM sequence number, singular.  Control, event and data frames all
   travel one bus in one sequence space, and the chip's credit accounting
   follows whatever we stamp into software-header byte 4 - so a second counter
   for a second channel does not give that channel its own numbering, it makes
   the stream jump backwards every time the two are interleaved.  Every frame
   this driver puts on fn2, whatever the channel, takes its number from here. */
static uint8_t g_sdpcm_tx_sequence = 0u;
static sdio_host_t g_runtime_device;
static bool g_runtime_started;
static bool g_runtime_link_up;
/* Set by WLC_E_PSK_SUP/KEYED; gates link-up on a secured network so we do not
   announce a usable link during the 4-way handshake. Meaningless (and never
   set) on an open network, which is why the gate only applies when a
   passphrase is configured. */
static bool g_runtime_psk_keyed;
/* When the association went up, so the keyed-gate above can time out. */
static uint32_t g_runtime_link_up_us;
#define SDIO_PSK_KEYED_GRACE_US 3000000u
static uint32_t g_runtime_tx_frame_count;
static uint32_t g_runtime_rx_frame_count;
/* The chip's WiFi MAC (cur_etheraddr), captured at boot so the lwIP
   netif can use the real address.  The CDC decoder fills
   g_runtime_chip_mac when the GET-VAR response carrying the exact
   request_id we sent for cur_etheraddr arrives - matching by ID
   rather than by a global "currently expecting MAC" flag means any
   other GET_VAR reply that happens to land in the same window is
   not mistaken for the MAC. */
static uint8_t g_runtime_chip_mac[6];
static bool g_runtime_chip_mac_valid;
static bool g_runtime_mac_request_pending;
static uint16_t g_runtime_mac_request_id;
/* On-demand signal-strength (RSSI) read, driven only when the /status
   page is viewed.  The value is captured by the CDC decoder from a
   WLC_GET_RSSI reply, matched by request_id exactly like the MAC.  The
   query itself is a small send-settle-drain state machine run from
   sdio_runtime_rssi_poll() on the cooperative poll path - never from the
   TCP callback - so it stays off the RX-drain path. */
static int32_t g_runtime_rssi;
static bool g_runtime_rssi_valid;
/* WLC_GET_PM readback: what the chip says its power-save mode actually is.
   0 = off (what we ask for), 1 = PM_MAX, 2 = PM_FAST. */
static int32_t g_runtime_pm_value = -1;
static bool g_runtime_pm_value_valid;
static bool g_runtime_pm_query_wanted;
static bool g_runtime_pm_query_sent;
static bool g_runtime_pm_request_pending;
static uint16_t g_runtime_pm_request_id;
static uint32_t g_runtime_pm_deadline_us;
static bool g_runtime_rssi_request_pending;
static uint16_t g_runtime_rssi_request_id;
static bool g_runtime_rssi_query_wanted;
/* Chip-side packet counters, fetched the same way as the RSSI.  Kept so a
   loss episode can be attributed: the chip counts frames that actually
   arrived over the air, so rx_good moving while our own frame count does not
   means we dropped them, and rx_good not moving at all means they never
   arrived. */
static uint32_t g_runtime_pktcnt[5];
static bool g_runtime_pktcnt_valid;
static bool g_runtime_pktcnt_query_wanted;
static bool g_runtime_pktcnt_step_sent;
static bool g_runtime_pktcnt_request_pending;
static uint16_t g_runtime_pktcnt_request_id;
static uint32_t g_runtime_pktcnt_deadline_us;

/* WLC_GET_RATE one-shot, same shape as RSSI/PKTCNTS above. */
static int32_t g_runtime_rate_500kbps;
static bool g_runtime_rate_valid;
static bool g_runtime_rate_query_wanted;
static bool g_runtime_rate_step_sent;
static bool g_runtime_rate_request_pending;
static uint16_t g_runtime_rate_request_id;
static uint32_t g_runtime_rate_deadline_us;
static bool g_runtime_rssi_step_sent;
static uint32_t g_runtime_rssi_step_deadline_us;
/* MAC the caller (wifi.c) wants the chip to transmit with.  Pushed
   into the chip via WLC_SET_VAR("cur_etheraddr", mac) during the
   per-tick SET_MAC stage.  If invalid, SET_MAC is skipped and the
   chip keeps its factory OTP MAC.
   *_request_pending / *_request_id mirror the GET_MAC mechanism so
   the CDC decoder can match the SET reply by exact request_id and
   surface the IOCTL status - silent firmware rejection of a
   cur_etheraddr SET used to leave the chip with its OTP MAC while
   the boot log claimed the override took. */
static uint8_t g_runtime_desired_mac[6];
static bool g_runtime_desired_mac_valid;
static bool g_runtime_set_mac_request_pending;
static uint16_t g_runtime_set_mac_request_id;
static bool g_runtime_set_mac_ack_seen;
static uint32_t g_runtime_set_mac_ack_status;
static char g_runtime_error[96];
/* The chip's SDPCM transmit credit window, refreshed from the software header
   of every received frame.  _valid keeps the pre-first-frame behaviour
   unchanged rather than refusing to transmit before the chip has spoken. */
static uint8_t g_runtime_max_seq;
static uint8_t g_runtime_wlan_flow_control;
static bool g_runtime_max_seq_valid;
/* Credit-window stall tracking, for the desync recovery in
   sdio_runtime_send_ethernet_frame(). */
static bool g_runtime_tx_stalled;
static uint32_t g_runtime_tx_stall_since_us;
/* When ANY frame last arrived on ANY channel (data, control reply, event) -
   stamped beside the max_seq refresh, never zero once stamped.  The 20 ms
   idle-tier resync may only fire if nothing has arrived since the stall
   began: an RX frame refreshes max_seq, so traffic arriving while the window
   stays shut is genuine flow control, and rebasing the sequence would desync
   a live transfer.  Measured: with refused TCP retried at main-loop rate,
   "FIFO momentarily empty" was true often enough that the idle resync fired
   MID-DOWNLOAD and collapsed throughput to zero. */
static uint32_t g_runtime_last_any_rx_us;
/* When the credit window first refused a frame and has not carried one
   since.  Unlike tx_stall_since_us this is NOT reset by a resync probe -
   only a send the bus actually accepted clears it - so it measures how long
   transmit has been genuinely dead.  The rejoin logic watches it: RX-silence
   alone never fires when the chip keeps receiving happily, which is exactly
   the observed wedge state (TX dead, RX alive, power cycle needed). */
static uint32_t g_runtime_tx_shut_since_us;
static uint32_t g_runtime_tx_resync_count;
/* Association retries since boot - see sdio_runtime_rejoin_start(). */
static uint32_t g_runtime_rejoin_count;
/* Set once the 4-bit data bus has been switched to and verified. */
static bool g_runtime_bus_four_bit;
/* Set once the bus has been verified running at 50 MHz high speed. */
static bool g_runtime_bus_high_speed;
/* Set the first time a WLC_E_LINK arrives with its LINK flag set, which proves
   this firmware populates the field - see sdio_event_is_link_down. */
static bool g_runtime_link_flag_trusted;
/* When the mailbox interrupt was last serviced, and whether the last poll
   found the fn2 FIFO empty - together these keep the INT_STATUS exchange out
   of the hot path without letting it starve.  See poll_ethernet_frame. */
static uint32_t g_runtime_int_service_us;
/* When a frame last actually moved over the bus.  Deliberately NOT stamped by
   the 4-byte FIFO peek: an idle link peeks every millisecond and every peek
   succeeds, so stamping there re-armed the window on each pass and the wake
   handshake below it never ran again - the check disabled itself precisely
   when it mattered.  Worse, a peek at a sleeping interface returns zeros,
   which is indistinguishable from "FIFO empty", so the driver could go blind
   with frames queued.  Only real traffic counts as proof of wakefulness. */
static uint32_t g_runtime_bus_active_us;
/* When a frame was last received from the chip.  A link that has silently
   stopped carrying traffic looks identical to a healthy one from every
   status the driver has - see sdio_runtime_rx_idle_us(). */
static uint32_t g_runtime_last_rx_us;
static bool g_runtime_fifo_was_empty = true;
/* Gating of the fn2 peek on the chip's in-band SDIO interrupt.  Armed at
   link-up.  g_rx_int_missed is the trust metric: frames the safety sweep found
   while the line was NOT asserted - zero means the signal can be relied on.
   The sweep runs at the old idle poll rate, so an untrusted signal costs
   nothing over the previous behaviour. */
static bool g_rx_int_armed;
static bool g_rx_sweeping;
static uint32_t g_rx_sweep_us;
static uint32_t g_rx_int_skips;
static uint32_t g_rx_sweeps;
static uint32_t g_rx_int_missed;
static uint32_t g_rx_int_high;
/* 10 ms: the sweep is a safety net, not the transport.  Measured with the
   sweep at 1 ms and the gate armed: 1 frame missed out of 3718 - the line
   announces essentially everything, so sweeping at the old 1 ms poll rate
   just spent ~1000 CMD53s a second confirming the FIFO was empty.  A missed
   frame now waits at most 10 ms, and g_rx_int_missed on /status says how
   often that actually happens. */
#define SDIO_RX_SWEEP_INTERVAL_US 10000u
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
static sdio_runtime_wait_state_t g_runtime_enable_wait;

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
   /* CLM download, SET_MAC, QUERY_MAC and the 38-step join each advance
      one step per tick so no single poll callback stalls the main 1 MHz
      loop. */
   SDIO_RUNTIME_STAGE_CLM_DOWNLOAD,
   /* Pushes wifi.c's cached desired MAC (the SoC board OTP MAC) into
      the chip's cur_etheraddr iovar via WLC_SET_VAR.  Skipped when no
      MAC was cached - the chip then keeps its factory OTP MAC.  Must
      run before QUERY_MAC so the readback reflects the new value. */
   SDIO_RUNTIME_STAGE_SET_MAC,
   SDIO_RUNTIME_STAGE_QUERY_MAC,
   SDIO_RUNTIME_STAGE_JOIN,
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
#define SDIO_CCCR_INT_ENABLE 0x04u        /* bit 0 = master, bit n = function n */
#define SDIO_CORE_HOST_INT_MASK_OFFSET 0x24u
/* I_XMTDATA_AVAIL: "the chip has data to transmit to the host" = a frame is
   waiting.  Chosen by measurement (77597-poll intstatus split): it was the
   only bit present when a frame was waiting and absent across every empty
   poll.  I_HMB_FRAME_IND (0x40) is permanently set on this chip and
   I_CHIPACTIVE is on for as long as the chip runs - neither can gate. */
#define SDIO_HOST_INT_MASK_VALUE 0x00800000u   /* I_XMTDATA_AVAIL */
#define SDIO_CCCR_BUS_INTERFACE_CONTROL 0x07u
#define SDIO_CCCR_SPEED_SELECT 0x13u
#define SDIO_CCCR_SPEED_SHS 0x01u      /* card supports high speed */
#define SDIO_CCCR_SPEED_EHS 0x02u      /* enable high speed */
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
/* The CYW43 SDIO core's backplane address is chip-specific: the 43430
   family puts it at CYW43_SDIO_CORE_BASE, but the BCM43455 (Pi 3 B+ / Pi 4)
   reports it at 0x18004000 (0x18002000 is the ARM core there).  The real
   address is found by sdio_backplane_scan_cores and latched below; until
   then we default to the 43430 value for the pre-scan diagnostic probe. */
static uint32_t g_runtime_sdio_core_base = CYW43_SDIO_CORE_BASE;
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
#define TKIP_ENABLED 0x0002u
#define AES_ENABLED 0x0004u
#define WPA_AUTH_DISABLED 0x0000u
#define WPA2_AUTH_PSK 0x0080u
#define WLC_GET_MAGIC 0u
#define WLC_GET_VERSION 1u
#define WLC_UP 2u
#define WLC_SET_INFRA 20u
#define WLC_SET_PM 86u
#define WLC_SET_AUTH 22u
#define WLC_SET_ANTDIV 64u
#define WLC_SET_GMODE 110u
#define WLC_SET_BAND 142u
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
#define WLC_GET_RADIO 37u
#define WLC_GET_RSSI 127u
#define WLC_GET_PM 85u
/* Current transmit rate in units of 500 kbit/s.  The one number that
   separates "the link itself is slow" (rate ~= the observed TCP ceiling,
   an RF problem) from "the link is fast and the host is the bottleneck". */
#define WLC_GET_RATE 12u
/* Returns the chip's own packet counters: rx_good, rx_bad, tx_good, tx_bad,
   rx_ocast_good, five little-endian u32s.  The point of asking is that the
   chip counts what actually arrived over the air, so comparing its rx_good
   against our own received-frame count separates "the frame never arrived"
   from "it arrived and we lost it" - which nothing on the host side can
   distinguish. */
#define WLC_GET_PKTCNTS 137u
#define WLC_PKTCNTS_BYTES 20u
#define WLC_SSID_MAX_LEN 32u
#define WSEC_MAX_PSK_LEN 64u
#define WSEC_PASSPHRASE 0x0001u
#define CYW_EAPOL_KEY_TIMEOUT 5000u
#define MFP_NONE 0u
#define MFP_CAPABLE 1u
#define SDPCM_CONTROL_EVENT_HEADER_LENGTH 12u
#define SDPCM_DATA_HEADER_LENGTH 14u
#define SDPCM_PREFIX_LENGTH 4u

/* How long the chip's transmit credit window may stay shut before it is
   treated as a sequence desync rather than back-pressure.  Genuine credit
   exhaustion clears as fast as the chip drains its queue onto the air -
   milliseconds - so a second of it means the two ends disagree about where
   the window is, and waiting longer only prolongs a dead link. */
#define SDPCM_TX_STALL_RESYNC_US 1000000u
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
#define TX_CONTROL_PROBE_JOIN_COMMAND_COUNT 44u
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
/* Chunked-firmware-download state.  The ~400 KB image is pushed over
   CMD53 in SDIO_BACKPLANE_TRANSFER_MAX byte chunks; doing the whole
   image in one sdio_runtime_tick() call stalls the 1 MHz bus loop for
   tens of milliseconds.  g_runtime_boot_setup_done latches once the
   one-time chip scan/setup (incl. cyw43_select_chip_variant, which
   frees the unused firmware blob and must run exactly once) is complete,
   so the download can resume across ticks without re-running setup;
   g_runtime_boot_fw_offset tracks how far the download has progressed. */
static bool g_runtime_boot_setup_done;
static uint32_t g_runtime_boot_fw_offset;

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
static bool sdio_event_is_link_down(uint32_t event_type, uint32_t event_flags);
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
static int sdio_runtime_enable_functions_step(sdio_host_t *dev,
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
static bool sdio_runtime_wake_bus(sdio_host_t *dev);
static bool sdio_runtime_tx_window_shut(void);

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

/* WLC_E_LINK carries the actual link state in the event's flags word, not in
   status or reason: bit 0 (WLC_EVENT_MSG_LINK) set means the link came up,
   clear means it went away.  Association is reported with it set; losing the
   AP without a deauth - beacon timeout, the AP simply powered off - is
   reported with it clear, and there is no deauth or disassoc to go with it. */
#define WLC_EVENT_MSG_LINK 0x01u

static bool sdio_event_is_link_up(uint32_t event_type,
                                  uint32_t event_status,
                                  uint32_t event_reason)
{
   /* Deliberately NOT gated on the LINK flag.  Doing that broke association
      outright: the boot sweep tests this against a flags word captured by a
      different decoder, which is zero unless that path filled it in, so every
      link-up was rejected and the Pi never got on the network.  Link-down
      below reads flags only where they have just been parsed from the event
      itself, and runs after this, so a flag-clear WLC_E_LINK still ends up
      down. */
   return event_type == 16u && event_status == 0u && event_reason == 0u;
}

/* WLC_E_PSK_SUP with status WLC_SUP_KEYED: the WPA 4-way handshake finished
   and the controlled port is authorised, which is the first moment the AP
   will actually forward our data frames.
   WLC_E_LINK (16) fires earlier, at 802.11 association, and that gap is real:
   every boot captured on 2026-07-25 sent its first DHCP Discover between the
   two events, had it silently dropped, and then waited out lwIP's ~1.8s retry
   before a second Discover succeeded in ~40ms. */
static bool sdio_event_is_psk_keyed(uint32_t event_type, uint32_t event_status)
{
   return event_type == 46u && event_status == 6u;
}

static bool sdio_event_is_link_down(uint32_t event_type, uint32_t event_flags)
{
   /* Deauth and disassoc, in both their forms - and a WLC_E_LINK whose LINK
      flag is clear, which is the only notice given when the AP disappears
      without saying goodbye.  Missing that case left the driver believing it
      was still associated, so the rejoin schedule kept resetting itself and
      the Pi stayed off the network indefinitely.

      The flag rule is only applied once this firmware has been seen to set
      the flag on an association, and that caution is not theoretical: reading
      the flag where it was never populated is what made an earlier attempt at
      this reject every link-up and leave the Pi unable to associate at all.
      Until a flag-set link-up has been observed, this behaves exactly as the
      long-standing code did - deauth and disassoc only. */
   if (event_type == 16u)
      return g_runtime_link_flag_trusted
             && (event_flags & WLC_EVENT_MSG_LINK) == 0u;

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
   /* A powered-down chip holds DAT1 low, which reads as permanently asserted. */
   sdio_host_set_card_interrupt(false);
   g_rx_int_armed = false;
   g_runtime_boot_fw_prepared = false;
   g_runtime_boot_deadline_us = 0u;
   g_runtime_boot_chip_id_register = 0u;
   g_runtime_boot_wait_attempt = 0u;
   g_runtime_boot_setup_done = false;
   g_runtime_boot_fw_offset = 0u;
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

   /* HT_AVAIL only appears once the CYW43's internal CPU is executing the
      downloaded firmware.  If it never appears, the firmware did not start.
      The old code treated a non-zero CSR as "must be an emulator" and
      proceeded anyway - which silently turned a real firmware-boot failure
      into a bogus "firmware-ready" state.  Now this is reported honestly;
      set wifi_emulator=1 in Pi1MHz.cfg to keep the lenient behaviour. */
   if ((clock_csr & CYW43_HT_AVAIL) == 0u) {
      const wifi_config_t *cfg = wifi_get_config();

      if (cfg == NULL || !cfg->allow_emulator_fallback) {
         /* One-line diagnostic: chip silicon variant + the byte
            counts of the loaded firmware / NVRAM blobs.  Most
            HT-timeout failures in practice are a wrong-file install
            for the board, and these four numbers are enough to
            triangulate that against what brcmfmac / Pi-OS ship. */
         sdio_debug_log("CYW43 firmware did not start: HT clock unavailable, CSR=0x%02x",
                        (unsigned int)clock_csr);
         sdio_debug_log("  chip_id=43430 chip_revision=%u socramrev=%u fw_bytes=%lu nvram_bytes=%lu",
                        (unsigned int)g_runtime_boot_chip.chip_revision,
                        (unsigned int)g_runtime_boot_chip.socramrev,
                        (unsigned long)g_cyw43_firmware_length,
                        (unsigned long)g_cyw43_nvram_length);
         sdio_runtime_set_error("CYW43 firmware did not start (HT clock never became available)");
         sdio_runtime_boot_reset_state();
         return -1;
      }
      g_runtime_emulator_mode = true;
      sdio_debug_log("emulator: HT_AVAIL not set but CSR=0x%02x; proceeding anyway (wifi_emulator=1)",
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
      g_runtime_sdio_core_base   = chip->sdregs;
   }

   /* Free the firmware + NVRAM host buffers now they are on the chip,
      but KEEP the CLM blob - it is still needed for the clmload iovar
      download in STAGE_PREPARE_JOIN, after the firmware has booted. */
   cyw43_release_boot_images();
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
   uint32_t now_us;

   if (dev == NULL || probe_result == NULL)
      return -1;

   now_us = RPI_GetSystemTime();

   if (!g_runtime_alp_wait.active) {
      probe_result->clock_probe_attempted = true;

      if (!sdio_function1_read_byte(dev, SDIO_CHIP_CLOCK_CSR, &clock_csr))
         return -1;

      probe_result->chip_clock_csr_initial = clock_csr;
      uint8_t requested_clock_csr = (uint8_t)(clock_csr | SDIO_FORCE_HW_CLKREQ_OFF
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

/* Move card and controller to a 4-bit data bus, reverting cleanly if that
   turns out not to work on this board.

   The bus came up 1-bit and stayed there: CCCR 0x07 was only ever read, and
   the controller's width bit is explicitly cleared during open.  DAT1-3 are
   already muxed and pulled up by the pin setup, and the chip supports 4-bit,
   so three quarters of the available bandwidth was simply going unused - at
   25 MHz that is 25 Mbit/s where 100 Mbit/s is on offer.  It is not only
   throughput: every frame is a busy-polled transfer inside the cooperative
   main loop, so a four-times-shorter transfer is four times less latency
   added to everything else the Pi is doing.

   The switch is verified rather than assumed.  CMD52 travels on CMD and
   keeps working even when the data lines are misconfigured, so reading CCCR
   back would prove nothing; instead a backplane read - a real CMD53 data
   transfer - has to return the chip ID that was already read successfully at
   1-bit.  If it does not, both ends go back to 1-bit and bring-up continues
   exactly as before.  Failing safe matters here: the alternative to a
   working WiFi link is a Pi that needs a power cycle. */
static void sdio_runtime_try_four_bit_bus(sdio_host_t *dev)
{
   uint8_t bus_control = 0u;
   uint32_t reference = 0u;
   unsigned int check;

   if (dev == NULL)
      return;

   /* Reference value, read while the bus is still known-good at 1-bit.  The
      chip ID is not stored anywhere this early in bring-up, and hard-coding
      one would make this stop working on the next chip variant. */
   if (!sdio_backplane_read_u32_timeout(dev, CYW43_CHIPCOMMON_BASE,
                                        SDIO_COMMAND_TIMEOUT_US, &reference)
      || reference == 0u)
      return;

   if (!sdio_probe_read_byte(dev, SDIO_CCCR_BUS_INTERFACE_CONTROL, &bus_control))
      return;

   /* CCCR 0x07: bits 1:0 select the width (00 = 1-bit, 10 = 4-bit), and bit 7
      disconnects the DAT3 card-detect pull-up, which has to go before DAT3
      can carry data. */
   if (!sdio_probe_write_byte(dev, SDIO_CCCR_BUS_INTERFACE_CONTROL,
                              (uint8_t)((bus_control & ~0x03u) | 0x82u)))
      return;

   if (sdio_host_set_bus_width(dev, true) != 0) {
      (void)sdio_probe_write_byte(dev, SDIO_CCCR_BUS_INTERFACE_CONTROL, bus_control);
      return;
   }

   /* Several reads, not one: a single lucky transfer proves less than a
      handful, and this is the only chance to catch a bad 4-bit bus before
      the firmware download commits to it. */
   for (check = 0u; check < 4u; ++check) {
      uint32_t value = 0u;

      if (!sdio_backplane_read_u32_timeout(dev, CYW43_CHIPCOMMON_BASE,
                                           SDIO_COMMAND_TIMEOUT_US, &value)
         || value != reference) {
         (void)sdio_host_set_bus_width(dev, false);
         (void)sdio_probe_write_byte(dev, SDIO_CCCR_BUS_INTERFACE_CONTROL,
                                     bus_control);
         sdio_debug_log("SDIO 4-bit verify failed - staying 1-bit");
         return;
      }
   }

   g_runtime_bus_four_bit = true;
   sdio_debug_log("SDIO bus width 4-bit");
}

/* Move the SDIO bus to high speed (50 MHz) if the card offers it.
 *
 * Verified the same way as the width switch, and for the same reason: CMD52
 * keeps working on a bus whose data timing is wrong, so only a real data
 * transfer proves the change took.
 *
 * The controller's HISPD bit is deliberately NOT set.  The BCM2835's Arasan
 * shifts its output drive edge when that bit is on, which corrupts transfers;
 * the platform's own drivers run this controller at 50 MHz with it clear, and
 * the existing divider path already produces exactly 50 MHz from the 200 MHz
 * base.  There is no CRC retry below TCP, so this is the one change here that
 * trades margin for speed - the SDIO error counters are what to watch. */
static void sdio_runtime_try_high_speed(sdio_host_t *dev)
{
   uint8_t speed = 0u;
   uint32_t reference = 0u;
   unsigned int check;

   if (dev == NULL)
      return;

   if (!sdio_backplane_read_u32_timeout(dev, CYW43_CHIPCOMMON_BASE,
                                        SDIO_COMMAND_TIMEOUT_US, &reference)
      || reference == 0u)
      return;

   if (!sdio_probe_read_byte(dev, SDIO_CCCR_SPEED_SELECT, &speed)
      || (speed & SDIO_CCCR_SPEED_SHS) == 0u)
      return;                          /* card does not offer high speed */

   if (!sdio_probe_write_byte(dev, SDIO_CCCR_SPEED_SELECT,
                              (uint8_t)(speed | SDIO_CCCR_SPEED_EHS)))
      return;

   if (sdio_host_set_clock(dev, SDIO_RUNTIME_HIGH_SPEED_CLOCK_HZ, NULL) != 0) {
      (void)sdio_probe_write_byte(dev, SDIO_CCCR_SPEED_SELECT, speed);
      return;
   }

   for (check = 0u; check < 4u; ++check) {
      uint32_t value = 0u;

      if (!sdio_backplane_read_u32_timeout(dev, CYW43_CHIPCOMMON_BASE,
                                           SDIO_COMMAND_TIMEOUT_US, &value)
         || value != reference) {
         (void)sdio_host_set_clock(dev, SDIO_RUNTIME_HIGH_CLOCK_HZ, NULL);
         (void)sdio_probe_write_byte(dev, SDIO_CCCR_SPEED_SELECT, speed);
         sdio_debug_log("SDIO high speed verify failed - staying at 25MHz");
         return;
      }
   }

   g_runtime_bus_high_speed = true;
   sdio_debug_log("SDIO bus 50MHz high speed");
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

      /* The first KSO write can fail while the core is asleep - that is why
         it is issued twice.  Clear any latched command error from the pair. */
      (void)sdio_function1_write_byte(dev, SDIO_SLEEP_CSR, requested_value);
      (void)sdio_function1_write_byte(dev, SDIO_SLEEP_CSR, requested_value);
      sdio_runtime_set_error(NULL);

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
   sdio_runtime_set_error(NULL); /* retried until the attempt cap - not yet an error */
   return 0;
}

/* Cooperative form of sdio_probe_enable_functions().  The blocking
   original polls SDIO_CCCR_IO_READY up to 100 times with a 1 ms usleep
   between reads - as much as ~100 ms of busy-wait inside a single
   sdio_runtime_tick() call.  This per-tick version issues the fn1
   IO_ENABLE write once, then re-checks IO_READY on a 1 ms deadline,
   returning 0 ("call me again") between polls so the main 1 MHz poll
   loop keeps running.  Returns 1 when fn1 is ready and block sizes are
   set, 0 while still waiting, <0 on error.  The blocking original is
   left in place for the diagnostic probe path (wifi_sdio_probe=1). */
static int sdio_runtime_enable_functions_step(sdio_host_t *dev,
                                              sdio_probe_result_t *probe_result)
{
   uint8_t io_ready = 0u;
   uint32_t now_us;

   if (dev == NULL || probe_result == NULL)
      return -1;

   now_us = RPI_GetSystemTime();

   if (!g_runtime_enable_wait.active) {
      sdio_debug_log("enable_functions: OCR=0x%08lx fn_count=%u raw_response=0x%08lx",
                     (unsigned long)probe_result->ocr.raw_ocr,
                     (unsigned int)probe_result->ocr.function_count,
                     (unsigned long)probe_result->response0);

      /* Emulator workaround: if function count isn't encoded but response
         is non-zero, assume a standard 2-function card. */
      if (probe_result->ocr.function_count < 2u && probe_result->response0 != 0u) {
         sdio_debug_log("enable_functions: working around emulator OCR encoding - accepting 2 functions");
         probe_result->ocr.function_count = 2u;
      }

      if (probe_result->ocr.function_count < 2u) {
         sdio_runtime_set_error("SDIO card reported fewer than 2 functions");
         return -1;
      }

      /* Enable function 1 (backplane) only.  Function 2 (radio data path)
         cannot become ready until the firmware is downloaded over fn1
         later in sdio_runtime_boot_firmware, so we poll for fn1 only. */
      uint8_t requested_io_enable = (uint8_t)(probe_result->io_enable | 0x02u);
      probe_result->requested_io_enable = requested_io_enable;
      probe_result->function_setup_attempted = true;

      if (!sdio_probe_write_byte(dev, SDIO_CCCR_IO_ENABLE, requested_io_enable))
         return -1;

      g_runtime_enable_wait.active = true;
      g_runtime_enable_wait.attempt = 0u;
      g_runtime_enable_wait.deadline_us = now_us;
   }

   if ((int32_t)(now_us - g_runtime_enable_wait.deadline_us) < 0)
      return 0;

   if (!sdio_probe_read_byte(dev, SDIO_CCCR_IO_READY, &io_ready)) {
      memset(&g_runtime_enable_wait, 0, sizeof(g_runtime_enable_wait));
      return -1;
   }

   if ((io_ready & 0x02u) != 0x02u) {
      if (g_runtime_enable_wait.attempt >= 99u) {
         sdio_runtime_set_error("Timed out waiting for SDIO function 1 ready");
         memset(&g_runtime_enable_wait, 0, sizeof(g_runtime_enable_wait));
         return -1;
      }
      g_runtime_enable_wait.attempt++;
      g_runtime_enable_wait.deadline_us = now_us + 1000u;
      return 0;
   }

   /* fn1 ready - finish the one-shot configuration. */
   if (!sdio_probe_read_byte(dev, SDIO_CCCR_IO_ENABLE, &probe_result->configured_io_enable)) {
      memset(&g_runtime_enable_wait, 0, sizeof(g_runtime_enable_wait));
      return -1;
   }
   probe_result->configured_io_ready = io_ready;

   if (!sdio_probe_set_block_size(dev, 1u, SDIO_PROBE_FUNCTION1_BLOCK_SIZE)
      || !sdio_probe_set_block_size(dev, 2u, SDIO_PROBE_FUNCTION2_BLOCK_SIZE)) {
      memset(&g_runtime_enable_wait, 0, sizeof(g_runtime_enable_wait));
      return -1;
   }

   probe_result->function1_block_size = SDIO_PROBE_FUNCTION1_BLOCK_SIZE;
   probe_result->function2_block_size = SDIO_PROBE_FUNCTION2_BLOCK_SIZE;
   probe_result->function_setup_success = true;
   memset(&g_runtime_enable_wait, 0, sizeof(g_runtime_enable_wait));
   return 1;
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
   _Alignas(4) uint8_t scan_buffer[SDIO_CORE_SCAN_SIZE]; // filled by 32-bit EMMC PIO reads
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
            core_id = (uint16_t)(((unsigned)scan_buffer[index + 1u] | ((unsigned)scan_buffer[index + 2u] << 8))
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
         /* Power up every SOCRAM bank.  Pi Zero W's BCM43430A1 only
            needs bank 3's BANKPDA cleared - the other three banks
            default to powered.  Pi Zero 2 W's BCM43436S (socramrev
            ~25) has banks that default to power-DOWN, so an early
            firmware access to any of them locks the chip up before
            HT_AVAIL can assert; this matches exactly the
            "firmware in RAM, ARM released, HT clock never set"
            symptom we previously hit.  Upstream brcmfmac walks
            every bank unconditionally - do the same here.

            'banks' was computed in the loop above from the SOCRAM
            COREINFO register.  Loop over the same range and write
            BANKPDA=0 to each so every bank is powered before the
            ARM core starts fetching instructions. */
         {
            uint32_t bidx;
            uint32_t bank_count = (banks == 0u) ? 4u : banks;
            for (bidx = 0u; bidx < bank_count; ++bidx) {
               if (!sdio_backplane_write_u32(dev,
                                            chip->socramregs + CYW43_BANKIDX_OFFSET,
                                            bidx)
                || !sdio_backplane_write_u32(dev,
                                             chip->socramregs + CYW43_BANKPDA_OFFSET,
                                             0u)) {
                  return false;
               }
            }
         }
         /* The bank loop above mis-sizes the 43430's SOCRAM on this
            silicon revision: SOCRAM rev >= 16 encodes the bank count
            and per-bank size differently from the simplified scan
            here, so the sum comes out as 0x8000 instead of the true
            0x80000.  Both 43430A1 (Pi Zero W) and 43436S (Pi Zero 2 W)
            always have exactly 512 KiB of SOCRAM - hard-code it,
            exactly as the cyw43-driver reference does (CYW43_RAM_SIZE
            = 512 * 1024).
            This matters because the NVRAM image and its trailer token
            are placed at the top of RAM (rambase + socramsize - ...):
            a wrong size puts them inside the firmware image and
            leaves the real top-of-RAM token slot unwritten, so the
            firmware boots, cannot find its NVRAM, faults early, and
            HT_AVAIL never appears. */
         chip->socramsize = 0x80000u;
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

         /* HT-clock wait budget.  Both the BCM43430A1 (Pi Zero W)
            and the BCM43430B0 (Pi Zero 2 W) firmware assert HTAvail
            within 100-300 ms once they start running.  30 * 50 ms
            = 1.5 s is comfortably wider than the worst observed
            settle time without dragging out failure paths (a real
            wrong-firmware case still hits the explicit error
            below, which logs the CSR + chip variant for triage). */
         if ((clock_csr & CYW43_HT_AVAIL) == 0u && g_runtime_boot_wait_attempt < 30u) {
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
            const wifi_config_t *cfg = wifi_get_config();

            if (cfg == NULL || !cfg->allow_emulator_fallback) {
               sdio_runtime_set_error("CYW43 firmware did not start (SDIO function 2 never became ready)");
               sdio_runtime_boot_reset_state();
               return -1;
            }
            g_runtime_emulator_mode = true;
            sdio_debug_log("emulator: function 2 ready bit not set after %ums; proceeding anyway (wifi_emulator=1)",
                           (unsigned int)g_runtime_boot_wait_attempt);
         }

         return sdio_runtime_complete_boot_stage(dev, probe_result, &chip,
                                                 chip_id_register);
      }

      return sdio_runtime_finalize_boot_stage(dev, clock_csr, &chip, now_us);
   }

   /* Resume an in-progress chunked firmware download.  The one-time chip
      scan/setup below has already run (g_runtime_boot_setup_done) and must
      not run again - cyw43_select_chip_variant in particular frees the
      unused firmware blob and is not idempotent - so reload the saved chip
      state and jump straight to the download loop. */
   if (g_runtime_boot_setup_done) {
      chip = g_runtime_boot_chip;
      chip_id_register = g_runtime_boot_chip_id_register;
      goto firmware_download;
   }

   if (!sdio_backplane_read_u32(dev, CYW43_ENUM_BASE, &chip_id_register)) {
      sdio_runtime_set_error("Failed to read CYW43 chip ID");
      return -1;
   }

   chip.chip_id = (uint16_t)(chip_id_register & 0xffffu);
   chip.chip_revision = (uint8_t)((chip_id_register >> 16) & 0x0fu);
   /* Surface chip_revision in the boot log: the 43430-family chip
      IDs (Pi Zero W's BCM43430A1, Pi Zero 2 W's BCM43436S/43436B0)
      all report chip_id=43430 - chip_revision is what
      distinguishes them and what upstream brcmfmac uses to pick
      between brcmfmac43430a0-sdio.bin, brcmfmac43430-sdio.bin,
      brcmfmac43436b0-sdio.bin and brcmfmac43436s-sdio.bin.
      socramrev is a useful secondary cue but doesn't fully
      disambiguate, hence logging both. */
   sdio_debug_log("chip: id=0x%04x (%u) revision=%u",
                  (unsigned int)chip.chip_id,
                  (unsigned int)chip.chip_id,
                  (unsigned int)chip.chip_revision);
   /* Accept 43430 family for every build; additionally accept 43455
      (Pi 3 B+ / Pi 4) on the ARMv8 build where cyw43_preload_images
      preloaded a 43455 alt firmware set.  ARMv6 builds (Pi Zero W
      only) keep the strict 43430-only check. */
#if __ARM_ARCH >= 7
   /* BCM43455 (Pi 3 B+ / Pi 4) reports ChipCommon ID 0x4345 - the BCM4345
      base part number in hex, NOT decimal 43455.  (The 43430 family reports
      0xA9A6, which happens to equal decimal 43430, hence the asymmetry;
      this matches Linux brcmfmac's BRCM_CC_43455_CHIP_ID = 0x4345.) */
   if (chip.chip_id != 43430u && chip.chip_id != 0x4345u) {
      sdio_runtime_set_error("Unsupported CYW43 chip ID");
      return -1;
   }
#else
   if (chip.chip_id != 43430u) {
      sdio_runtime_set_error("Unsupported CYW43 chip ID");
      return -1;
   }
#endif

   if (!sdio_backplane_read_u32(dev, CYW43_ENUM_BASE + (63u * 4u), &enumeration_address)
      || !sdio_backplane_scan_cores(dev, &chip, enumeration_address)) {
      sdio_runtime_set_error("Failed to enumerate CYW43 backplane cores");
      return -1;
   }

   /* Diagnostic: dump the enumerated core addresses.  For a BCM43430 the
      wrapper bases should sit ~0x100000 above the core bases, e.g. ARM CM3
      core ~0x18003000 / wrapper ~0x18103000, SOCRAM core ~0x18004000 /
      wrapper ~0x18104000, d11 wrapper ~0x18101000.  If armctl is not a
      plausible ARM-CM3 wrapper address, reset_core(armctl) writes to the
      wrong registers and the CYW43 CPU is never released - which presents
      exactly as "firmware in RAM but HT_AVAIL never appears". */
   sdio_debug_log("cores: arm_core=0x%lx cc=0x%08lx armregs=0x%08lx armctl=0x%08lx",
                  (unsigned long)chip.arm_core,
                  (unsigned long)chip.chipcommon,
                  (unsigned long)chip.armregs,
                  (unsigned long)chip.armctl);
   sdio_debug_log("cores: socramregs=0x%08lx socramctl=0x%08lx socramrev=%u d11ctl=0x%08lx sdregs=0x%08lx",
                  (unsigned long)chip.socramregs,
                  (unsigned long)chip.socramctl,
                  (unsigned int)chip.socramrev,
                  (unsigned long)chip.d11ctl,
                  (unsigned long)chip.sdregs);

#if __ARM_ARCH >= 7
   /* ARMv8 build: cyw43_preload_images stashed both 43436 (the
      blob the BCM43430B0 on Pi Zero 2 W actually uses, despite
      brcmfmac's naming) and 43455 firmware sets.  Now that we know
      chip_id+socramrev, pick the right one and free the loser.
      After this returns, g_cyw43_firmware_data / _length point at
      the matching blob for the rest of this boot. */
   if (!cyw43_select_chip_variant(chip.chip_id, chip.socramrev)) {
      /* cyw43_select_chip_variant has already LOG_INFO'd the specific
         reason (unknown chip_id, or missing firmware blob).  Surface a
         short error to the runtime so the boot stage halts cleanly. */
      sdio_runtime_set_error("No matching CYW43 firmware preloaded for this chip");
      return -1;
   }
   sdio_debug_log("variant: chip_id=%u socramrev=%u -> firmware=%lu bytes",
                  (unsigned int)chip.chip_id,
                  (unsigned int)chip.socramrev,
                  (unsigned long)g_cyw43_firmware_length);
#endif

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

   /* Diagnostic: a BCM43430 has exactly 512 KiB (0x00080000) of SOCRAM at
      rambase 0.  The NVRAM image and its trailer token are placed at the
      very top of RAM (rambase + socramsize - ...), so a wrong socramsize
      makes the firmware boot but read its NVRAM from the wrong address,
      fault early, and never reach HT_AVAIL.  socramsize MUST read
      0x00080000 here for a 43430. */
   /* Both the BCM43430A1 (Pi Zero W) and the BCM43430B0 (Pi Zero 2 W)
      ship with 512 KB of SOCRAM and the same rambase, so a single
      0x00080000 expectation covers both compile-time variants. */
   sdio_debug_log("RAM: rambase=0x%08lx socramsize=0x%08lx (expect 0x00080000 for 43430a1/43430b0)",
                  (unsigned long)chip.rambase,
                  (unsigned long)chip.socramsize);

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

   /* One-time setup is complete: persist the scanned chip state so a
      resumed tick can skip straight to the download loop below. */
   g_runtime_boot_chip = chip;
   g_runtime_boot_chip_id_register = chip_id_register;
   g_runtime_boot_setup_done = true;

firmware_download:
   /* Push the firmware image in bounded bursts, yielding to the poll loop
      between bursts so no single tick stalls the 1 MHz bus.  Progress is
      held in g_runtime_boot_fw_offset across ticks; the resume fast-path
      above re-enters here without re-running setup. */
   {
      unsigned int chunks_this_tick = 0u;

      while (g_runtime_boot_fw_offset < g_cyw43_firmware_length
             && chunks_this_tick < SDIO_RUNTIME_BOOT_FW_CHUNKS_PER_TICK) {
         uint32_t chunk = g_cyw43_firmware_length - g_runtime_boot_fw_offset;

         if (chunk > SDIO_BACKPLANE_TRANSFER_MAX)
            chunk = SDIO_BACKPLANE_TRANSFER_MAX;

         if (!sdio_backplane_write_bytes(dev, chip.rambase + g_runtime_boot_fw_offset,
                                         &g_cyw43_firmware_data[g_runtime_boot_fw_offset], chunk)) {
            sdio_runtime_set_error("Failed to upload CYW43 firmware image");
            sdio_runtime_boot_reset_state();
            return -1;
         }

         g_runtime_boot_fw_offset += chunk;
         chunks_this_tick++;
      }

      if (g_runtime_boot_fw_offset < g_cyw43_firmware_length)
         return 0;   /* more chunks next tick */
   }

   /* Verify the firmware download landed intact.  A subtle backplane
      window / CMD53 chunking bug corrupts SOCRAM silently: the chip ID
      still reads back fine, but the firmware image in RAM is wrong, so
      the CYW43's internal CPU crashes the instant it is started and
      HT_AVAIL never appears.  Sample words spread across the image
      (including either side of 32 KiB window boundaries, plus the last
      word) and compare against the source, so a corrupt download is
      reported clearly instead of resurfacing later as a mysterious
      "firmware did not start". */
   {
      static const uint32_t verify_offsets[] = {
         0x00000u, 0x04000u, 0x07ffcu, 0x08000u,
         0x0fffcu, 0x10000u, 0x20000u, 0x30000u
      };
      const size_t verify_count = sizeof(verify_offsets) / sizeof(verify_offsets[0]);
      uint32_t last_offset = (g_cyw43_firmware_length >= 4u) ? ((g_cyw43_firmware_length - 4u) & ~3u) : 0u;
      size_t v;

      for (v = 0u; v <= verify_count; ++v) {
         uint32_t off = (v < verify_count) ? verify_offsets[v] : last_offset;
         uint32_t expected;
         uint32_t got = 0u;

         if (off + 4u > g_cyw43_firmware_length)
            continue;

         expected = sdio_load_u32_le(&g_cyw43_firmware_data[off]);
         if (!sdio_backplane_read_u32(dev, chip.rambase + off, &got)) {
            sdio_runtime_set_error("Failed to read back CYW43 firmware for verification");
            return -1;
         }
         if (got != expected) {
            sdio_debug_log("firmware verify FAILED at 0x%lx: wrote 0x%08lx read 0x%08lx",
                           (unsigned long)off,
                           (unsigned long)expected,
                           (unsigned long)got);
            sdio_runtime_set_error("CYW43 firmware download corrupted (backplane write verify failed)");
            return -1;
         }
      }
      sdio_debug_log("firmware download verified intact (%u sample points)",
                     (unsigned int)(verify_count + 1u));
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

   nvram_token = (nvram_length / 4u) & 0xffffu;
   nvram_token |= (~nvram_token << 16);
   sdio_store_u32_le(token_buffer, nvram_token);
   if (!sdio_backplane_write_bytes(dev, chip.rambase + chip.socramsize - 4u,
                                   token_buffer, sizeof(token_buffer))) {
      free(condensed_nvram);
      sdio_runtime_set_error("Failed to write CYW43 NVRAM token");
      return -1;
   }

   /* Diagnostic: read the NVRAM trailer token and the first NVRAM word
      back from the top of RAM and compare with what we wrote.  The
      firmware locates its NVRAM via the token at (rambase + socramsize -
      4): a MISMATCH means the NVRAM never reached the chip (or landed at
      the wrong address because socramsize is wrong); an OK readback with
      the firmware still not starting points at the NVRAM content/format
      itself. */
   {
      uint32_t token_got = 0u;
      uint32_t nvram_first_got = 0u;
      uint32_t nvram_first_expected = sdio_load_u32_le(condensed_nvram);
      bool token_ok = sdio_backplane_read_u32(dev, chip.rambase + chip.socramsize - 4u,
                                              &token_got);
      bool nvram_ok = sdio_backplane_read_u32(dev,
                         chip.rambase + chip.socramsize - nvram_length - 4u,
                         &nvram_first_got);

      sdio_debug_log("NVRAM: len=%lu token wrote=0x%08lx read=0x%08lx %s",
                     (unsigned long)nvram_length,
                     (unsigned long)nvram_token,
                     (unsigned long)token_got,
                     (token_ok && token_got == nvram_token) ? "OK" : "MISMATCH");
      sdio_debug_log("NVRAM: first word wrote=0x%08lx read=0x%08lx %s",
                     (unsigned long)nvram_first_expected,
                     (unsigned long)nvram_first_got,
                     (nvram_ok && nvram_first_got == nvram_first_expected) ? "OK" : "MISMATCH");
   }

   free(condensed_nvram);

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
   } else {
      /* Clear any pending SDIO-core interrupt-status bits before the
         ARM CM3 starts running.  The 43430A1 (Pi Zero W) doesn't
         normally have anything pending at this point, so the original
         code only did this on the CR4 path - but the 43436S
         apparently does, and if firmware is released with an IRQ
         already latched the Cortex-M3 services it immediately, finds
         no handler installed yet, falls into HardFault and spins
         there forever (cur_res never moves off 0x3, HT_AVAIL never
         asserts).  Clear the same way the CR4 path does. */
      if (!sdio_backplane_write_u32(dev, chip.sdregs + SDIO_CORE_INT_STATUS_OFFSET, ~0u)) {
         sdio_runtime_set_error("Failed to clear CYW43 SDIO interrupts (CM3)");
         return -1;
      }

      if (!sdio_backplane_reset_core(dev, chip.armctl, 0u, 0u)) {
         sdio_runtime_set_error("Failed to start CYW43 ARM core");
         return -1;
      }
   }

   /* Diagnostic: read the ARM core wrapper back after starting it.  This
      decisively splits the remaining failure modes:
        - RESETCTRL bit0 == 1  -> the CPU is STILL HELD IN RESET; reset_core
          did not take effect (armctl wrong, or the reset sequence failed).
          The firmware-not-running cause is then on the host side.
        - RESETCTRL bit0 == 0 and IOCTRL bit0 == 1 -> the CPU is released
          and clocked, so it IS executing; the firmware itself is faulting
          early (NVRAM handoff / clock / firmware-content), not the host
          reset path. */
   {
      uint32_t arm_resetctrl = 0xffffffffu;
      uint32_t arm_ioctrl = 0xffffffffu;

      (void)sdio_backplane_read_u32(dev, chip.armctl + CYW43_CORE_RESETCTRL_OFFSET,
                                    &arm_resetctrl);
      (void)sdio_backplane_read_u32(dev, chip.armctl + CYW43_CORE_IOCTRL_OFFSET,
                                    &arm_ioctrl);
      sdio_debug_log("ARM core after start: armctl=0x%08lx resetctrl=0x%08lx ioctrl=0x%08lx",
                     (unsigned long)chip.armctl,
                     (unsigned long)arm_resetctrl,
                     (unsigned long)arm_ioctrl);
      sdio_debug_log("  -> resetctrl bit0=%u (0=released) ioctrl bit0=%u (1=clocked)",
                     (unsigned int)(arm_resetctrl & 1u),
                     (unsigned int)(arm_ioctrl & 1u));
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

   /* This sequence is a faithful port of the working bare-metal PicoWi
      driver's join_start() + join_restart() (picowi_join.c): every
      iovar/ioctl PicoWi issues to join a WPA2 network is reproduced
      here, in the same order, INCLUDING PicoWi's settle delays (see
      sdio_tx_probe_post_delay_us): 150 ms after the AMPDU/aggregation
      block, 50 ms after events_enable, 50 ms after mcast_list.

      Two deviations were removed after the first full-port test
      (a join that ran all 35 steps but never started - WLC_SET_SSID
      acked, GET_SSID read back all-zero, zero events):
        * No leading WLC_DOWN.  PicoWi's join_start does NOT issue
          WLC_DOWN; the chip is already in its power-on (down) state
          after firmware boot.  Sending WLC_DOWN first made the very
          next "country" iovar return BCME_BADARG (-2).
        * PicoWi's real inter-stage delays are restored.  The earlier
          flat 10 ms spacing skipped the 150 ms radio/PHY settle that
          PicoWi waits after the AMPDU block, so the radio was not
          ready when WLC_SET_SSID fired.
      Pi1MHz-specific extras kept on top of PicoWi: MPC_OFF /
      POWERSAVE_OFF / ROAM_OFF (keep the radio awake and stationary)
      and the three-form event_msgs setup (this driver's equivalent
      of PicoWi's events_enable()). */

   /* --- join_start: radio + aggregation setup.  The regulatory country
          is deliberately NOT set here - see the WLC_UP / DOWN / COUNTRY
          / UP block below for why it has to wait for the first UP. --- */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_ANTDIV;          /* WLC_SET_ANTDIV = 0 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_TXGLOM_OFF;      /* bus:txglom = 0 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_APSTA;           /* apsta = 1 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_BA_WSIZE;  /* ampdu_ba_wsize = 8 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_MPDU;      /* ampdu_mpdu = 4 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_RX_FACTOR; /* ampdu_rx_factor = 0; 150 ms settle follows */
   /* Enable async events before the join so WLC_E_SET_SSID / _LINK /
      _PSK_SUP are delivered.  PicoWi calls events_enable() here; this
      driver sends the per-bsscfg + global + _ext event masks.  Order
      matches PicoWi: events_enable BEFORE mcast_list. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_GLOBAL_EVENT_MSGS;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_EXT;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_VERIFY; /* 50 ms settle follows */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_MCAST_LIST;      /* mcast_list (IPv4 multicast MAC); 50 ms settle follows */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_MPC_OFF;         /* mpc = 0: keep the radio awake */
   /* Band / G-mode MUST be set while the interface is DOWN.  WLC_UP
      initialises the PHY, and on the BCM43430 the PHY needs to know
      its band and G-mode BEFORE it comes up.  The first PicoWi-order
      port placed these AFTER WLC_UP: WLC_UP returned status 0 but the
      interface never actually came up - a following WLC_SCAN reported
      BCME_NOTUP (-4).  brcmfmac sets band + gmode down, then UP. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_BAND;           /* WLC_SET_BAND = 0 (auto / 2.4 GHz) */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_GMODE;          /* WLC_SET_GMODE = 1 */

   /* --- join_restart: bring the interface up and configure the join.
          WLC_UP gets a 100 ms settle (see sdio_tx_probe_post_delay_us)
          so the PHY has finished coming up before anything else runs. --- */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_UP;             /* first WLC_UP - warms up the wlc/regulatory module */
   /* Set the regulatory country only AFTER the interface has been
      brought up once.  Sent cold as the first ioctl - with or without
      a preceding WLC_DOWN - the "country" iovar SET returned
      BCME_BADARG every single time, while the GET form always worked.
      That GET-ok / SET-bad asymmetry means the SET handler needs the
      wlc to have been through a WLC_UP first (brcmfmac never sets
      country cold either).  brcm convention is to change country with
      the interface DOWN, so: first UP (above), then DOWN, set country,
      then UP again - the second UP re-evaluates the radio's regulatory
      state, which should clear WL_RADIO_COUNTRY_DISABLE. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_DOWN;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_COUNTRY;        /* regulatory domain (wifi_country, default "GB") */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_GET_COUNTRY;    /* diagnostic: did the SET finally take? */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_UP;             /* second WLC_UP - radio re-evaluates regulatory */
   /* Diagnostic: read the radio-disable bitmask after the second UP.
      0x00 = radio enabled (success); 0x08 = still country-disabled. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_GET_RADIO;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_POWERSAVE_OFF;  /* WLC_SET_PM = 0 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_PM2_SLEEP_RET;  /* pm2_sleep_ret = 0xc8 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_BCN;     /* bcn_li_bcn = 1 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_DTIM;    /* bcn_li_dtim = 1 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_ASSOC_LISTEN;   /* assoc_listen = 0x0a */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_INFRA;          /* WLC_SET_INFRA = 1 */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_AUTH;           /* WLC_SET_AUTH = 0 (802.11 open) */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_WSEC;           /* WLC_SET_WSEC = 6 (TKIP|AES) */
   /* The in-firmware WPA supplicant must exist BEFORE the passphrase is
      installed: PicoWi sets the three sup_wpa iovars, delays 2 ms, then
      sends WLC_SET_WSEC_PMK.  The 10 ms inter-command delay in
      sdio_probe_send_tx_control_template covers that settle time. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA2_EAPVER;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA_TMO;
   if (config != NULL && config->password[0] != '\0')
      commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_PMK;         /* WLC_SET_WSEC_PMK */
   /* PicoWi re-issues INFRA and AUTH after the PMK, then WPA_AUTH. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_INFRA;          /* WLC_SET_INFRA = 1 (again) */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_AUTH;           /* WLC_SET_AUTH = 0 (again) */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH;       /* WLC_SET_WPA_AUTH = 0x80 (WPA2-PSK) */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_ROAM_OFF;       /* roam_off = 1 */
   /* WLC_SET_SSID with a wlc_ssid_t triggers the actual scan + join. */
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_SSID;
   commands[count++] = WIFI_SDIO_TX_PROBE_COMMAND_GET_SSID;       /* diagnostic readback */
   /* No explicit WLC_SCAN here: it was only ever a radio diagnostic,
      and now that the CLM is loaded and SET_SSID genuinely starts a
      join, a scan issued immediately afterwards just collides with it
      (the firmware answers BCME_BUSY).  Let the join run undisturbed. */

   return count;
}

static uint32_t sdio_tx_probe_post_delay_us(wifi_sdio_tx_probe_command_t command)
{
   /* Per-command settle delay applied AFTER the ioctl is sent and its
      CDC response drained, matching the working PicoWi driver's
      join_start() timing (picowi_join.c):
        usdelay(150000) after the AMPDU / aggregation block - lets the
                        radio + PHY finish coming up;
        usdelay(50000)  after events_enable();
        usdelay(50000)  after mcast_list.
      The earlier flat 10 ms spacing skipped the 150 ms settle, which
      left the radio not ready when WLC_SET_SSID fired (join accepted
      but never started).  Every other command keeps the 10 ms
      inter-ioctl spacing, which also covers PicoWi's 2 ms pause
      between the sup_wpa iovars and WLC_SET_WSEC_PMK. */
   switch (command) {
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_RX_FACTOR:
         return 150000u;
      case WIFI_SDIO_TX_PROBE_COMMAND_UP:
         /* WLC_UP initialises the PHY; give it a generous settle so the
            radio has finished coming up before WLC_SET_SSID / WLC_SCAN
            run.  The BCM43430 PHY bring-up takes tens of ms. */
         return 100000u;
      case WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_VERIFY:
      case WIFI_SDIO_TX_PROBE_COMMAND_MCAST_LIST:
         return 50000u;
      default:
         return 10000u;
   }
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
      case WIFI_SDIO_TX_PROBE_COMMAND_APSTA:
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_BA_WSIZE:
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_MPDU:
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_RX_FACTOR:
      case WIFI_SDIO_TX_PROBE_COMMAND_MCAST_LIST:
      case WIFI_SDIO_TX_PROBE_COMMAND_PM2_SLEEP_RET:
      case WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_BCN:
      case WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_DTIM:
      case WIFI_SDIO_TX_PROBE_COMMAND_ASSOC_LISTEN:
      case WIFI_SDIO_TX_PROBE_COMMAND_SET_MAC:
         return WLC_SET_VAR;
      case WIFI_SDIO_TX_PROBE_COMMAND_ANTDIV:
         return WLC_SET_ANTDIV;
      case WIFI_SDIO_TX_PROBE_COMMAND_GMODE:
         return WLC_SET_GMODE;
      case WIFI_SDIO_TX_PROBE_COMMAND_BAND:
         return WLC_SET_BAND;
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
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_COUNTRY:
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
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_RADIO:
         return WLC_GET_RADIO;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_RSSI:
         return WLC_GET_RSSI;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_PM:
         return WLC_GET_PM;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_PKTCNTS:
         return WLC_GET_PKTCNTS;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_RATE:
         return WLC_GET_RATE;
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
         /* wl_wsec_pmk_t is exactly u16 key_len + u16 flags +
            u8 key[WSEC_MAX_PSK_LEN] = 68 bytes.  cyw43-driver and
            brcmfmac send precisely this struct size; an over-length
            WLC_SET_WSEC_PMK ioctl is rejected by some BCM43430
            firmware builds (which would silently leave the supplicant
            with no key).  Previously this returned the 80-byte
            template maximum. */
         return (uint16_t)(4u + WSEC_MAX_PSK_LEN);
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
      case WIFI_SDIO_TX_PROBE_COMMAND_SET_MAC:
         /* "cur_etheraddr\0" + 6 MAC bytes the chip should adopt.  The
            actual payload bytes are filled by sdio_prepare_tx_control_-
            payload from g_runtime_desired_mac; the length is identical
            to GET_MAC. */
         return (uint16_t)(sizeof("cur_etheraddr") + 6u);
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_COUNTRY:
         /* "country\0" + 20-byte response slot.  wl_country_t is 12
            bytes; the extra room lets us see if this firmware build
            returns a larger struct. */
         return (uint16_t)(sizeof("country") + 20u);
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_SUP_WPA:
         /* "bsscfg:sup_wpa\0" + bsscfg_idx (4) + 4-byte response slot.
            Per-bsscfg iovar - same shape as bsscfg:event_msgs but with
            a 4-byte u32 value instead of a 16-byte mask. */
         return (uint16_t)(sizeof("bsscfg:sup_wpa") + 4u + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WSEC:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WPA_AUTH:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_AUTH:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_INFRA:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_RADIO:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_RSSI:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_PM:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_RATE:
         /* WLC_GET_* ioctls return a u32; send a 4-byte zero buffer
            for the chip to fill. */
         return 4u;
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_PKTCNTS:
         return WLC_PKTCNTS_BYTES;   /* five u32 counters to be filled in */
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
      {
         /* PicoWi sends WLC_SET_SSID with EXACTLY 4 + strlen(ssid)
            bytes - a u32 length field followed by just the SSID
            characters, no 32-byte padding (picowi_join.c: *data = n;
            strcpy(&data[4], ssid); ioctl_wr_data(WLC_SET_SSID, .., n+4)).
            We previously sent the canonical 36-byte wlc_ssid_t and,
            earlier still, a 50-byte wl_join_params_le; the chip acked
            both but never started the join.  Copy PicoWi byte-for-byte
            so the SET_SSID frame is identical to the known-good driver. */
         const wifi_config_t *config = wifi_get_config();
         size_t ssid_length = 0u;

         if (config != NULL)
            ssid_length = strnlen(config->ssid, WLC_SSID_MAX_LEN);
         return (uint16_t)(4u + ssid_length);
      }
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
      case WIFI_SDIO_TX_PROBE_COMMAND_APSTA:
         return (uint16_t)(sizeof("apsta") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_BA_WSIZE:
         return (uint16_t)(sizeof("ampdu_ba_wsize") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_MPDU:
         return (uint16_t)(sizeof("ampdu_mpdu") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_RX_FACTOR:
         return (uint16_t)(sizeof("ampdu_rx_factor") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_PM2_SLEEP_RET:
         return (uint16_t)(sizeof("pm2_sleep_ret") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_BCN:
         return (uint16_t)(sizeof("bcn_li_bcn") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_DTIM:
         return (uint16_t)(sizeof("bcn_li_dtim") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_ASSOC_LISTEN:
         return (uint16_t)(sizeof("assoc_listen") + 4u);
      case WIFI_SDIO_TX_PROBE_COMMAND_MCAST_LIST:
         /* "mcast_list\0" + maclist_t: u32 count + 10 * ether_addr (60).
            PicoWi sends a 60-byte mcast_addr block (count=1 + one MAC,
            the rest zero-padded) - reproduced verbatim. */
         return (uint16_t)(sizeof("mcast_list") + 60u);
      case WIFI_SDIO_TX_PROBE_COMMAND_COUNTRY:
         /* "country\0" + a 12-byte wl_country_t (country_abbrev[4] +
            int32 rev + ccode[4]) - the EXACT size brcmfmac sends.
            We previously sent 20 bytes (PicoWi's country_data[20], i.e.
            the struct plus 8 trailing zeros); a firmware that strictly
            length-checks the iovar against sizeof(wl_country_t) rejects
            that with BCME_BADARG, which is what this build keeps doing. */
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
         || command == WIFI_SDIO_TX_PROBE_COMMAND_MPC_OFF
         || command == WIFI_SDIO_TX_PROBE_COMMAND_ANTDIV
         || command == WIFI_SDIO_TX_PROBE_COMMAND_GMODE
         || command == WIFI_SDIO_TX_PROBE_COMMAND_BAND
         || command == WIFI_SDIO_TX_PROBE_COMMAND_APSTA
         || command == WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_BA_WSIZE
         || command == WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_MPDU
         || command == WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_RX_FACTOR
         || command == WIFI_SDIO_TX_PROBE_COMMAND_MCAST_LIST
         || command == WIFI_SDIO_TX_PROBE_COMMAND_PM2_SLEEP_RET
         || command == WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_BCN
         || command == WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_DTIM
         || command == WIFI_SDIO_TX_PROBE_COMMAND_ASSOC_LISTEN;
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
      case WIFI_SDIO_TX_PROBE_COMMAND_GMODE:
         /* WLC_SET_INFRA = 1 (BSS infrastructure mode);
            WLC_SET_GMODE = 1 (GMODE_AUTO) - both match PicoWi. */
         return 1u;
      case WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH:
      {
         const wifi_config_t *config = wifi_get_config();

         return (config != NULL && config->password[0] != '\0') ? WPA2_AUTH_PSK : WPA_AUTH_DISABLED;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_WSEC:
      {
         /* PicoWi sets WLC_SET_WSEC = 6 for a WPA2 network
            (TKIP_ENABLED | AES_ENABLED): the chip's in-firmware
            supplicant negotiates the actual cipher with the AP, so it
            must advertise both.  An AES-only WSEC made some BCM43430
            builds reject mixed WPA/WPA2 (TKIP group cipher) APs. */
         const wifi_config_t *config = wifi_get_config();

         return (config != NULL && config->password[0] != '\0')
            ? (TKIP_ENABLED | AES_ENABLED) : 0u;
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
      case WIFI_SDIO_TX_PROBE_COMMAND_ANTDIV:  /* WLC_SET_ANTDIV = 0 (antenna 0) */
      case WIFI_SDIO_TX_PROBE_COMMAND_BAND:    /* WLC_SET_BAND = 0 (auto / 2.4 GHz) */
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
   uint16_t payload_length;

   if (probe_result == NULL)
      return;

   /* Defensive: every per-command branch below writes into
      tx_control_template_payload_bytes via memcpy with a length
      derived from sdio_tx_probe_payload_length(command).  If a
      future iovar were to declare a length > the array size, the
      memcpy would overflow.  Refuse to prepare such a command - the
      caller will see the unset state and bail rather than corrupting
      adjacent fields. */
   payload_length = sdio_tx_probe_payload_length(command);
   if (payload_length > SDIO_TX_CONTROL_PAYLOAD_MAX) {
      sdio_debug_log("tx_control payload length %u exceeds buffer (%u); refusing",
                     (unsigned)payload_length,
                     (unsigned)SDIO_TX_CONTROL_PAYLOAD_MAX);
      return;
   }

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
            ssid_length=0 here means Pi1MHz.cfg didn't supply a
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
      case WIFI_SDIO_TX_PROBE_COMMAND_SET_MAC:
      {
         /* SET-VAR "cur_etheraddr" - overrides the chip's factory OTP
            MAC with g_runtime_desired_mac.  Caller (wifi.c) must have
            cached the MAC via sdio_runtime_set_desired_mac() before
            sdio_runtime_start; the SET_MAC stage skips itself when
            no MAC is cached so the chip just keeps its OTP MAC. */
         size_t name_length = sizeof("cur_etheraddr");

         memcpy(probe_result->tx_control_template_payload_bytes,
                "cur_etheraddr", name_length);
         if (g_runtime_desired_mac_valid) {
            memcpy(&probe_result->tx_control_template_payload_bytes[name_length],
                   g_runtime_desired_mac, 6u);
         }
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_COUNTRY:
      {
         /* GET-VAR "country" - the chip writes back its current
            wl_country_t (country_abbrev[4] + int32 rev + ccode[4]).
            Trailing bytes left zero for the chip to fill. */
         size_t name_length = sizeof("country");

         memcpy(probe_result->tx_control_template_payload_bytes,
                "country", name_length);
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
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_RADIO:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_RSSI:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_PM:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_PKTCNTS:
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_RATE:
         /* WLC_GET_* ioctls write their reply into the response slot, which
            the memset at the top of this function has already zeroed. */
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
      /* --- PicoWi join_start / join_restart radio + aggregation setup.
             Each value is byte-for-byte what picowi_join.c sends. --- */
      case WIFI_SDIO_TX_PROBE_COMMAND_APSTA:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "apsta", 1u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_BA_WSIZE:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "ampdu_ba_wsize", 8u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_MPDU:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "ampdu_mpdu", 4u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_RX_FACTOR:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "ampdu_rx_factor", 0u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_PM2_SLEEP_RET:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "pm2_sleep_ret", 0xc8u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_BCN:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "bcn_li_bcn", 1u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_DTIM:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "bcn_li_dtim", 1u);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_ASSOC_LISTEN:
         sdio_prepare_tx_control_iovar_u32_payload(probe_result, "assoc_listen", 0x0au);
         break;
      case WIFI_SDIO_TX_PROBE_COMMAND_MCAST_LIST:
      {
         /* "mcast_list\0" + maclist_t.  PicoWi's mcast_addr block is 60
            bytes: u32 count = 1, then ether_addr[0] = 01:00:5E:00:00:FB
            (the IPv4 "all mDNS" multicast MAC), the remaining 9 slots
            left zero.  Reproduced verbatim. */
         size_t name_length = sizeof("mcast_list");
         uint8_t *value;
         static const uint8_t mcast_mac[6] = { 0x01u, 0x00u, 0x5Eu, 0x00u, 0x00u, 0xFBu };

         memcpy(probe_result->tx_control_template_payload_bytes,
                "mcast_list", name_length);
         value = &probe_result->tx_control_template_payload_bytes[name_length];
         /* count = 1 (LE u32); remaining 56 bytes already zero. */
         sdio_store_u32_le(value, 1u);
         memcpy(&value[4], mcast_mac, sizeof(mcast_mac));
         break;
      }
      case WIFI_SDIO_TX_PROBE_COMMAND_COUNTRY:
      {
         /* "country\0" followed by a 20-byte country structure, in the
            same layout as the working PicoWi driver's country_data:
              country_abbrev[4]  (e.g. "XX")
              rev (int32 LE)    = -1   (0xFFFFFFFF, "use firmware default")
              ccode[4]           (e.g. "XX")
              + 8 trailing zero bytes
            The country code comes from the wifi_country Pi1MHz.cfg property
            (config->country, default "XX" = worldwide / unrestricted).
            PicoWi sends sizeof(country_data) = 20 bytes here and joins
            successfully; the full 20-byte length matters - an earlier
            12-byte struct drew BCME_BADARG from this firmware build. */
         const wifi_config_t *config = wifi_get_config();
         const char *country = "XX";
         size_t name_length = sizeof("country");
         uint8_t *value;
         size_t i;

         if (config != NULL && config->country[0] != '\0')
            country = config->country;

         memcpy(probe_result->tx_control_template_payload_bytes, "country", name_length);
         value = &probe_result->tx_control_template_payload_bytes[name_length];
         /* country_abbrev[4] and ccode[4]: copy up to 3 chars of the
            code into both fields, leaving the 4th byte as the NUL
            terminator (already zero from the function-entry memset). */
         for (i = 0u; i < 3u && country[i] != '\0'; ++i) {
            value[i]     = (uint8_t)country[i];   /* country_abbrev */
            value[8 + i] = (uint8_t)country[i];   /* ccode */
         }
         sdio_store_u32_le(&value[4], 0xffffffffu); /* rev = -1 */
         /* value[12..19] remain zero from the memset at function entry. */
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

/* Take the next number in the shared SDPCM sequence.  Called at the point a
   frame is actually written to fn2, never when one is merely prepared: the
   number has to reflect the order frames reach the bus, and a control template
   can be built one poll and sent another with data frames in between. */
static uint8_t sdio_next_sdpcm_sequence(void)
{
   uint8_t sequence = g_sdpcm_tx_sequence;

   ++g_sdpcm_tx_sequence;
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

   if (probe_result == NULL)
      return;

   request_id = sdio_next_tx_probe_request_id();
   payload_length = sdio_tx_probe_payload_length(command);

   /* An oversized payload would make sdio_prepare_tx_control_payload()
      refuse to fill the buffer below, but the length fields written here
      would still be sent - leave the template unusable instead. */
   if (payload_length > SDIO_TX_CONTROL_PAYLOAD_MAX) {
      probe_result->tx_control_template_ready = false;
      return;
   }

   probe_result->tx_control_template_frame_size = (uint16_t)(SDPCM_CONTROL_EVENT_HEADER_LENGTH
      + CDC_HEADER_LENGTH + payload_length);
   probe_result->tx_control_template_frame_size_complement = (uint16_t)~probe_result->tx_control_template_frame_size;
   /* tx_control_template_sequence is filled in by the sender, from the shared
      SDPCM sequence, at the moment the frame goes out. */
   probe_result->tx_control_template_sequence = 0u;
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

/* One CMD53 per frame, by rounding up to a whole number of blocks rather
   than sending the remainder as a second, byte-mode command.

   A 1532-byte SDPCM frame used to become two transfers: 2 x 512 in block
   mode plus 508 in byte mode.  The second command is pure overhead - its
   command, response and completion polling cost about as much as the 508
   bytes it carries - and every frame in both directions paid it.

   Padding is safe in both directions because SDPCM frames are delimited by
   the length in their own header, not by the size of the bus transfer: on
   write the chip ignores the tail (tx_frame is zeroed before use, so it is
   never stale data on the wire), and on read the extra bytes are chip
   padding that the parser never looks at.  What that costs is buffer
   headroom, which is why the frame buffers are a block larger than the
   longest frame they will accept. */
static bool sdio_function2_transfer_timeout(sdio_host_t *dev, bool write, uint8_t *buffer,
                                            uint16_t length, uint32_t timeout_us)
{
   if (length == 0u)
      return true;

   if (length > SDIO_PROBE_FUNCTION2_BLOCK_SIZE) {
      uint16_t block_count = (uint16_t)((length + SDIO_PROBE_FUNCTION2_BLOCK_SIZE - 1u)
                                        / SDIO_PROBE_FUNCTION2_BLOCK_SIZE);

      if (block_count > 511u)
         block_count = 511u;

      return sdio_cmd53_execute_timeout(dev, 2u, 0u, write, true, false, block_count,
                                        buffer, SDIO_PROBE_FUNCTION2_BLOCK_SIZE,
                                        timeout_us, NULL);
   }

   return sdio_cmd53_execute_timeout(dev, 2u, 0u, write, false, false, length,
                                     buffer, length, timeout_us, NULL);
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
   uint32_t event_flags;

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
   event_flags = ((uint32_t)payload[2] << 8) | (uint32_t)payload[3];
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

   if (sdio_event_is_psk_keyed(event_type, event_status))
      g_runtime_psk_keyed = true;

   if (event_type == 16u && (event_flags & WLC_EVENT_MSG_LINK) != 0u)
      g_runtime_link_flag_trusted = true;

   if (sdio_event_is_link_up(event_type, event_status, event_reason))
      g_runtime_link_up = true;

   if (sdio_event_is_link_down(event_type, event_flags)) {
      g_runtime_link_up = false;
      g_runtime_psk_keyed = false;
      g_runtime_link_up_us = 0u;
   }
}

static void sdio_runtime_note_event(const uint8_t *frame, uint16_t frame_length,
                                    uint16_t ethernet_offset)
{
   uint16_t event_offset;
   uint16_t message_offset;
   uint32_t event_type;
   uint32_t event_status;
   uint32_t event_reason;
   uint32_t event_flags;

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
   event_flags = ((uint32_t)frame[message_offset + 2u] << 8)
      | (uint32_t)frame[message_offset + 3u];
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

   if (sdio_event_is_psk_keyed(event_type, event_status))
      g_runtime_psk_keyed = true;

   if (event_type == 16u && (event_flags & WLC_EVENT_MSG_LINK) != 0u)
      g_runtime_link_flag_trusted = true;

   if (sdio_event_is_link_up(event_type, event_status, event_reason))
      g_runtime_link_up = true;

   if (sdio_event_is_link_down(event_type, event_flags)) {
      g_runtime_link_up = false;
      g_runtime_psk_keyed = false;
      g_runtime_link_up_us = 0u;
   }
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
   /* static: ~1.6 KB.  Called from the cooperative poll path
      (wifi_lwip_poll -> drain_rx_frames -> poll_ethernet_frame ->
      here); the call chain is single-threaded and never re-entered,
      so the buffer can live in BSS instead of pushing 1.6 KB onto a
      stack that already carries other large transient frames. */
   /* _Alignas(4): the bcm2835 EMMC controller drains DATA one 32-bit
      word at a time, so the receive buffer must be 4-byte aligned or
      the host code's uint32_t* access can take an alignment fault on
      ARMv6.  Static uint8_t arrays only have 1-byte alignment by
      default; force 4 to match the hardware. */
   _Alignas(4) static uint8_t frame_buffer[SDIO_RUNTIME_FRAME_BUFFER_SIZE];
   uint16_t total_length;
   uint8_t channel;
   uint8_t header_length;
   uint8_t bdc_index;
   uint16_t ethernet_length;

   if (frame_length != NULL)
      *frame_length = 0u;

   total_length = hwtag[0];
   if ((uint16_t)(hwtag[0] ^ hwtag[1]) != (uint16_t)0xffffu || total_length < 12u
      || total_length > (uint16_t)SDIO_RUNTIME_MAX_FRAME_SIZE) {
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

   /* SDPCM bus flow control.  Every received frame carries the chip's current
      transmit credit window in its software header: byte 8 is the wireless
      flow-control mask, byte 9 is max_seq - the highest sequence number the
      chip will still accept.  Writing past that window overruns the chip's
      queues; measured effect of honouring it is that the SDIO data-CRC and
      CMD53 error counts under a sustained bidirectional load drop from
      hundreds to zero.  Captured on every channel because control and event
      frames refresh the window just as data frames do. */
   g_runtime_wlan_flow_control = frame_buffer[8];
   g_runtime_max_seq = frame_buffer[9];
   g_runtime_max_seq_valid = true;
   /* The chip has just told us where the window stands.  If it has room for
      our next frame and no flow-control stop, transmit is demonstrably
      alive - this is the ONLY place that may clear the TX-dead clock,
      because it is the only signal the chip itself vouches for. */
   if (!sdio_runtime_tx_window_shut())
      g_runtime_tx_shut_since_us = 0u;
   /* Any frame, any channel: proof the chip is alive and talking to us.
      Distinct from g_runtime_last_rx_us, which stamps only delivered DATA
      frames and is load-bearing for the 45 s RX-silence trigger. */
   g_runtime_last_any_rx_us = RPI_GetSystemTime() | 1u;

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

      /* Only an ioctl ERROR is worth a line now that the join works;
         a stream of status=0 acks is just noise. */
      if ((cdc_flags & CDCF_IOC_ERROR) != 0u)
         sdio_debug_log("cdc rsp cmd=%lu status=%lu ERROR",
                        (unsigned long)cdc_cmd, (unsigned long)cdc_status);

      /* Capture the chip's MAC from the cur_etheraddr GET-VAR reply.
         Matched by the exact request_id we sent so an unrelated
         GET_VAR reply that happens to land in the same window cannot
         be mistaken for the MAC.  The 6-byte MAC is the first thing
         after the CDC header. */
      {
         uint16_t cdc_request_id =
            (uint16_t)((cdc_flags & CDCF_IOC_ID_MASK) >> CDCF_IOC_ID_SHIFT);
         if (g_runtime_mac_request_pending && cdc_cmd == WLC_GET_VAR
             && cdc_request_id == g_runtime_mac_request_id
             && !(cdc_flags & CDCF_IOC_ERROR)
             && total_length >= (uint16_t)(header_length + CDC_HEADER_LENGTH + 6u)) {
            memcpy(g_runtime_chip_mac,
                   &frame_buffer[header_length + CDC_HEADER_LENGTH], 6u);
            g_runtime_chip_mac_valid = true;
            g_runtime_mac_request_pending = false;
         }
         /* SET cur_etheraddr ack: same request_id correlation as
            GET_MAC.  Record both the seen-bit and the firmware's
            status word so the SET_MAC stage can log a clear warning
            if the chip rejected the override (vs. accepted-but-
            ignored, which the seen-bit alone wouldn't surface). */
         if (g_runtime_set_mac_request_pending && cdc_cmd == WLC_SET_VAR
             && cdc_request_id == g_runtime_set_mac_request_id) {
            g_runtime_set_mac_ack_status = cdc_status;
            g_runtime_set_mac_ack_seen = true;
            g_runtime_set_mac_request_pending = false;
         }
         /* WLC_GET_RSSI reply: a signed int32 dBm in the first 4 bytes
            after the CDC header.  Same request_id correlation as the
            MAC so a stray reply in the window can't be misread. */
         if (g_runtime_rssi_request_pending && cdc_cmd == WLC_GET_RSSI
             && cdc_request_id == g_runtime_rssi_request_id
             && !(cdc_flags & CDCF_IOC_ERROR)
             && total_length >= (uint16_t)(header_length + CDC_HEADER_LENGTH + 4u)) {
            g_runtime_rssi = (int32_t)sdio_load_u32_le(
               &frame_buffer[header_length + CDC_HEADER_LENGTH]);
            g_runtime_rssi_valid = true;
            g_runtime_rssi_request_pending = false;
         }
         /* WLC_GET_PM reply: the chip's current power-save mode as a u32. */
         if (g_runtime_pm_request_pending && cdc_cmd == WLC_GET_PM
             && cdc_request_id == g_runtime_pm_request_id
             && !(cdc_flags & CDCF_IOC_ERROR)
             && total_length >= (uint16_t)(header_length + CDC_HEADER_LENGTH + 4u)) {
            g_runtime_pm_value = (int32_t)sdio_load_u32_le(
               &frame_buffer[header_length + CDC_HEADER_LENGTH]);
            g_runtime_pm_value_valid = true;
            g_runtime_pm_request_pending = false;
         }
         /* WLC_GET_PKTCNTS reply: five little-endian u32 counters, matched by
            request id like the others. */
         if (g_runtime_pktcnt_request_pending && cdc_cmd == WLC_GET_PKTCNTS
             && cdc_request_id == g_runtime_pktcnt_request_id
             && !(cdc_flags & CDCF_IOC_ERROR)
             && total_length >= (uint16_t)(header_length + CDC_HEADER_LENGTH
                                           + WLC_PKTCNTS_BYTES)) {
            unsigned int i;

            for (i = 0u; i < 5u; ++i)
               g_runtime_pktcnt[i] = sdio_load_u32_le(
                  &frame_buffer[header_length + CDC_HEADER_LENGTH + (i * 4u)]);
            g_runtime_pktcnt_valid = true;
            g_runtime_pktcnt_request_pending = false;
         }

         /* WLC_GET_RATE reply: one little-endian int32, units of
            500 kbit/s (-1 = auto/unknown). */
         if (g_runtime_rate_request_pending && cdc_cmd == WLC_GET_RATE
             && cdc_request_id == g_runtime_rate_request_id
             && !(cdc_flags & CDCF_IOC_ERROR)
             && total_length >= (uint16_t)(header_length + CDC_HEADER_LENGTH + 4u)) {
            g_runtime_rate_500kbps = (int32_t)sdio_load_u32_le(
               &frame_buffer[header_length + CDC_HEADER_LENGTH]);
            g_runtime_rate_valid = true;
            g_runtime_rate_request_pending = false;
         }
      }
      return false;
   }

   if (channel == SDPCM_EVENT_CHANNEL) {
      /* Async event frame layout, per cyw43-driver sdpcm_process_rx_packet
         (ASYNCEVENT_HEADER):
           SDPCM header    header_length bytes
           BDC header      4 bytes (flags, priority, flags2, data_offset)
           data_offset*4   padding bytes
           Ethernet frame  dst[6] src[6] type[2]  (type = ETHER_TYPE_BRCM)
           BRCM event      event_msg, 24 bytes into the Ethernet frame
         The previous code read the ethertype at header_length+12 -
         ignoring the BDC header AND its data_offset - so every real
         event frame mis-parsed, failed the ethertype check, and was
         silently dropped by note_event_bare's event_type sanity check.
         That is why join events never surfaced. */
      uint16_t payload_length = (total_length > header_length)
         ? (uint16_t)(total_length - header_length) : 0u;

      if (payload_length >= 4u) {
         uint8_t bdc_data_offset = frame_buffer[header_length + 3u];
         uint16_t ethernet_offset = (uint16_t)((unsigned)header_length + 4u
            + ((unsigned)bdc_data_offset << 2));

         if ((uint16_t)(ethernet_offset + 14u) <= total_length) {
            uint16_t ev_ethertype =
               (uint16_t)(((uint16_t)frame_buffer[ethernet_offset + 12u] << 8)
                  | (uint16_t)frame_buffer[ethernet_offset + 13u]);
            if (ev_ethertype == ETHER_TYPE_BRCM) {
               sdio_runtime_note_event(frame_buffer, total_length, ethernet_offset);
               return false;
            }
            /* Not the BRCM ethertype at the computed offset - log it
               (instead of dropping silently) so a remaining offset
               mismatch is visible rather than invisible. */
            sdio_debug_log("ev: ch=1 ethertype=0x%04x ethoff=%u hdr=%u tot=%u",
                           (unsigned)ev_ethertype, (unsigned)ethernet_offset,
                           (unsigned)header_length, (unsigned)total_length);
         }
      }

      /* Fallback: a few firmware builds deliver the event_msg with no
         BDC/Ethernet wrapper.  note_event_bare has its own event_type
         sanity check, so a mis-parse here is harmless. */
      if (payload_length >= BRCM_EVENT_MSG_LENGTH)
         sdio_runtime_note_event_bare(&frame_buffer[header_length], payload_length);
      else
         sdio_debug_log("ev: short ch=1 hdr=%u tot=%u",
                        (unsigned)header_length, (unsigned)total_length);
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
   /* The Ethernet frame begins after the SDPCM header, the 4-byte BDC
      header, AND bdc.data_offset*4 bytes of BDC padding.  The earlier
      code used a fixed header_length+4 (data_offset assumed 0); when the
      chip pads a received data frame that mis-frames every packet, so
      lwIP drops them all and DHCP can never complete.  data_offset is
      byte 3 of the BDC header - the same field the channel-1 event path
      now honours (cyw43-driver sdpcm_process_rx_packet). */
   {
      uint8_t bdc_data_offset = frame_buffer[header_length + 3u];
      uint16_t eth_offset = (uint16_t)((unsigned)header_length + 4u
         + ((unsigned)bdc_data_offset << 2));

      if ((uint16_t)(eth_offset + 14u) > total_length) {
         sdio_debug_log("fn2: drop ch=2 short hdr=%u tot=%u doff=%u",
                        (unsigned)header_length, (unsigned)total_length,
                        (unsigned)bdc_data_offset);
         return false;
      }

      uint16_t ethertype = (uint16_t)(((uint16_t)frame_buffer[eth_offset + 12u] << 8)
         | (uint16_t)frame_buffer[eth_offset + 13u]);
      ethernet_length = (uint16_t)(total_length - eth_offset);

      if (ethertype == ETHER_TYPE_BRCM) {
         /* A BRCM async event delivered on the data channel. */
         sdio_runtime_note_event(frame_buffer, total_length, eth_offset);
         return false;
      }

      if ((uint8_t)(frame_buffer[bdc_index] >> BDC_VERSION_SHIFT) != BDC_PROTOCOL_VERSION)
         return false;

      if (frame == NULL || frame_capacity < ethernet_length) {
         sdio_runtime_set_error("Ethernet frame exceeds receive buffer");
         return false;
      }

      memcpy(frame, &frame_buffer[eth_offset], ethernet_length);
   }
   if (frame_length != NULL)
      *frame_length = ethernet_length;
   ++g_runtime_rx_frame_count;
   g_runtime_bus_active_us = RPI_GetSystemTime();
   g_runtime_last_rx_us = g_runtime_bus_active_us;
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
   /* static + _Alignas(4): same single-threaded poll-path rationale
      as the buffer in complete_read_ethernet_frame_timeout, plus the
      4-byte alignment the EMMC PIO transfer needs. */
   _Alignas(4) static uint8_t scratch[SDIO_RUNTIME_FRAME_BUFFER_SIZE];
   uint8_t i;
   uint8_t consumed = 0u;

   for (i = 0u; i < SDIO_RUNTIME_MAX_RX_FRAMES_PER_POLL; ++i) {
      /* _Alignas(4): filled by a 32-bit EMMC PIO read.
         The SDPCM hardware header is two little-endian uint16_ts on
         the wire; this project targets little-endian ARM Pi cores
         (ARM1176 and Cortex-A), so reading them as native uint16_t
         and comparing hwtag[0] ^ hwtag[1] == 0xFFFF works directly.
         If the codebase is ever ported to a big-endian host, decode
         the bytes via sdio_load_u16_le() helpers instead. */
      _Alignas(4) uint16_t hwtag[2] = { 0u, 0u };
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
                                                       (uint16_t)SDIO_RUNTIME_MAX_FRAME_SIZE,
                                                       &scratch_length);
      ++consumed;
   }

   return consumed;
}

static bool sdio_probe_send_single_tx_control_template_timeout(sdio_host_t *dev,
                                                               sdio_probe_result_t *probe_result,
                                                               uint32_t timeout_us)
{
   _Alignas(4) uint8_t tx_frame[SDPCM_CONTROL_EVENT_HEADER_LENGTH + CDC_HEADER_LENGTH + TX_CONTROL_TEMPLATE_MAX_PAYLOAD_LENGTH]; // drained by 32-bit EMMC PIO writes
   sdio_cmd53_result_t cmd53_result;

   if (dev == NULL || probe_result == NULL || !probe_result->tx_control_template_ready)
      return false;

   memset(tx_frame, 0, sizeof(tx_frame));
   memset(&cmd53_result, 0, sizeof(cmd53_result));
   probe_result->tx_control_probe_attempted = true;

   sdio_store_u16_le(&tx_frame[0], probe_result->tx_control_template_frame_size);
   sdio_store_u16_le(&tx_frame[2], probe_result->tx_control_template_frame_size_complement);
   probe_result->tx_control_template_sequence = sdio_next_sdpcm_sequence();
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
      /* Nothing reached the chip, so give the number back rather than leave a
         gap the chip would credit against a frame it never saw. */
      /* Reclaim the number ONLY when the command phase timed out - nothing
         reached the card, so the number is provably unused.  Any later
         failure (response CRC, data-phase error) may have delivered the
         frame: reclaiming then re-sends a consumed number, which the chip
         discards without crediting - one silent step toward a permanently
         shut window per occurrence. */
      if (sdio_host_last_failure_precommand())
         --g_sdpcm_tx_sequence;
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
         comes up. The base 10 ms is comfortably above the BCM43430's
         worst case ack latency at 25 MHz; 5 ms turned out to race
         with iovars that have to touch SOC memory (PMK, event_msgs).
         sdio_tx_probe_post_delay_us() additionally restores PicoWi's
         150 ms radio/PHY settle after the AMPDU block and the 50 ms
         pauses after events_enable and mcast_list. */
      {
         uint32_t post_delay_us = sdio_tx_probe_post_delay_us(join_commands[index]);

         usleep(post_delay_us);
         (void)sdio_drain_fn2_responses(dev);   /* per-step log removed - join works */
      }
   }

   return true;
}

/* CLM (Country Locale Matrix) download.  The CLM is the chip's regulatory
   database.  Without it the BCM43430 firmware only has the "#n" worldwide
   placeholder locale, rejects every "country" iovar SET with BCME_BADARG,
   and leaves the radio in WL_RADIO_COUNTRY_DISABLE so the interface can
   never come up.  The CLM is delivered to the already-running firmware in
   chunks via the "clmload" iovar, each chunk prefixed by a 12-byte
   download header (matches brcmfmac brcmf_dload_data_le and cyw43-driver
   cyw43_clm_load):
       u16 flag        (DLOAD_HANDLER_VER<<12) | DL_BEGIN/DL_END
       u16 dload_type  DL_TYPE_CLM = 2
       u32 len         chunk byte count
       u32 crc         0 (the firmware does not check it)
   The blob is optional: a missing file is not an error (the 43430 has a
   minimal built-in CLM), so this never fails the boot - it just logs and
   returns. */
#define SDIO_CLM_DLOAD_FLAG_VER   0x1000u   /* DLOAD_HANDLER_VER(1) << DLOAD_FLAG_VER_SHIFT(12) */
#define SDIO_CLM_DLOAD_FLAG_BEGIN 0x0002u   /* DL_BEGIN */
#define SDIO_CLM_DLOAD_FLAG_END   0x0004u   /* DL_END   */
#define SDIO_CLM_DLOAD_TYPE       2u        /* DL_TYPE_CLM */
#define SDIO_CLM_CHUNK_LEN        1024u

/* Per-tick boot state for the non-blocking join.  The CLM chunk
   download, the MAC read and the 38-step join used to run inside a
   single sdio_runtime_tick() call (~0.9 s, stalling the main 1 MHz
   loop the whole time).  They now advance one step per tick, the
   inter-step settle delays becoming deadline checks.  step_sent /
   step_deadline_us are shared by the three phases, which run strictly
   one after another (CLM -> MAC -> JOIN). */
static wifi_sdio_tx_probe_command_t g_runtime_join_commands[TX_CONTROL_PROBE_JOIN_COMMAND_COUNT];
static uint8_t  g_runtime_join_count;
static uint8_t  g_runtime_join_index;
static uint32_t g_runtime_clm_offset;
static bool     g_runtime_step_sent;
static uint32_t g_runtime_step_deadline_us;

/* Send one CLM clmload chunk per call.  Returns 1 when the whole blob
   is downloaded (or there is no blob), 0 while more work remains, -1 on
   a transfer error.  See the clmload framing notes above. */
static int sdio_runtime_clm_download_step(sdio_host_t *dev)
{
   /* static: ~1.6 KB.  Each tick rebuilds the next CLM chunk; the
      step runs from the cooperative poll path and is not re-entered,
      so the buffer can be reused across ticks from BSS. */
   _Alignas(4) static uint8_t tx_frame[SDIO_RUNTIME_FRAME_BUFFER_SIZE];
   static const char clm_iovar[] = "clmload";
   const uint16_t name_length = (uint16_t)sizeof(clm_iovar); /* "clmload" + NUL = 8 */
   uint32_t clm_length = g_cyw43_clm_length;
   uint32_t chunk_len;
   uint16_t flag;
   uint32_t now;

   if (dev == NULL)
      return -1;
   if (g_cyw43_clm_data == NULL || clm_length == 0u)
      return 1;                       /* no clm_blob - use built-in regulatory */
   if (g_runtime_clm_offset >= clm_length)
      return 1;                       /* done */

   chunk_len = clm_length - g_runtime_clm_offset;
   if (chunk_len > SDIO_CLM_CHUNK_LEN)
      chunk_len = SDIO_CLM_CHUNK_LEN;
   flag = SDIO_CLM_DLOAD_FLAG_VER;
   if (g_runtime_clm_offset == 0u)
      flag |= SDIO_CLM_DLOAD_FLAG_BEGIN;
   if (g_runtime_clm_offset + chunk_len >= clm_length)
      flag |= SDIO_CLM_DLOAD_FLAG_END;

   now = RPI_GetSystemTime();

   if (!g_runtime_step_sent) {
      /* CDC payload = "clmload\0" + 12-byte dload header + chunk bytes. */
      uint16_t payload_length = (uint16_t)(name_length + 12u + chunk_len);
      uint16_t frame_size = (uint16_t)(SDPCM_CONTROL_EVENT_HEADER_LENGTH
         + CDC_HEADER_LENGTH + payload_length);
      uint16_t request_id = sdio_next_tx_probe_request_id();
      uint32_t cdc_flags;
      uint8_t *payload;

      memset(tx_frame, 0, sizeof(tx_frame));
      sdio_store_u16_le(&tx_frame[0], frame_size);
      sdio_store_u16_le(&tx_frame[2], (uint16_t)~frame_size);
      tx_frame[4] = sdio_next_sdpcm_sequence();
      tx_frame[5] = SDPCM_CONTROL_CHANNEL;
      tx_frame[7] = SDPCM_CONTROL_EVENT_HEADER_LENGTH;
      sdio_store_u32_le(&tx_frame[12], WLC_SET_VAR);
      sdio_store_u32_le(&tx_frame[16], (uint32_t)payload_length);
      cdc_flags = ((uint32_t)request_id << CDCF_IOC_ID_SHIFT)
         | ((uint32_t)TX_CONTROL_TEMPLATE_INTERFACE << CDCF_IOC_IF_SHIFT)
         | CDCF_IOC_SET;
      sdio_store_u32_le(&tx_frame[20], cdc_flags);
      sdio_store_u32_le(&tx_frame[24], 0u);
      payload = &tx_frame[SDPCM_CONTROL_EVENT_HEADER_LENGTH + CDC_HEADER_LENGTH];
      memcpy(payload, clm_iovar, name_length);
      sdio_store_u16_le(&payload[name_length + 0u], flag);
      sdio_store_u16_le(&payload[name_length + 2u], (uint16_t)SDIO_CLM_DLOAD_TYPE);
      sdio_store_u32_le(&payload[name_length + 4u], chunk_len);
      sdio_store_u32_le(&payload[name_length + 8u], 0u);  /* crc - unused */
      memcpy(&payload[name_length + 12u],
             &g_cyw43_clm_data[g_runtime_clm_offset], chunk_len);

      if (!sdio_function2_transfer_timeout(dev, true, tx_frame, frame_size,
                                           SDIO_COMMAND_TIMEOUT_US)) {
         /* Reclaim the number ONLY when the command phase timed out - nothing
         reached the card, so the number is provably unused.  Any later
         failure (response CRC, data-phase error) may have delivered the
         frame: reclaiming then re-sends a consumed number, which the chip
         discards without crediting - one silent step toward a permanently
         shut window per occurrence. */
      if (sdio_host_last_failure_precommand())
         --g_sdpcm_tx_sequence;    /* chunk never landed - reclaim its number */
      sdio_debug_log("CLM: chunk transfer failed at offset %lu",
                     (unsigned long)g_runtime_clm_offset);
      return -1;
      }
      g_runtime_step_sent = true;
      /* The final (DL_END) chunk makes the firmware ingest the whole
         CLM, which takes longer than a normal iovar. */
      g_runtime_step_deadline_us = now
         + (((flag & SDIO_CLM_DLOAD_FLAG_END) != 0u) ? 50000u : 10000u);
      return 0;
   }

   if ((int32_t)(now - g_runtime_step_deadline_us) < 0)
      return 0;                       /* chunk still settling */

   (void)sdio_drain_fn2_responses(dev);
   g_runtime_clm_offset += chunk_len;
   g_runtime_step_sent = false;
   return (g_runtime_clm_offset >= clm_length) ? 1 : 0;
}

/* Push wifi.c's cached desired MAC into the chip's cur_etheraddr iovar
   via WLC_SET_VAR.  No reply needs to be captured - the subsequent
   QUERY_MAC stage reads the current MAC back and that is what we hand
   to lwIP.  Returns 1 immediately when no MAC is cached (stage is a
   no-op), 0 while the SET is in flight, 1 when settled. */
static int sdio_runtime_set_mac_step(sdio_host_t *dev)
{
   uint32_t now;

   if (dev == NULL)
      return 1;
   if (!g_runtime_desired_mac_valid)
      return 1;                       /* nothing to do - keep OTP MAC */

   now = RPI_GetSystemTime();

   if (!g_runtime_step_sent) {
      bool sent;
      g_runtime_set_mac_ack_seen = false;
      g_runtime_set_mac_ack_status = 0u;
      sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                       WIFI_SDIO_TX_PROBE_COMMAND_SET_MAC);
      /* Match the SET ack by exact request_id, the same way the
         GET_MAC capture does, so an unrelated SET_VAR reply that
         lands in the same window can't be mistaken for ours. */
      g_runtime_set_mac_request_id =
         g_sdio_probe_result.tx_control_template_request_id;
      g_runtime_set_mac_request_pending = true;
      sent = sdio_probe_send_single_tx_control_template_timeout(dev,
                                                                &g_sdio_probe_result,
                                                                SDIO_RUNTIME_POLL_TIMEOUT_US);
      if (!sent) {
         g_runtime_set_mac_request_pending = false;
         sdio_debug_log("SET_MAC: send failed; chip will keep its OTP MAC");
         sdio_runtime_set_error(NULL); /* tolerated */
         g_runtime_step_sent = false;
         return 1;
      }
      g_runtime_step_sent = true;
      g_runtime_step_deadline_us = now + 10000u;
      return 0;
   }

   if ((int32_t)(now - g_runtime_step_deadline_us) < 0)
      return 0;

   (void)sdio_drain_fn2_responses(dev);
   g_runtime_set_mac_request_pending = false;
   g_runtime_step_sent = false;

   if (!g_runtime_set_mac_ack_seen) {
      sdio_debug_log("SET_MAC: no ack within 10 ms; chip MAC may be unchanged");
   } else if (g_runtime_set_mac_ack_status != 0u) {
      sdio_debug_log("SET_MAC: chip rejected cur_etheraddr (status=%lu); keeping OTP MAC",
                     (unsigned long)g_runtime_set_mac_ack_status);
   } else {
      sdio_debug_log("SET_MAC: cur_etheraddr <- %02x:%02x:%02x:%02x:%02x:%02x (accepted)",
                     (unsigned)g_runtime_desired_mac[0], (unsigned)g_runtime_desired_mac[1],
                     (unsigned)g_runtime_desired_mac[2], (unsigned)g_runtime_desired_mac[3],
                     (unsigned)g_runtime_desired_mac[4], (unsigned)g_runtime_desired_mac[5]);
   }
   return 1;
}

/* Read the chip's cur_etheraddr MAC across ticks: send the GET, wait a
   short settle, then drain (the CDC decoder captures the MAC).  Returns
   1 when done, 0 while in progress. */
static int sdio_runtime_query_mac_step(sdio_host_t *dev)
{
   uint32_t now;

   if (dev == NULL)
      return 1;

   now = RPI_GetSystemTime();

   if (!g_runtime_step_sent) {
      g_runtime_chip_mac_valid = false;
      sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                       WIFI_SDIO_TX_PROBE_COMMAND_GET_MAC);
      /* Remember the exact request_id the template carries so the
         CDC decoder matches only the reply to THIS GET_VAR rather
         than any GET_VAR that happens to arrive in the window. */
      g_runtime_mac_request_id =
         g_sdio_probe_result.tx_control_template_request_id;
      g_runtime_mac_request_pending = true;
      if (!sdio_probe_send_single_tx_control_template_timeout(dev,
                                                              &g_sdio_probe_result,
                                                              SDIO_RUNTIME_POLL_TIMEOUT_US)) {
         sdio_runtime_set_error(NULL); /* tolerated - lwIP keeps its default MAC */
      }
      g_runtime_step_sent = true;
      g_runtime_step_deadline_us = now + 10000u;
      return 0;
   }

   if ((int32_t)(now - g_runtime_step_deadline_us) < 0)
      return 0;

   (void)sdio_drain_fn2_responses(dev);
   g_runtime_mac_request_pending = false;
   g_runtime_step_sent = false;
   return 1;
}

/* Send one join ioctl per call: prepare g_runtime_join_commands[index],
   send it, wait out that command's settle delay, drain the reply, then
   advance.  Returns 1 when the whole sequence is sent, 0 while more
   remain, -1 on a send error. */
static int sdio_runtime_join_step(sdio_host_t *dev)
{
   uint32_t now;

   if (dev == NULL)
      return -1;
   if (g_runtime_join_index >= g_runtime_join_count)
      return 1;

   now = RPI_GetSystemTime();

   if (!g_runtime_step_sent) {
      sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                       g_runtime_join_commands[g_runtime_join_index]);
      g_sdio_probe_result.tx_control_probe_last_command =
         g_sdio_probe_result.tx_control_template_command;
      if (!sdio_probe_send_single_tx_control_template(dev, &g_sdio_probe_result))
         return -1;
      g_runtime_step_sent = true;
      g_runtime_step_deadline_us = now
         + sdio_tx_probe_post_delay_us(g_runtime_join_commands[g_runtime_join_index]);
      return 0;
   }

   if ((int32_t)(now - g_runtime_step_deadline_us) < 0)
      return 0;                       /* command still settling */

   (void)sdio_drain_fn2_responses(dev);
   g_runtime_join_index++;
   g_runtime_step_sent = false;
   return (g_runtime_join_index >= g_runtime_join_count) ? 1 : 0;
}

/* Per-tick form of the post-firmware SDPCM interrupt-ack handshake.
   The blocking sdio_probe_ack_interrupts() polls INT_STATUS up to 30
   times with a 10 ms usleep between polls - as much as 300 ms inside a
   single poll callback.  This spreads those same 30 polls across ticks
   (about one poll per 10 ms of wall-clock) so the cooperative 1 MHz loop
   is never stalled.  Returns 1 = done, 0 = in progress, -1 = bus error.
   The blocking version is retained for the diagnostic probe path. */
static int sdio_runtime_ack_interrupts_step(sdio_host_t *dev)
{
   static bool         started;
   static unsigned int round_count;
   static unsigned int no_hmb;
   static uint32_t     poll_deadline_us;
   uint32_t            now;

   if (dev == NULL)
      return -1;

   now = RPI_GetSystemTime();

   if (!started) {
      uint32_t ack_value = g_sdio_probe_result.sdio_int_status
                         & SDIO_HOST_INTERRUPT_MASK;

      g_sdio_probe_result.sdio_interrupt_ack_value = ack_value;
      if (ack_value != 0u) {
         uint32_t int_status_after_ack = 0u;

         g_sdio_probe_result.interrupt_ack_attempted = true;
         if (!sdio_backplane_write_u32(dev,
                g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET,
                ack_value)
            || !sdio_backplane_read_u32(dev,
                g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET,
                &int_status_after_ack))
            return -1;
         g_sdio_probe_result.sdio_int_status_after_ack = int_status_after_ack;
         g_sdio_probe_result.interrupt_ack_success = true;
         (void)sdio_backplane_write_u32(dev,
                g_runtime_sdio_core_base + SDIO_CORE_TO_SB_MAILBOX_OFFSET,
                0x00000002u);   /* SMB_INT_ACK */
      }
      round_count = 0u;
      no_hmb = 0u;
      poll_deadline_us = now + 10000u;   /* first HMB poll in ~10 ms */
      started = true;
      return 0;
   }

   if ((int32_t)(now - poll_deadline_us) < 0)
      return 0;                          /* still inside the 10 ms gap */

   {
      uint32_t int_status = 0u;

      if (!sdio_backplane_read_u32(dev,
             g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET,
             &int_status)) {
         started = false;
         return 1;        /* bus read failed: stop, as the old loop did */
      }
      if ((int_status & SDIO_HOST_INTERRUPT_MASK) != 0u) {
         uint32_t hmb_data = 0u;

         no_hmb = 0u;
         (void)sdio_backplane_write_u32(dev,
                g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET,
                int_status);
         (void)sdio_backplane_read_u32(dev,
                g_runtime_sdio_core_base + SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET,
                &hmb_data);
         (void)sdio_backplane_write_u32(dev,
                g_runtime_sdio_core_base + SDIO_CORE_TO_SB_MAILBOX_OFFSET,
                0x00000002u);   /* SMB_INT_ACK */
         if ((hmb_data & 0x00000002u) != 0u) {   /* FWREADY */
            started = false;
            return 1;
         }
      } else {
         if (++no_hmb >= 3u) {            /* ~30 ms with no activity */
            started = false;
            return 1;
         }
      }
   }

   if (++round_count >= 30u) {            /* original 30-poll cap */
      started = false;
      return 1;
   }
   poll_deadline_us = now + 10000u;
   return 0;
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
      || !sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET, &int_status)
      || !sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_TO_SB_MAILBOX_OFFSET, &to_sb_mailbox)
      || !sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET, &to_host_mailbox_data)) {
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
   uint32_t window = address >> 8;
   uint8_t addr_low = (uint8_t)(window & 0xffu);
   uint8_t addr_mid = (uint8_t)((window >> 8) & 0xffu);
   uint8_t addr_high = (uint8_t)((window >> 16) & 0xffu);

   /* Always reprogram.  This used to cache the last window and skip the three
      CMD52s when it had not changed - which is what Linux brcmfmac does with
      sdiodev->sbwad - but on this chip something moves the window behind the
      driver's back, so a cached belief goes stale and every later backplane
      access lands at the wrong address.  Nothing errors: the CMD52s and CMD53
      all succeed, they just read and write the wrong registers, so the mailbox
      handshake in the RX poll silently stops working and inbound frames are
      delivered late.

      Measured, idle ping over 40 packets, three runs each: with the cache
      92-260 ms average and ~3 s worst case; without it 8-39 ms average, 0%
      loss.  Masking the cache key to the true 32 KB window - the granularity
      brcmfmac uses - made it no better (25-133 ms, 4 s worst case), which is
      the tell: a coarser key means more hits, more skipped reprogramming and
      more staleness.  Until what moves the window is understood, do not
      reintroduce this. */
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

   probe_result->sdio_core_base = g_runtime_sdio_core_base;

   if (!sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET, &int_status)
      || !sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_INT_HOST_MASK_OFFSET, &int_host_mask)
      || !sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_TO_SB_MAILBOX_OFFSET, &to_sb_mailbox)
      || !sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET, &to_host_mailbox_data)) {
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
      if (!sdio_backplane_write_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET, ack_value)
         || !sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET, &int_status_after_ack)) {
         return false;
      }
      probe_result->sdio_int_status_after_ack = int_status_after_ack;
      probe_result->interrupt_ack_success = true;

      /* The snapshot clearing above only acks the INT_STATUS bit but does NOT
         send SMB_INT_ACK to TO_SB_MAILBOX.  The firmware is now stalled waiting
         for SMB_INT_ACK before it will proceed to DEVREADY→FWREADY (round 2).
         Send it now so the firmware can continue. */
      (void)sdio_backplane_write_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_TO_SB_MAILBOX_OFFSET,
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
      if (!sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET,
                                   &int_status))
         break;
      if (int_status & SDIO_HOST_INTERRUPT_MASK) {
         uint32_t hmb_data = 0u;
         no_hmb_count = 0u;
         (void)sdio_backplane_write_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET,
                                        int_status);
         (void)sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET,
                                       &hmb_data);
         (void)sdio_backplane_write_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_TO_SB_MAILBOX_OFFSET,
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
   if (!sdio_backplane_write_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_INT_HOST_MASK_OFFSET,
                                 SDIO_HOST_INTERRUPT_MASK)
      || !sdio_backplane_read_u32(dev, g_runtime_sdio_core_base + SDIO_CORE_INT_HOST_MASK_OFFSET,
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

      probe_result->sdpcm_bdc_flags = probe_result->sdpcm_post_header_prefix0;
      probe_result->sdpcm_bdc_priority = probe_result->sdpcm_post_header_prefix1;
      probe_result->sdpcm_bdc_flags2 = probe_result->sdpcm_post_header_prefix2;
      probe_result->sdpcm_bdc_data_offset = probe_result->sdpcm_post_header_prefix3;
      probe_result->sdpcm_bdc_version = (uint8_t)(probe_result->sdpcm_bdc_flags >> BDC_VERSION_SHIFT);
      probe_result->sdpcm_bdc_version_valid = probe_result->sdpcm_bdc_version == BDC_PROTOCOL_VERSION;
      probe_result->sdpcm_bdc_data_offset_bytes = (uint8_t)(probe_result->sdpcm_bdc_data_offset << 2);
      bytes_after_bdc = (uint16_t)((unsigned)probe_result->frame_header_size - probe_result->sdpcm_expected_header_length - SDPCM_PREFIX_LENGTH);
      probe_result->sdpcm_bdc_data_offset_sane = probe_result->sdpcm_bdc_data_offset_bytes <= bytes_after_bdc;
      probe_result->sdpcm_bdc_header_decoded = true;
      probe_result->sdpcm_data_ethertype_probe_attempted = true;
      ethertype_read_count = (uint16_t)(probe_result->sdpcm_bdc_data_offset_bytes + ETHERNET_HEADER_LENGTH);
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
               uint16_t brcm_event_read_count = (uint16_t)(probe_result->sdpcm_bdc_data_offset_bytes
                  + ETHERNET_HEADER_LENGTH + BRCM_EVENT_HEADER_LENGTH);
               if (brcm_event_read_count <= bytes_after_bdc
                  && brcm_event_read_count <= (uint16_t)sizeof(ethertype_buffer)) {
                  memset(ethertype_buffer, 0, sizeof(ethertype_buffer));
                  if (sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                                         brcm_event_read_count, ethertype_buffer,
                                         (uint32_t)brcm_event_read_count, NULL)) {
                     uint16_t event_offset = (uint16_t)(probe_result->sdpcm_bdc_data_offset_bytes + ETHERNET_HEADER_LENGTH);

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
                     uint16_t brcm_event_msg_read_count = (uint16_t)(probe_result->sdpcm_bdc_data_offset_bytes
                        + ETHERNET_HEADER_LENGTH + BRCM_EVENT_HEADER_LENGTH + BRCM_EVENT_MSG_LENGTH);
                     if (probe_result->sdpcm_brcm_event_oui_match
                        && probe_result->sdpcm_brcm_event_version_valid
                        && brcm_event_msg_read_count <= bytes_after_bdc
                        && brcm_event_msg_read_count <= (uint16_t)sizeof(ethertype_buffer)) {
                        memset(ethertype_buffer, 0, sizeof(ethertype_buffer));
                        if (sdio_cmd53_execute(dev, 2u, 0u, false, false, false,
                                               brcm_event_msg_read_count, ethertype_buffer,
                                               (uint32_t)brcm_event_msg_read_count, NULL)) {
                           uint16_t event_msg_offset = (uint16_t)(probe_result->sdpcm_bdc_data_offset_bytes
                              + ETHERNET_HEADER_LENGTH + BRCM_EVENT_HEADER_LENGTH);

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
      probe_result->sdpcm_cdc_payload_bytes_available = (uint16_t)((unsigned)probe_result->frame_header_size
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
   /* _Alignas(4): the buffer is filled by a 32-bit-wide EMMC PIO
      read, which is undefined behaviour on ARMv6 against a buffer
      with only uint16_t alignment. */
   _Alignas(4) uint16_t hwtag[2];
   _Alignas(4) uint8_t sdpcm_header[8];

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
   g_runtime_psk_keyed = false;
   g_runtime_link_up_us = 0u;
   g_runtime_tx_frame_count = 0u;
   g_runtime_rx_frame_count = 0u;
   /* Safe to rebase to 0 here and only here: this call re-powers the chip via
      WL_REG_ON, so its own accounting starts from scratch too. */
   g_sdpcm_tx_sequence = 0u;
   g_runtime_max_seq = 0u;
   g_runtime_wlan_flow_control = 0u;
   g_runtime_max_seq_valid = false;
   g_runtime_tx_stalled = false;
   g_runtime_tx_stall_since_us = 0u;
   /* Both freshness clocks, or a runtime restart inherits a dead past: a
      stale tx_shut re-fires the escalation the instant the restart
      completes, and a stale last_rx makes rx_idle_us() read minutes of
      silence on a link that has been up for a second.  The RX clocks are
      stamped with now rather than 0: rx_idle_us() reads 0 as "nothing
      yet, count as fresh", which made a restart that came up deaf look
      fresh FOREVER - the 45 s silence trigger could never fire and the
      ladder disarmed on a dead link.  Stamped, silence accrues from the
      restart itself.  (|1 keeps the stamp from aliasing the sentinel.) */
   g_runtime_tx_shut_since_us = 0u;
   g_runtime_last_rx_us = RPI_GetSystemTime() | 1u;
   g_runtime_last_any_rx_us = g_runtime_last_rx_us;
   g_runtime_tx_resync_count = 0u;
   g_runtime_rejoin_count = 0u;
   g_runtime_bus_four_bit = false;
   g_runtime_bus_high_speed = false;
   g_runtime_link_flag_trusted = false;
   g_runtime_emulator_mode = false;
   g_runtime_identify_started = false;
   g_runtime_identify_attempt = 0u;
   g_runtime_identify_deadline_us = 0u;
   memset(&g_runtime_alp_wait, 0, sizeof(g_runtime_alp_wait));
   memset(&g_runtime_kso_wait, 0, sizeof(g_runtime_kso_wait));
   memset(&g_runtime_enable_wait, 0, sizeof(g_runtime_enable_wait));
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
      {
         int enable_result = sdio_runtime_enable_functions_step(&g_runtime_device,
                                                                &g_sdio_probe_result);

         if (enable_result < 0)
            return sdio_runtime_finalize_error("WiFi SDIO function enable failed");
         if (enable_result == 0)
            return true;
         g_runtime_stage = SDIO_RUNTIME_STAGE_REQUEST_ALP;
         return true;
      }

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
         sdio_runtime_try_four_bit_bus(&g_runtime_device);
         sdio_runtime_try_high_speed(&g_runtime_device);
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
      {
         int ack_result = sdio_runtime_ack_interrupts_step(&g_runtime_device);

         if (ack_result < 0)
            return sdio_runtime_finalize_error("WiFi SDIO interrupt ack failed");
         if (ack_result == 0)
            return true;          /* still polling the HMB handshake */
         g_runtime_stage = SDIO_RUNTIME_STAGE_WRITE_INTR_MASK;
         return true;
      }

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

         /* Build the join command list once.  The CLM download, the MAC
            read and the join itself then each advance one step per tick
            (STAGE_CLM_DOWNLOAD / QUERY_MAC / JOIN) so no single poll
            callback stalls the main 1 MHz loop - this whole phase used
            to run inline here as one ~0.9 s blocking call. */
         g_runtime_join_count = sdio_tx_probe_join_commands(g_runtime_join_commands,
            sizeof(g_runtime_join_commands) / sizeof(g_runtime_join_commands[0]));
         g_runtime_join_index = 0u;
         g_runtime_clm_offset = 0u;
         g_runtime_step_sent = false;
         if (g_cyw43_clm_data != NULL && g_cyw43_clm_length != 0u)
            sdio_debug_log("== STAGE_CLM_DOWNLOAD: %lu bytes via clmload ==",
                           (unsigned long)g_cyw43_clm_length);
         else
            sdio_debug_log("== STAGE_CLM_DOWNLOAD: no clm_blob - built-in regulatory ==");
         g_runtime_stage = SDIO_RUNTIME_STAGE_CLM_DOWNLOAD;
         return true;

      case SDIO_RUNTIME_STAGE_CLM_DOWNLOAD:
      {
         int clm_result = sdio_runtime_clm_download_step(&g_runtime_device);

         if (clm_result == 0)
            return true;             /* more chunks / still settling */
         if (clm_result < 0) {
            sdio_debug_log("CLM: download failed - continuing with built-in regulatory");
            sdio_runtime_set_error(NULL); /* tolerated - a latched command error must not shadow a later real one */
         }
         else if (g_cyw43_clm_data != NULL && g_cyw43_clm_length != 0u)
            sdio_debug_log("CLM: clmload download complete (%lu bytes)",
                           (unsigned long)g_cyw43_clm_length);
         g_runtime_step_sent = false;
         if (g_runtime_desired_mac_valid)
            sdio_debug_log("== STAGE_SET_MAC: cur_etheraddr override pending ==");
         g_runtime_stage = SDIO_RUNTIME_STAGE_SET_MAC;
         return true;
      }

      case SDIO_RUNTIME_STAGE_SET_MAC:
      {
         int set_result = sdio_runtime_set_mac_step(&g_runtime_device);

         if (set_result == 0)
            return true;
         g_runtime_step_sent = false;
         g_runtime_stage = SDIO_RUNTIME_STAGE_QUERY_MAC;
         return true;
      }

      case SDIO_RUNTIME_STAGE_QUERY_MAC:
      {
         int mac_result = sdio_runtime_query_mac_step(&g_runtime_device);

         if (mac_result == 0)
            return true;
         if (g_runtime_chip_mac_valid)
            sdio_debug_log("chip MAC %02x:%02x:%02x:%02x:%02x:%02x",
                           (unsigned)g_runtime_chip_mac[0], (unsigned)g_runtime_chip_mac[1],
                           (unsigned)g_runtime_chip_mac[2], (unsigned)g_runtime_chip_mac[3],
                           (unsigned)g_runtime_chip_mac[4], (unsigned)g_runtime_chip_mac[5]);
         else
            sdio_debug_log("chip MAC read failed - lwIP netif keeps its default address");
         g_runtime_step_sent = false;
         sdio_debug_log("== STAGE_JOIN: starting join sequence ==");
         g_runtime_stage = SDIO_RUNTIME_STAGE_JOIN;
         return true;
      }

      case SDIO_RUNTIME_STAGE_JOIN:
      {
         int join_result = sdio_runtime_join_step(&g_runtime_device);

         if (join_result == 0)
            return true;             /* sending / command settling */
         g_sdio_probe_result.tx_control_probe_steps_requested = g_runtime_join_count;
         g_sdio_probe_result.tx_control_probe_steps_completed = g_runtime_join_index;
         if (join_result < 0) {
            sdio_debug_log("join command sequence failed at step %u/%u",
                           (unsigned int)g_runtime_join_index,
                           (unsigned int)g_runtime_join_count);
            sdio_runtime_set_error(NULL); /* tolerated - runtime still reaches DONE, join can be retried */
         } else
            sdio_debug_log("join command sequence sent (%u steps)",
                           (unsigned int)g_runtime_join_count);
         g_runtime_stage = SDIO_RUNTIME_STAGE_SWEEP_RX;
         return true;
      }

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

/* True only once the bring-up state machine has run all the way to
   STAGE_DONE.  Not the same thing as sdio_runtime_started(): that goes
   true at WRITE_INTR_MASK - "firmware booted" - five stages before the
   CLM download, MAC programming and join have run, and it STAYS true if
   a later stage finalizes to STAGE_ERROR.  Anything deciding whether a
   restart or boot actually finished must use this, not started(). */
bool sdio_runtime_ready(void)
{
   return g_runtime_stage == SDIO_RUNTIME_STAGE_DONE;
}

/* Re-arm the join sequence on a runtime that has already finished bring-up.
   The chip does not re-associate on its own once the association is lost, and
   the boot path runs the join exactly once, so without this a scan that comes
   back empty at boot - or an AP that reboots an hour later - leaves the Pi off
   the network until someone power-cycles it.  Re-entering STAGE_JOIN reuses
   the whole existing sequence: the caller then drives sdio_runtime_tick()
   until it returns false, exactly as the boot path does.

   Only from STAGE_DONE, so this cannot cut across bring-up or a rejoin that
   is still running.  link_up and psk_keyed are cleared because the previous
   association is over; both are re-established from the chip's events. */
bool sdio_runtime_rejoin_start(void)
{
   if (!g_runtime_started || g_runtime_emulator_mode)
      return false;
   if (g_runtime_stage != SDIO_RUNTIME_STAGE_DONE)
      return false;

   g_runtime_join_count = sdio_tx_probe_join_commands(g_runtime_join_commands,
      sizeof(g_runtime_join_commands) / sizeof(g_runtime_join_commands[0]));
   if (g_runtime_join_count == 0u)
      return false;

   g_runtime_join_index = 0u;
   g_runtime_step_sent = false;
   g_runtime_link_up = false;
   g_runtime_psk_keyed = false;
   ++g_runtime_rejoin_count;
   sdio_debug_log("== STAGE_JOIN: rejoin attempt %lu ==",
                  (unsigned long)g_runtime_rejoin_count);
   g_runtime_stage = SDIO_RUNTIME_STAGE_JOIN;
   return true;
}

/* True while a rejoin is still working through its stages.  The caller keeps
   calling sdio_runtime_tick() until this goes false. */
/* How long since anything arrived from the chip.  Zero until the first frame,
   so a caller cannot mistake "nothing yet" for "silent for ages". */
uint32_t sdio_runtime_rx_idle_us(void)
{
   if (g_runtime_last_rx_us == 0u)
      return 0u;
   return RPI_GetSystemTime() - g_runtime_last_rx_us;
}

bool sdio_runtime_rejoin_busy(void)
{
   return g_runtime_stage == SDIO_RUNTIME_STAGE_JOIN
       || g_runtime_stage == SDIO_RUNTIME_STAGE_SWEEP_RX;
}

bool sdio_runtime_link_is_up(void)
{
   const wifi_config_t *config = wifi_get_config();

   if (!g_runtime_started || !g_runtime_link_up)
      return false;

   /* On a secured network the link is not usable until the 4-way handshake
      has keyed the port, so hold it down until WLC_E_PSK_SUP/KEYED arrives.
      Announcing it at WLC_E_LINK put a DHCP Discover into the handshake
      window, where the AP drops it. Open networks never emit the event, so
      they are not gated. */
   if (config != NULL && config->password[0] != '\0' && !g_runtime_psk_keyed) {
      /* Never let this gate be the reason there is no network. Only WPA-PSK
         was observed here; a setup that authorises without ever sending
         PSK_SUP/KEYED would otherwise stay offline forever, which is far
         worse than the dropped packet this avoids. Associated for this long
         with no keyed event: take the link as-is. */
      if (g_runtime_link_up_us == 0u)
         g_runtime_link_up_us = RPI_GetSystemTime();
      if ((RPI_GetSystemTime() - g_runtime_link_up_us) < SDIO_PSK_KEYED_GRACE_US)
         return false;
   }

   return true;
}

/* Copy the chip's WiFi MAC (read from cur_etheraddr at boot) into
   mac_out.  Returns false if it was never captured, in which case the
   caller should keep its own default. */
bool sdio_runtime_get_chip_mac(uint8_t mac_out[6])
{
   if (mac_out == NULL || !g_runtime_chip_mac_valid)
      return false;

   memcpy(mac_out, g_runtime_chip_mac, 6u);
   return true;
}

/* Request a one-shot RSSI read.  Safe to call from any context - it only
   sets a flag; no SDIO access happens here.  The read is performed later
   by sdio_runtime_rssi_poll() on the cooperative poll path (outside the
   RX drain), so this may safely be called from the /status TCP callback. */
void sdio_runtime_request_rssi(void)
{
   g_runtime_rssi_query_wanted = true;
}

void sdio_runtime_request_pktcnts(void)
{
   g_runtime_pktcnt_query_wanted = true;
}

bool sdio_runtime_get_pktcnts(uint32_t out[5])
{
   unsigned int i;

   if (out == NULL || !g_runtime_pktcnt_valid)
      return false;
   for (i = 0u; i < 5u; ++i)
      out[i] = g_runtime_pktcnt[i];
   return true;
}

/* Same shape as sdio_runtime_rssi_poll: request on one tick, let the ordinary
   RX drain deliver the reply, give up after a generous deadline. */
void sdio_runtime_pktcnts_poll(void)
{
   uint32_t now;

   if (!g_runtime_pktcnt_query_wanted)
      return;

   /* One ioctl at a time: sdio_prepare_tx_control_template() builds into the
      single shared probe-result buffer, so starting this while an RSSI read
      is in flight overwrites its request id and neither reply is ever
      matched.  RSSI is user-driven and short; wait for it. */
   if (g_runtime_rssi_query_wanted || g_runtime_rssi_request_pending)
      return;

   if (!sdio_runtime_link_is_up()) {
      g_runtime_pktcnt_query_wanted = false;
      g_runtime_pktcnt_step_sent = false;
      g_runtime_pktcnt_request_pending = false;
      return;
   }

   /* Do not burn a sequence number the chip will not credit - see the
      window-shut note above the helper.  The request keeps; the counters
      simply refresh once the window reopens. */
   if (!g_runtime_pktcnt_step_sent && sdio_runtime_tx_window_shut()) {
      g_runtime_pktcnt_request_pending = false;
      return;
   }

   now = RPI_GetSystemTime();

   if (!g_runtime_pktcnt_step_sent) {
      sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                       WIFI_SDIO_TX_PROBE_COMMAND_GET_PKTCNTS);
      g_runtime_pktcnt_request_id =
         g_sdio_probe_result.tx_control_template_request_id;
      g_runtime_pktcnt_request_pending = true;
      if (!sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                              &g_sdio_probe_result,
                                                              SDIO_RUNTIME_POLL_TIMEOUT_US)) {
         g_runtime_pktcnt_request_pending = false;
         g_runtime_pktcnt_query_wanted = false;
         return;
      }
      g_runtime_pktcnt_step_sent = true;
      g_runtime_pktcnt_deadline_us = now + 250000u;
      return;
   }

   if (!g_runtime_pktcnt_request_pending) {
      g_runtime_pktcnt_step_sent = false;
      g_runtime_pktcnt_query_wanted = false;
      return;
   }

   if ((int32_t)(now - g_runtime_pktcnt_deadline_us) < 0)
      return;

   g_runtime_pktcnt_request_pending = false;
   g_runtime_pktcnt_step_sent = false;
   g_runtime_pktcnt_query_wanted = false;
}

/* Same shape again; lowest priority of the three, so it defers to both an
   RSSI and a PKTCNTS request in flight (all share one control template). */
void sdio_runtime_rate_poll(void)
{
   uint32_t now;

   if (!g_runtime_rate_query_wanted)
      return;

   if (g_runtime_rssi_query_wanted || g_runtime_rssi_request_pending
       || g_runtime_pktcnt_query_wanted || g_runtime_pktcnt_request_pending)
      return;

   if (!sdio_runtime_link_is_up()) {
      g_runtime_rate_query_wanted = false;
      g_runtime_rate_step_sent = false;
      g_runtime_rate_request_pending = false;
      return;
   }

   if (!g_runtime_rate_step_sent && sdio_runtime_tx_window_shut()) {
      g_runtime_rate_request_pending = false;
      return;
   }

   now = RPI_GetSystemTime();

   if (!g_runtime_rate_step_sent) {
      sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                       WIFI_SDIO_TX_PROBE_COMMAND_GET_RATE);
      g_runtime_rate_request_id =
         g_sdio_probe_result.tx_control_template_request_id;
      g_runtime_rate_request_pending = true;
      if (!sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                              &g_sdio_probe_result,
                                                              SDIO_RUNTIME_POLL_TIMEOUT_US)) {
         g_runtime_rate_request_pending = false;
         g_runtime_rate_query_wanted = false;
         return;
      }
      g_runtime_rate_step_sent = true;
      g_runtime_rate_deadline_us = now + 250000u;
      return;
   }

   if (!g_runtime_rate_request_pending) {
      g_runtime_rate_step_sent = false;
      g_runtime_rate_query_wanted = false;
      return;
   }

   if ((int32_t)(now - g_runtime_rate_deadline_us) < 0)
      return;

   g_runtime_rate_request_pending = false;
   g_runtime_rate_step_sent = false;
   g_runtime_rate_query_wanted = false;
}

void sdio_runtime_request_rate(void)
{
   g_runtime_rate_query_wanted = true;
}

bool sdio_runtime_get_rate(int32_t *rate_500kbps_out)
{
   if (rate_500kbps_out == NULL || !g_runtime_rate_valid)
      return false;

   *rate_500kbps_out = g_runtime_rate_500kbps;
   return true;
}

/* Cooperative-poll worker: when a read has been requested and the link is
   up, send WLC_GET_RSSI, wait a short settle, then drain the reply into
   the cache.  Non-blocking - the send and the drain land on separate
   ticks, exactly like the boot MAC read (sdio_runtime_query_mac_step).
   Costs nothing (no bus traffic) unless a read is pending, so it is only
   ever active for a moment after /status is viewed. */
/* True while the SDPCM credit window is shut.  The optional runtime ioctls
   (RSSI, PKTCNTS, PM) consult this and simply wait: control frames share the
   one sequence space but bypass the credit gate, so sending them during a
   stall burns sequence numbers the chip will not credit - and 128 of those
   flip the gate's int8_t comparison and falsely reopen it.  The join/rejoin
   machinery is deliberately NOT gated here; it has to be able to talk to a
   chip whose window state is unknown. */
static bool sdio_runtime_tx_window_shut(void)
{
   return g_runtime_max_seq_valid
      && (g_runtime_wlan_flow_control != 0u
          || (int8_t)(g_sdpcm_tx_sequence - g_runtime_max_seq) >= 0);
}

/* Re-assert "no power save" once the link is actually up.
 *
 * The join sequence already sends WLC_SET_PM = 0, but it sends it while the
 * interface is still coming up - before WLC_UP, the country setting and the
 * association itself.  Broadcom firmware applies PM per-interface and settles
 * it again as part of associating, so the early write does not necessarily
 * survive the join, and the station ends up in PM_FAST without anything
 * failing or reporting an error.
 *
 * The symptom that led here: an idle ping averaged 531 ms with multi-second
 * outliers while the firmware's own measured request-to-reply turnaround was
 * 4 microseconds, and the chip reported no bad frames in either direction -
 * so the requests were arriving at the chip seconds late.  Under a sustained
 * download the same ping averaged 138 ms.  Fast when there is traffic to keep
 * the station awake, slow when idle, is what an access point buffering for a
 * dozing station looks like.
 *
 * Sent once per link-up, and again after a rejoin, since a fresh association
 * gets a fresh PM state.  It yields to an outstanding RSSI or PKTCNTS request
 * because all three share one control template. */
static bool g_runtime_pm_asserted;
static uint32_t g_runtime_pm_due_us;

void sdio_runtime_powersave_note_link_change(bool link_up)
{
   if (!link_up) {
      g_runtime_pm_asserted = false;
      g_runtime_pm_due_us = 0u;
      g_runtime_pm_query_wanted = false;
      g_runtime_pm_query_sent = false;
      g_runtime_pm_value_valid = false;
      return;
   }

   if (!g_runtime_pm_asserted && g_runtime_pm_due_us == 0u) {
      /* Let the association settle before writing: a PM write landing in the
         middle of the join is exactly how the original one got lost. */
      g_runtime_pm_due_us = RPI_GetSystemTime() + 2000000u;
      if (g_runtime_pm_due_us == 0u)
         g_runtime_pm_due_us = 1u;
   }
}

// cppcheck-suppress unusedFunction
void sdio_runtime_powersave_poll(void)
{
   if (g_runtime_pm_asserted || g_runtime_pm_due_us == 0u)
      return;

   if (!sdio_runtime_link_is_up())
      return;

   if (sdio_runtime_tx_window_shut())
      return;                    /* do not burn sequence numbers in a stall */

   if ((int32_t)(RPI_GetSystemTime() - g_runtime_pm_due_us) < 0)
      return;

   /* The control template is shared by RSSI, PKTCNTS and this - do not step
      on any read in flight.  Overwriting the template mid-request is what
      previously coincided with a link loss. */
   if (g_runtime_rssi_query_wanted || g_runtime_rssi_request_pending
       || g_runtime_pktcnt_query_wanted || g_runtime_pktcnt_step_sent
       || g_runtime_pktcnt_request_pending)
      return;

   sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                    WIFI_SDIO_TX_PROBE_COMMAND_POWERSAVE_OFF);
   if (!sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                           &g_sdio_probe_result,
                                                           SDIO_RUNTIME_POLL_TIMEOUT_US))
      return;                   /* try again on the next poll */

   g_runtime_pm_asserted = true;
   g_runtime_pm_query_wanted = true;   /* now confirm it actually took */

   /* Arm the in-band interrupt gate.  Here rather than at fn2-ready because
      the boot state machine calls boot_reset_state() on its way through and
      would silently disarm it.  Mask the chip's sources first or it asserts
      for everything; clear the controller's stale latch last, because DAT1
      has been flapping through bring-up and whatever it latched is history. */
   if (!g_rx_int_armed) {
      uint8_t int_enable = 0x01u | 0x04u;   /* IENM (master) + IEN2 */

      if (sdio_backplane_write_u32_timeout(&g_runtime_device,
                                           g_runtime_sdio_core_base
                                              + SDIO_CORE_HOST_INT_MASK_OFFSET,
                                           SDIO_HOST_INT_MASK_VALUE,
                                           SDIO_RUNTIME_POLL_TIMEOUT_US)
          && sdio_probe_write_byte(&g_runtime_device, SDIO_CCCR_INT_ENABLE,
                                   int_enable)) {
         sdio_host_set_card_interrupt(true);
         sdio_host_clear_card_interrupt();
         g_rx_sweep_us = RPI_GetSystemTime();
         g_rx_int_armed = true;
      }
   }
}

/* Read PM back.  Same one-shot shape as the RSSI poll, and it yields to the
   other readers for the same reason - one shared control template. */
// cppcheck-suppress unusedFunction
void sdio_runtime_powersave_verify_poll(void)
{
   /* A lost reply must not park the query forever: past the deadline the
      request is abandoned and re-armed, exactly like the RSSI poller. */
   if (g_runtime_pm_query_sent && g_runtime_pm_request_pending
       && (int32_t)(RPI_GetSystemTime() - g_runtime_pm_deadline_us) >= 0) {
      g_runtime_pm_request_pending = false;
      g_runtime_pm_query_sent = false;
   }
   if (!g_runtime_pm_query_wanted || g_runtime_pm_query_sent)
      return;
   if (!sdio_runtime_link_is_up())
      return;
   if (sdio_runtime_tx_window_shut())
      return;
   if (g_runtime_rssi_query_wanted || g_runtime_rssi_request_pending
       || g_runtime_pktcnt_query_wanted || g_runtime_pktcnt_step_sent
       || g_runtime_pktcnt_request_pending)
      return;

   sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                    WIFI_SDIO_TX_PROBE_COMMAND_GET_PM);
   g_runtime_pm_request_id = g_sdio_probe_result.tx_control_template_request_id;
   g_runtime_pm_request_pending = true;
   if (!sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                           &g_sdio_probe_result,
                                                           SDIO_RUNTIME_POLL_TIMEOUT_US)) {
      g_runtime_pm_request_pending = false;
      return;                          /* try again next poll */
   }
   g_runtime_pm_query_sent = true;
   g_runtime_pm_deadline_us = RPI_GetSystemTime() + 250000u;
}

// cppcheck-suppress unusedFunction
bool sdio_runtime_get_powersave_mode(int32_t *mode)
{
   if (!g_runtime_pm_value_valid)
      return false;
   *mode = g_runtime_pm_value;
   return true;
}

void sdio_runtime_rssi_poll(void)
{
   uint32_t now;

   if (!g_runtime_rssi_query_wanted)
      return;

   /* No link => nothing to read, and the boot state machine may still
      own the shared control-template buffer.  Drop the request. */
   if (!sdio_runtime_link_is_up()) {
      g_runtime_rssi_query_wanted = false;
      g_runtime_rssi_step_sent = false;
      g_runtime_rssi_request_pending = false;
      return;
   }

   if (!g_runtime_rssi_step_sent && sdio_runtime_tx_window_shut())
      return;                    /* wait for credit; the request keeps */

   now = RPI_GetSystemTime();

   if (!g_runtime_rssi_step_sent) {
      sdio_prepare_tx_control_template(&g_sdio_probe_result,
                                       WIFI_SDIO_TX_PROBE_COMMAND_GET_RSSI);
      g_runtime_rssi_request_id =
         g_sdio_probe_result.tx_control_template_request_id;
      g_runtime_rssi_request_pending = true;
      if (!sdio_probe_send_single_tx_control_template_timeout(&g_runtime_device,
                                                              &g_sdio_probe_result,
                                                              SDIO_RUNTIME_POLL_TIMEOUT_US)) {
         /* Send failed - abandon this request and keep any prior value. */
         g_runtime_rssi_request_pending = false;
         g_runtime_rssi_query_wanted = false;
         return;
      }
      g_runtime_rssi_step_sent = true;
      /* Generous, because this no longer fetches the reply itself - it waits
         for the ordinary RX drain to deliver it.  10 ms was inherited from
         when this drained the FIFO synchronously, and once that was removed
         it was tight enough that the request was routinely abandoned before
         the reply arrived, leaving /status showing "(querying)" for ever. */
      g_runtime_rssi_step_deadline_us = now + 250000u;
      return;
   }

   /* The reply landed if the shared completion path matched it by request id;
      pending is cleared there. */
   if (!g_runtime_rssi_request_pending) {
      g_runtime_rssi_step_sent = false;
      g_runtime_rssi_query_wanted = false;
      return;
   }

   if ((int32_t)(now - g_runtime_rssi_step_deadline_us) < 0)
      return;

   /* Deliberately NOT sdio_drain_fn2_responses() here.  That helper reads
      frames into a scratch buffer and discards them, which is right during
      bring-up but destructive once lwIP is running: every /status view would
      throw away up to eight inbound frames that happened to arrive in the
      10 ms window - the same silent-discard bug that draining events into a
      throwaway buffer used to cause, and indistinguishable from packet loss
      on the air.  Nothing is needed here anyway: the reply is matched by
      request id in the shared RX completion path, so the ordinary drain
      captures the RSSI and delivers the data frames. */
   g_runtime_rssi_request_pending = false;
   g_runtime_rssi_step_sent = false;
   g_runtime_rssi_query_wanted = false;
}

/* Return the last RSSI captured (signed dBm).  False until the first
   successful read has completed. */
bool sdio_runtime_get_rssi(int32_t *out)
{
   if (out == NULL || !g_runtime_rssi_valid)
      return false;

   *out = g_runtime_rssi;
   return true;
}

void sdio_runtime_set_desired_mac(const uint8_t mac[6])
{
   if (mac == NULL) {
      g_runtime_desired_mac_valid = false;
      memset(g_runtime_desired_mac, 0, sizeof g_runtime_desired_mac);
      return;
   }
   memcpy(g_runtime_desired_mac, mac, sizeof g_runtime_desired_mac);
   g_runtime_desired_mac_valid = true;
}

bool sdio_runtime_send_ethernet_frame(const uint8_t *frame, uint16_t frame_length)
{
   /* static: keeps this ~1.6 KB buffer off the stack.  This runs inside
      the lwIP transmit path, which is itself nested under the RX drain
      (wifi_lwip_drain_rx_frames -> lwIP -> link_output -> here), so a
      stack copy would compound an already deep call chain. */
   _Alignas(4) static uint8_t tx_frame[SDIO_RUNTIME_FRAME_BUFFER_SIZE];
   uint16_t total_length;

   if (!g_runtime_started || frame == NULL || frame_length == 0u) {
      sdio_runtime_set_error("WiFi SDIO runtime is not ready for transmit");
      return false;
   }

   /* Reject oversize frames BEFORE the (uint16_t)(18u + frame_length) cast,
      which would otherwise wrap a 65519+ payload back into a small
      total_length, slip past the size check below, and overrun the
      1.6 KB tx_frame buffer in the memcpy that follows. */
   if (frame_length > (uint16_t)(SDIO_RUNTIME_MAX_FRAME_SIZE - 18u)) {
      sdio_runtime_set_error("Ethernet frame exceeds SDIO transmit buffer");
      return false;
   }

   /* Respect the chip's credit window.  Refusing here is back-pressure, not an
      error, so it deliberately does not call sdio_runtime_set_error(): the
      sequence number is not consumed, lwIP keeps the segment on its unacked
      queue and retransmits, and the window reopens as the chip drains. */
   if (sdio_runtime_tx_window_shut()) {
      uint32_t now_us = RPI_GetSystemTime();

      if (!g_runtime_tx_stalled) {
         g_runtime_tx_stalled = true;
         g_runtime_tx_stall_since_us = now_us;
         if (g_runtime_tx_shut_since_us == 0u)
            g_runtime_tx_shut_since_us = (now_us == 0u) ? 1u : now_us;
         return false;
      }
      /* How long to wait before treating a shut window as a desync depends on
         whether anything can still reopen it.  Only a received frame refreshes
         max_seq, so when the chip's FIFO came up empty there is nothing coming
         to do that, and waiting a whole second accomplishes nothing except
         holding the frame until the queue discards it as stale.

         That is not hypothetical: on an idle link it made the Pi lose 18% of
         pings with a 3.8 s worst case and a ~1 s average, while the gateway
         from the same host lost none - the echo reply was refused, held, and
         binned at 250 ms, with only the occasional one surviving to be sent by
         the resync a second later.  Under load the FIFO is rarely empty, so
         the conservative wait still governs there, which is where overrunning
         the chip actually matters. */
      if ((uint32_t)(now_us - g_runtime_tx_stall_since_us)
             < (((int32_t)(g_runtime_last_any_rx_us
                           - g_runtime_tx_stall_since_us) < 0)
                    ? SDPCM_TX_STALL_IDLE_RESYNC_US
                    : SDPCM_TX_STALL_RESYNC_US))
         return false;

      /* Sustained shut window.  Two distinct faults land here, and only one
         of them is curable from the host side:

         - A stale flow-control stop: the chip's "pause" arrived in a frame
           header and the matching "resume" was lost.  Nothing later
           contradicts it, so the cached mask holds the gate shut forever.
           Clearing it is safe - the next received frame re-asserts the
           stop if it still holds.

         - Genuine credit exhaustion: (int8_t)(seq - max_seq) >= 0 with the
           chip advertising no more.  Everything below max_seq is sequence
           space the chip has ALREADY consumed.  The old recovery here
           rewrote g_sdpcm_tx_sequence to max_seq - 1: the chip discards a
           replayed sequence number without crediting and without advancing
           max_seq, so every later pass rebased onto the same dead number
           and transmit stayed dead forever while receive ran on (RX needs
           no host credits).  Twice that ended in a physical power cycle.

         So: clear the flow-control mask, then re-ask the gate.  If it is
         still shut the stall is real credit exhaustion, and the only exits
         run through the recovery ladder, which is already armed - the
         TX-dead clock (g_runtime_tx_shut_since_us) has been counting since
         the first refusal: rejoins from 8 s dead, and a full chip restart
         (WL_REG_ON cycle, firmware re-download, fresh sequence space on
         BOTH sides) at 25 s or three failed rejoins.  Only the restart
         cures a poisoned window - a rejoin's own commands travel through
         it and are discarded like everything else.  NEVER rewrite the
         sequence number. */
      g_runtime_wlan_flow_control = 0u;
      g_runtime_tx_stall_since_us = now_us;
      /* Refreshing stall_since IS the snapshot refresh: the idle tier
         compares the last-RX stamp against it, so each pass starts a
         fresh observation window automatically. */
      if (sdio_runtime_tx_window_shut())
         return false;      /* out of credit for real - the ladder recovers */
      ++g_runtime_tx_resync_count;   /* a stale flow-control stop was cleared */
   } else {
      g_runtime_tx_stalled = false;
   }

   /* The RX path wakes the bus before touching it, but this path never did -
      and since the interrupt gate removed the every-poll KSO side effect, an
      idle link's first transmit could land on a sleeping interface, which
      swallows the frame while the host sees success: sequence consumed, one
      credit leaked, and enough of those close the window for good.  Failing
      here consumes nothing; lwIP or the hold queue simply retries.  Under
      traffic the recent-transfer fast path makes this a time compare. */
   if (!sdio_runtime_wake_bus(&g_runtime_device))
      return false;

   total_length = (uint16_t)(18u + frame_length);

   memset(tx_frame, 0, sizeof(tx_frame));
   sdio_store_u16_le(&tx_frame[0], total_length);
   sdio_store_u16_le(&tx_frame[2], (uint16_t)~total_length);
   tx_frame[4] = sdio_next_sdpcm_sequence();
   tx_frame[5] = SDPCM_DATA_CHANNEL;
   tx_frame[6] = 0u;
   tx_frame[7] = SDPCM_DATA_HEADER_LENGTH;
   tx_frame[8] = 0u;
   tx_frame[9] = 0u;
   tx_frame[14] = (uint8_t)(BDC_PROTOCOL_VERSION << BDC_VERSION_SHIFT);
   memcpy(&tx_frame[18], frame, frame_length);

   if (!sdio_function2_transfer(&g_runtime_device, true, tx_frame, total_length)) {
      /* Give the sequence number back.  A CMD53 that never completed left no
         frame in the chip's queue, so consuming a number the chip will never
         acknowledge walks us permanently ahead of its credit window - which is
         how a run of bus errors used to end in a shut window that only the
         resync above could reopen. */
      /* Reclaim the number ONLY when the command phase timed out - nothing
         reached the card, so the number is provably unused.  Any later
         failure (response CRC, data-phase error) may have delivered the
         frame: reclaiming then re-sends a consumed number, which the chip
         discards without crediting - one silent step toward a permanently
         shut window per occurrence. */
      if (sdio_host_last_failure_precommand())
         --g_sdpcm_tx_sequence;
      /* A bus-level failure is TX-dead evidence too.  One observed wedge
         flavour never touches the credit gate at all: the chip holds DAT1
         low outside the interrupt period, the controller reports the data
         lines busy, and every CMD53 fails - so gate-based accounting reads
         "healthy" while nothing can be sent. */
      if (g_runtime_tx_shut_since_us == 0u) {
         uint32_t fail_now_us = RPI_GetSystemTime();
         g_runtime_tx_shut_since_us = (fail_now_us == 0u) ? 1u : fail_now_us;
      }
      sdio_runtime_set_error("Failed to write Ethernet frame over SDIO");
      return false;
   }

   ++g_runtime_tx_frame_count;
   g_runtime_bus_active_us = RPI_GetSystemTime();
   /* Deliberately NOT clearing g_runtime_tx_shut_since_us here.  A CMD53 the
      host sees succeed proves only that the bus took the bytes - the resync
      probe "succeeds" this way once a second while the chip discards every
      frame uncredited, and clearing on it kept the TX-dead clock forever
      young through a genuinely dead window.  Life is declared where the chip
      proves it: max_seq moving our window open, in the RX refresh path. */
   return true;
}

/* Wake the chip's SDIO interface before a bus access.  The CYW43 lets
   its SDIO backplane sleep when the host goes idle; the KSO bit set once
   during boot is only a standing request - the chip still drops the
   DEVICE_ON state, and the host must re-assert KSO and wait for
   DEVICE_ON before each access (this is what cyw43-driver does in
   cyw43_ll_bus_sleep()).  Without it the poll loop's fn2 reads land on a
   sleeping interface: the firmware's asynchronous events queued while it
   slept are missed, and eventually a CMD53 to the sleeping device times
   out.

   This is NON-BLOCKING: it never sleeps.  If the chip is already awake
   that is a single CMD52; otherwise it asserts KSO and re-checks a
   handful of times back-to-back (each CMD52 is tens of microseconds -
   enough spacing for DEVICE_ON to come up).  If it has still not woken
   it returns false WITHOUT spinning - KSO has been asserted, so the
   caller simply skips this poll and a later one finds the bus awake.
   The old version spun up to 32 ms here, stalling the main loop. */
static bool sdio_runtime_wake_bus(sdio_host_t *dev)
{
   const uint8_t awake = (uint8_t)(SDIO_SLEEP_CSR_KEEP_WL_KSO | SDIO_SLEEP_CSR_WL_DEVON);
   uint8_t status = 0u;
   uint8_t attempt;

   if (dev == NULL)
      return false;

   /* A transfer that completed microseconds ago proves the interface is
      awake, so skip the CMD52 that would ask.  KSO sleep only engages after
      the host goes idle, and the check is self-healing: if the chip somehow
      did sleep, the next transfer fails, the stamp stops advancing, and the
      following poll takes the full handshake below. */
   if ((uint32_t)(RPI_GetSystemTime() - g_runtime_bus_active_us)
          < SDIO_BUS_AWAKE_ASSUME_US)
      return true;

   if (sdio_function1_read_byte(dev, SDIO_SLEEP_CSR, &status)
       && (status & awake) == awake)
      return true;   /* already awake */

   (void)sdio_function1_write_byte(dev, SDIO_SLEEP_CSR,
                                   SDIO_SLEEP_CSR_KEEP_WL_KSO);
   for (attempt = 0u; attempt < 4u; ++attempt) {
      status = 0u;
      if (sdio_function1_read_byte(dev, SDIO_SLEEP_CSR, &status)
          && (status & awake) == awake)
         return true;
   }

   return false;
}

// cppcheck-suppress unusedFunction
void sdio_runtime_rx_gate_counts(uint32_t *skips, uint32_t *sweeps,
                                 uint32_t *missed, bool *armed, uint32_t *high)
{
   *skips = g_rx_int_skips;
   *sweeps = g_rx_sweeps;
   *missed = g_rx_int_missed;
   *armed = g_rx_int_armed;
   *high = g_rx_int_high;
}

/* Undo everything the interrupt gate told the CHIP before a warm reboot.
   WL_REG_ON stays high across a kernel.now jump, so CCCR 0x04 and HOSTINTMASK
   survive into the next kernel - which then has the chip driving DAT1 low
   into the middle of its bring-up, on the same line its firmware download
   uses.  Observed as the incoming kernel dying silently and the watchdog
   falling back to the SD kernel; masking only the controller bit was not
   enough, because the chip was still signalling.  Failures here are ignored:
   the bus may already be wedged, and the reboot must happen regardless. */
// cppcheck-suppress unusedFunction
void sdio_runtime_prepare_for_warm_reboot(void)
{
   if (g_runtime_started && !g_runtime_emulator_mode) {
      (void)sdio_probe_write_byte(&g_runtime_device, SDIO_CCCR_INT_ENABLE, 0u);
      (void)sdio_backplane_write_u32_timeout(&g_runtime_device,
                                             g_runtime_sdio_core_base
                                                + SDIO_CORE_HOST_INT_MASK_OFFSET,
                                             0u, SDIO_RUNTIME_POLL_TIMEOUT_US);
   }
   sdio_host_set_card_interrupt(false);
   g_rx_int_armed = false;
}

/* How long transmit has been continuously refused, in microseconds; zero
   while it is healthy.  See g_runtime_tx_shut_since_us. */
// cppcheck-suppress unusedFunction
uint32_t sdio_runtime_tx_dead_us(void)
{
   if (g_runtime_tx_shut_since_us == 0u)
      return 0u;
   return RPI_GetSystemTime() - g_runtime_tx_shut_since_us;
}

// cppcheck-suppress unusedFunction
bool sdio_runtime_rx_gate_is_armed(void)
{
   return g_rx_int_armed;
}

bool sdio_runtime_poll_ethernet_frame(uint8_t *frame, uint16_t frame_capacity,
                                      uint16_t *frame_length)
{
   uint8_t frame_index;
   /* _Alignas(4): filled by a 32-bit EMMC PIO read. */
   _Alignas(4) uint16_t hwtag[2];

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

   /* Ask the cheap question before ANY bus traffic.  This must come ahead of
      the KSO wake check and the INT_STATUS service, not just ahead of the
      peek: measured with it placed after them, an idle main loop still spent
      a KSO CMD52 plus a full backplane INT_STATUS exchange on every one of
      ~50k passes a second, and the gate was only saving the peek.  A skipped
      poll must cost one MMIO read and nothing else.  The sweeps and asserted
      polls fall through and do the housekeeping, which keeps the mailbox
      serviced at worst every SDIO_RX_SWEEP_INTERVAL_US - comfortably inside
      the 20 ms floor the service logic wants. */
   if (g_rx_int_armed) {
      uint32_t gate_now_us = RPI_GetSystemTime();

      if (sdio_host_card_interrupt_asserted()) {
         g_rx_int_high++;
         g_rx_sweeping = false;
      } else if ((uint32_t)(gate_now_us - g_rx_sweep_us) < SDIO_RX_SWEEP_INTERVAL_US) {
         g_rx_int_skips++;
         return false;
      } else {
         g_rx_sweep_us = gate_now_us;
         g_rx_sweeps++;
         g_rx_sweeping = true;
      }
   }


   /* Wake the chip's SDIO interface before touching the bus.  It sleeps
      when the host is idle, and a sleeping interface silently swallows
      the firmware's asynchronous events.  wake_bus is non-blocking: if
      the chip has not woken yet it returns false, and we skip this poll
      entirely rather than stall the main loop - KSO has been asserted,
      so a later poll (a few microseconds away) will find it awake. */
   if (!sdio_runtime_wake_bus(&g_runtime_device))
      return false;

   bool service_interrupts = g_runtime_fifo_was_empty
      || (uint32_t)(RPI_GetSystemTime() - g_runtime_int_service_us)
            > SDIO_INT_SERVICE_INTERVAL_US;

   /* Ack pending mailbox interrupts.  This used to run on every poll, ahead
      of the fn2 read, costing two backplane CMD53s (~50 us) each time - but
      frames are not detected through INT_STATUS at all, they are found by
      peeking fn2 below, so on a poll that has frames waiting the whole
      exchange is pure overhead in the middle of the hot path.

      So it runs when the FIFO came up empty, plus a time-based floor: under
      sustained receive the FIFO may never be empty, and the firmware still
      needs its mailbox handshake, so service it anyway if it has been longer
      than SDIO_INT_SERVICE_INTERVAL_US.

      Note: we do NOT gate on READ_FRAME_BC here because the BCM43430 does not
      reliably update that register.  Instead we read 4 bytes directly from fn2
      (cyw43-driver approach) and treat 0x0000/0x0000 as "no frame". */
   if (service_interrupts) {
      static uint32_t s_poll_count;

      uint32_t int_status = 0u;
      g_runtime_int_service_us = RPI_GetSystemTime();
      if (sdio_backplane_read_u32_timeout(&g_runtime_device,
                        g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET,
                        SDIO_RUNTIME_POLL_TIMEOUT_US,
                        &int_status)) {
         if (int_status != 0u) {
            if (!g_runtime_emulator_mode) {
               /* Clear ALL INT_STATUS bits (write-1-to-clear), not just the
                  0xF0 mailbox bits.  Bits such as I_SBINT (0x20000000) and
                  I_HMB_DATA (0x00020000) must also be acked; leaving them
                  set can cause the firmware to stall before sending events.
                  This matches the cyw43-driver approach of writing back the
                  full int_status value. */
               (void)sdio_backplane_write_u32_timeout(&g_runtime_device,
                                                      g_runtime_sdio_core_base + SDIO_CORE_INT_STATUS_OFFSET,
                                                      int_status,
                                                      SDIO_RUNTIME_POLL_TIMEOUT_US);
               /* Per Circle ether4330.c intwait(): read Hostmboxdata and send
                  SMB_INT_ACK only on MailboxInt (bit 7 = 0x80).  Sending the
                  ACK spuriously on FrameInt-only events confuses the firmware. */
               if (int_status & 0x80u) {
                  uint32_t hmb_data = 0u;
                  (void)sdio_backplane_read_u32_timeout(&g_runtime_device,
                                                        g_runtime_sdio_core_base + SDIO_CORE_TO_HOST_MAILBOX_DATA_OFFSET,
                                                        SDIO_RUNTIME_POLL_TIMEOUT_US,
                                                        &hmb_data);
                  (void)sdio_backplane_write_u32_timeout(&g_runtime_device,
                                                         g_runtime_sdio_core_base + SDIO_CORE_TO_SB_MAILBOX_OFFSET,
                                                         0x00000002u,
                                                         SDIO_RUNTIME_POLL_TIMEOUT_US); /* SMB_INT_ACK */
                  (void)hmb_data;   /* HMB read+acked; not logged */
               }
            }
         }
      }
      /* Heartbeat every 1000 polls, but ONLY while the link is still
         down - it shows the join is still being waited on.  Once the
         link is up the heartbeat goes silent: a working connection
         should not stream log lines forever. */
      if ((s_poll_count % 1000u) == 999u && !g_runtime_link_up) {
         sdio_debug_log("poll heartbeat: %lu polls, link still down, rx_frames=%lu",
                        (unsigned long)(s_poll_count + 1u),
                        (unsigned long)g_runtime_rx_frame_count);
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

      if (hwtag[0] == 0u && hwtag[1] == 0u) {
         /* FIFO drained and the chip-side intstatus was cleared by the
            service block above, so re-arm the controller's latch.  This is
            the line every earlier attempt was missing: EMMC_INTERRUPT is
            write-1-to-clear, and without clearing it here the first genuine
            assertion reads as asserted for ever after - measured as 145k
            consecutive "high" polls.  If the chip still has a pending
            condition it re-asserts immediately and the next poll sees it,
            so nothing can be lost here. */
         if (g_rx_int_armed)
            sdio_host_clear_card_interrupt();
         g_runtime_fifo_was_empty = true;
         break; /* fn2 FIFO empty */
      }
      g_runtime_fifo_was_empty = false;
      if (g_rx_sweeping && frame_index == 0u)
         g_rx_int_missed++;            /* the sweep found what the line did not say */

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
   status.tx_resyncs = g_runtime_tx_resync_count;
   status.rejoins = g_runtime_rejoin_count;
   status.bus_four_bit = g_runtime_bus_four_bit;
   status.bus_high_speed = g_runtime_bus_high_speed;
   status.link_flag_trusted = g_runtime_link_flag_trusted;
   return status;
}
