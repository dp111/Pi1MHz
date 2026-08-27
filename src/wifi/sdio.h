#ifndef WIFI_SDIO_H
#define WIFI_SDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "sdio_host.h"
#include "wifi.h"

typedef struct {
   uint32_t raw_ocr;
   uint8_t function_count;
   bool memory_present;
   bool supports_1p8v;
} sdio_ocr_info_t;

typedef struct {
   bool started;
   bool link_up;
   uint32_t tx_frames;
   uint32_t rx_frames;
   /* Times a sustained shut transmit window turned out to be a stale
      flow-control stop and was reopened by clearing the cached mask.  (The
      sequence number is never rewritten; a window shut on credit falls to
      the recovery ladder instead.)  Should be 0; a climbing count means
      transmit is repeatedly stalling. */
   uint32_t tx_resyncs;
   /* Association retries since boot.  Non-zero means the link was lost (or
      never came up at boot) and the join sequence was re-issued. */
   uint32_t rejoins;
   /* True when the SDIO data bus is running 4-bit rather than 1-bit. */
   bool bus_four_bit;
   /* True when the SDIO bus is running 50 MHz high speed rather than 25 MHz. */
   bool bus_high_speed;
   /* True once a WLC_E_LINK has been seen with its LINK flag set, which is
      what arms detection of a link lost without a deauth. */
   bool link_flag_trusted;
} sdio_runtime_status_t;

typedef struct {
   bool success;
   uint8_t data;
   uint32_t response0;
   uint32_t interrupt;
   uint32_t error;
} sdio_cmd52_result_t;

typedef struct {
   bool success;
   uint32_t response0;
   uint32_t interrupt;
   uint32_t error;
} sdio_cmd53_result_t;

typedef struct {
   bool attempted;
   bool success;
   bool cccr_read_success;
   bool function_setup_attempted;
   bool function_setup_success;
   bool backplane_probe_success;
   bool clock_probe_attempted;
   bool clock_probe_success;
   bool power_probe_success;
   bool kso_probe_attempted;
   bool kso_probe_success;
   bool mailbox_probe_success;
   bool function2_probe_success;
   bool interrupt_ack_attempted;
   bool interrupt_ack_success;
   bool interrupt_mask_write_attempted;
   bool interrupt_mask_write_success;
   bool frame_header_probe_attempted;
   bool frame_header_probe_success;
   bool frame_header_valid;
   bool frame_read_abort_attempted;
   bool frame_read_abort_success;
   bool sdpcm_header_read_success;
   bool sdpcm_channel_known;
   bool sdpcm_header_sane;
   bool sdpcm_header_length_expected;
   bool sdpcm_post_header_probe_attempted;
   bool sdpcm_post_header_probe_success;
   bool sdpcm_bdc_header_decoded;
   bool sdpcm_cdc_prefix_decoded;
   bool sdpcm_bdc_version_valid;
   bool sdpcm_bdc_data_offset_sane;
   bool sdpcm_cdc_header_probe_attempted;
   bool sdpcm_cdc_header_probe_success;
   bool sdpcm_cdc_response_length_sane;
   bool sdpcm_cdc_payload_word0_probe_attempted;
   bool sdpcm_cdc_payload_word0_probe_success;
   bool sdpcm_cdc_payload_word0_magic_valid;
   bool sdpcm_cdc_payload_word1_probe_attempted;
   bool sdpcm_cdc_payload_word1_probe_success;
   bool sdpcm_data_ethertype_probe_attempted;
   bool sdpcm_data_ethertype_probe_success;
   bool sdpcm_brcm_event_probe_attempted;
   bool sdpcm_brcm_event_probe_success;
   bool sdpcm_brcm_event_oui_match;
   bool sdpcm_brcm_event_version_valid;
   bool sdpcm_brcm_event_msg_probe_attempted;
   bool sdpcm_brcm_event_msg_probe_success;
   bool sdpcm_brcm_event_msg_datalen_sane;
   bool sdpcm_brcm_event_ifname_truncated;
   bool tx_control_template_ready;
   bool tx_control_probe_attempted;
   bool tx_control_probe_success;
   bool tx_control_probe_multi_step;
   bool rx_frame_sweep_attempted;
   bool rx_frame_sweep_success;
   bool rx_frame_sweep_more_pending;
   bool tx_control_post_state_probe_attempted;
   bool tx_control_post_state_probe_success;
   uint32_t response0;
   uint32_t interrupt;
   uint32_t error;
   sdio_ocr_info_t ocr;
   uint8_t cccr_revision;
   uint8_t sd_revision;
   uint8_t io_enable;
   uint8_t io_ready;
   uint8_t bus_interface_control;
   uint8_t requested_io_enable;
   uint8_t configured_io_enable;
   uint8_t configured_io_ready;
   uint16_t function1_block_size;
   uint16_t function2_block_size;
   uint32_t chipcommon_id_register;
   uint16_t chip_id;
   uint8_t chip_revision;
   uint8_t chip_clock_csr_initial;
   uint8_t chip_clock_csr_requested;
   uint8_t chip_clock_csr_final;
   uint8_t wakeup_control;
   uint8_t sleep_control_status;
   uint8_t kso_control_requested;
   uint8_t kso_control_final;
   uint32_t sdio_core_base;
   uint32_t sdio_int_status;
   uint32_t sdio_int_status_after_ack;
   uint32_t sdio_int_host_mask;
   uint32_t sdio_int_host_mask_requested;
   uint32_t sdio_int_host_mask_after_write;
   uint32_t sdio_to_sb_mailbox;
   uint32_t sdio_to_host_mailbox_data;
   uint32_t sdio_interrupt_ack_value;
   uint16_t frame_header_size;
   uint16_t frame_header_size_complement;
   uint16_t tx_control_template_frame_size;
   uint16_t tx_control_template_frame_size_complement;
   uint8_t sdpcm_channel;
   uint8_t sdpcm_expected_header_length;
   uint8_t sdpcm_post_header_bytes_requested;
   uint8_t sdpcm_sequence;
   uint8_t sdpcm_channel_and_flags;
   uint8_t sdpcm_next_length;
   uint8_t sdpcm_header_length;
   uint8_t sdpcm_wireless_flow_control;
   uint8_t sdpcm_bus_data_credit;
   uint8_t sdpcm_post_header_prefix0;
   uint8_t sdpcm_post_header_prefix1;
   uint8_t sdpcm_post_header_prefix2;
   uint8_t sdpcm_post_header_prefix3;
   uint8_t sdpcm_bdc_flags;
   uint8_t sdpcm_bdc_priority;
   uint8_t sdpcm_bdc_flags2;
   uint8_t sdpcm_bdc_version;
   uint8_t sdpcm_bdc_data_offset;
   uint8_t sdpcm_bdc_data_offset_bytes;
   uint8_t sdpcm_brcm_event_version;
   uint8_t sdpcm_brcm_event_oui0;
   uint8_t sdpcm_brcm_event_oui1;
   uint8_t sdpcm_brcm_event_oui2;
   uint8_t sdpcm_brcm_event_ifidx;
   uint8_t sdpcm_brcm_event_bsscfgidx;
   uint8_t sdpcm_cdc_interface;
   uint8_t tx_control_template_sequence;
   uint8_t tx_control_template_channel_and_flags;
   uint8_t tx_control_template_next_length;
   uint8_t tx_control_template_header_length;
   uint8_t tx_control_template_wireless_flow_control;
   uint8_t tx_control_template_bus_data_credit;
   uint8_t tx_control_template_interface;
   uint8_t tx_control_probe_steps_requested;
   uint8_t tx_control_probe_steps_completed;
   uint8_t tx_control_probe_last_sequence;
   uint8_t rx_frame_sweep_limit;
   uint8_t rx_frames_decoded;
   uint8_t sdpcm_brcm_event_count;
   uint16_t sdpcm_cdc_request_length;
   uint16_t sdpcm_cdc_response_length;
   uint16_t sdpcm_cdc_payload_bytes_available;
   uint16_t sdpcm_brcm_event_msg_version;
   uint16_t sdpcm_brcm_event_msg_flags;
   uint16_t sdpcm_data_ethertype;
   uint16_t sdpcm_brcm_event_subtype;
   uint16_t sdpcm_brcm_event_length;
   uint16_t sdpcm_brcm_event_usr_subtype;
   uint16_t tx_control_template_payload_length;
   uint16_t tx_control_template_request_id;
   uint16_t tx_control_probe_last_request_id;
   uint32_t sdpcm_cdc_cmd_prefix;
   uint32_t tx_control_template_command;
   uint32_t tx_control_probe_last_command;
   uint32_t tx_control_template_payload_word0;
   uint32_t tx_control_template_cdc_length;
   uint32_t tx_control_template_cdc_flags;
   uint32_t tx_control_template_cdc_status;
   uint32_t tx_control_probe_response0;
   uint32_t tx_control_probe_interrupt;
   uint32_t tx_control_probe_error;
   uint32_t tx_control_post_int_status;
   uint32_t tx_control_post_to_sb_mailbox;
   uint32_t tx_control_post_to_host_mailbox_data;
   uint32_t sdpcm_brcm_event_first_type;
   uint32_t sdpcm_brcm_event_first_status;
   uint32_t sdpcm_brcm_event_first_reason;
   uint32_t sdpcm_brcm_event_type;
   uint32_t sdpcm_brcm_event_status;
   uint32_t sdpcm_brcm_event_reason;
   uint32_t sdpcm_brcm_event_auth_type;
   uint32_t sdpcm_brcm_event_datalen;
   uint32_t sdpcm_brcm_event_payload_bytes_available;
   /* Scratch for the IOCTL payload bytes (iovar name + value).  80 B
      is enough for every command we send today; the prepare path
      asserts payload_length stays within SDIO_TX_CONTROL_PAYLOAD_MAX
      so a future iovar with a longer name can't silently overflow. */
#define SDIO_TX_CONTROL_PAYLOAD_MAX 80u
   uint8_t tx_control_template_payload_bytes[SDIO_TX_CONTROL_PAYLOAD_MAX];
   uint8_t sdpcm_brcm_event_addr[6];
   char sdpcm_brcm_event_ifname[17];
   uint32_t sdpcm_cdc_length;
   uint32_t sdpcm_cdc_flags;
   uint32_t sdpcm_cdc_status;
   uint32_t sdpcm_cdc_payload_word0;
   uint32_t sdpcm_cdc_payload_word1;
   uint16_t sdpcm_cdc_request_id;
   uint8_t function2_info;
   uint8_t function2_watermark;
   uint16_t read_frame_byte_count;
   uint16_t tx_control_post_read_frame_byte_count;
} sdio_probe_result_t;

bool sdio_function_is_valid(uint8_t function_number);
uint32_t sdio_cmd52_argument(uint8_t function_number, uint32_t address, bool write,
                             bool read_after_write, uint8_t data);
uint32_t sdio_cmd53_argument(uint8_t function_number, uint32_t address, bool write,
                             bool block_mode, bool incrementing_address,
                             uint16_t count);
sdio_ocr_info_t sdio_decode_ocr(uint32_t raw_ocr);
bool sdio_cmd52_execute(sdio_host_t *dev, uint8_t function_number,
                        uint32_t address, bool write, bool read_after_write,
                        uint8_t *data, sdio_cmd52_result_t *result);
bool sdio_cmd53_execute(sdio_host_t *dev, uint8_t function_number,
                        uint32_t address, bool write, bool block_mode,
                        bool incrementing_address, uint16_t count, void *buffer,
                        uint32_t block_size, sdio_cmd53_result_t *result);
bool sdio_probe_card(bool tx_control_probe_enabled,
                     wifi_sdio_tx_probe_command_t tx_control_probe_command,
                     sdio_probe_result_t *result);
const sdio_probe_result_t *sdio_get_probe_result(void);
bool sdio_runtime_start(void);
bool sdio_runtime_tick(void);
bool sdio_runtime_started(void);
bool sdio_runtime_ready(void);
/* Re-issue the association sequence after a lost or never-established link.
   rejoin_start() re-arms it (false if bring-up or another rejoin is running);
   the caller then drives sdio_runtime_tick() while rejoin_busy() is true. */
bool sdio_runtime_rejoin_start(void);
bool sdio_runtime_rejoin_busy(void);
/* Microseconds since the last frame arrived from the chip (0 before the
   first).  A link can stop carrying traffic without the firmware reporting
   anything, so this is the only evidence that it has. */
uint32_t sdio_runtime_rx_idle_us(void);
uint32_t sdio_runtime_last_any_rx_stamp(void);
bool sdio_runtime_link_is_up(void);
void sdio_runtime_powersave_note_link_change(bool link_up);
void sdio_runtime_powersave_poll(void);
void sdio_runtime_powersave_verify_poll(void);
bool sdio_runtime_get_powersave_mode(int32_t *mode);
bool sdio_runtime_rx_gate_is_armed(void);
uint32_t sdio_runtime_tx_dead_us(void);
void sdio_runtime_prepare_for_warm_reboot(void);
void sdio_runtime_rx_gate_counts(uint32_t *skips, uint32_t *sweeps,
                                 uint32_t *missed, bool *armed, uint32_t *high);
/* Credit-window instrumentation (wifi_diag=1 in Pi1MHz.cfg).  set_diag is
   called once at boot; credit_diag returns false while disabled so /status
   can skip the rows.  depth_hist buckets: 0,1,2,3,4-7,8-15,>=16 (0 includes
   flow-control stops); reopen_hist buckets: <100us,<500us,<1ms,<5ms,<20ms,
   >=20ms. */
void sdio_runtime_set_diag(bool enabled);
bool sdio_runtime_diag_enabled(void);
bool sdio_runtime_credit_diag(uint32_t depth_hist[7], uint8_t *depth_min,
                              uint32_t reopen_hist[6], uint32_t *reopen_max_us);
/* Grant-loop latency (wifi_diag=1): DAT1 low->high service-latency
   histogram (<50us,<100,<250,<1ms,<5ms,5ms+) and credit-refill
   inter-arrival histogram (<500us,<1ms,<2,<5,<10,10ms+). */
bool sdio_runtime_grant_diag(uint32_t gate_hist[6], uint32_t gap_hist[6]);
/* Completion-phase data CMD53 failures (sequence deliberately not
   reclaimed).  Always counted; /status shows it only when nonzero. */
uint32_t sdio_runtime_tx_data_phase_fails(void);
/* TX glom (SDPCM superframes).  set_txglom caches the wifi_txglom limit
   (0 = off, default; clamped to the compile-time ceiling) - must be called
   before sdio_runtime_start().  txglom_status reports the configured
   limit, whether the bring-up iovar negotiation activated glom headers,
   and supers/subs/fallbacks plus the channel-3 RX tripwire counter. */
#define SDIO_RUNTIME_TXGLOM_MAX 16u
void sdio_runtime_set_txglom(uint8_t max_frames);
/* How many frames the TX hold-queue flush may hand
   sdio_runtime_send_ethernet_frames() in one call right now: 1 while glom
   is off / not negotiated / belt-and-braces disabled after repeated
   superframe failures, else the configured wifi_txglom limit. */
uint8_t sdio_runtime_txglom_batch_limit(void);
/* Send up to n Ethernet frames as ONE SDPCM superframe, in order, one
   credit per subframe.  Returns:
     >0  that many frames (a prefix of the input, clamped to the batch
         limit and the chip's credit depth) were accepted by the bus;
      0  refused - credit window shut, bus asleep, or a command-phase
         CMD53 failure whose sequence numbers were reclaimed: NOTHING was
         consumed and the caller retries later;
     <0  -(frames packed): the CMD53 failed after the command phase, the
         card may hold any prefix and the sequence numbers stay consumed -
         the caller MUST drop those frames (a retry would replay consumed
         sequence numbers). */
int8_t sdio_runtime_send_ethernet_frames(const uint8_t *const frames[],
                                         const uint16_t lens[], uint8_t n);
void sdio_runtime_txglom_status(uint8_t *config, bool *active,
                                uint32_t *supers, uint32_t *subs,
                                uint32_t *fallbacks, uint32_t *rx_channel3);
/* wifi_diag-gated glom feed diagnostics: histogram of the batch sizes
   actually sent (buckets 1, 2-3, 4-7, 8-15, 16+; bucket 1 includes every
   single-frame data send) and the count of credit refills (RX refreshes
   that advanced max_seq).  False while wifi_diag is off. */
bool sdio_runtime_txglom_diag(uint32_t batch_hist[5], uint32_t *credit_refills);
bool sdio_runtime_get_chip_mac(uint8_t mac_out[6]);
/* On-demand signal-strength read.  sdio_runtime_request_rssi() just flags
   a read (safe from the /status TCP callback); sdio_runtime_rssi_poll()
   performs it on the cooperative poll path and must be called from
   webserver_poll(); sdio_runtime_get_rssi() returns the cached signed dBm
   (false until the first read completes). */
void sdio_runtime_request_rssi(void);
void sdio_runtime_rssi_poll(void);
bool sdio_runtime_get_rssi(int32_t *out);
/* The chip's own packet counters: rx_good, rx_bad, tx_good, tx_bad,
   rx_ocast_good.  Requested and polled exactly like the RSSI.  Comparing the
   chip's rx_good against the host's received-frame count is what separates a
   frame that never arrived from one we failed to deliver. */
void sdio_runtime_request_pktcnts(void);
void sdio_runtime_pktcnts_poll(void);
bool sdio_runtime_get_pktcnts(uint32_t out[5]);

/* On-demand WLC_GET_RATE read, same pattern.  The cached value is the
   chip's current TX rate in 500 kbit/s units (-1 = auto/unknown). */
void sdio_runtime_request_rate(void);
void sdio_runtime_rate_poll(void);
bool sdio_runtime_get_rate(int32_t *rate_500kbps_out);
/* Cache a 6-byte MAC the runtime should push into the chip's
   cur_etheraddr iovar at boot.  Must be called BEFORE
   sdio_runtime_start() so the SET_MAC stage picks it up.  Passing
   NULL clears the cache - the chip then keeps its factory OTP MAC. */
void sdio_runtime_set_desired_mac(const uint8_t mac[6]);
bool sdio_runtime_send_ethernet_frame(const uint8_t *frame, uint16_t frame_length);
bool sdio_runtime_poll_ethernet_frame(uint8_t *frame, uint16_t frame_capacity,
                                      uint16_t *frame_length);
const char *sdio_runtime_last_error(void);
sdio_runtime_status_t sdio_runtime_get_status(void);

#endif