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
   uint8_t tx_control_template_payload_bytes[80];
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
bool sdio_runtime_link_is_up(void);
bool sdio_runtime_get_chip_mac(uint8_t mac_out[6]);
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