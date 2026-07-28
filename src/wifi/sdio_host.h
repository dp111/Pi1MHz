#ifndef WIFI_SDIO_HOST_H
#define WIFI_SDIO_HOST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
   uint32_t command;
   uint32_t argument;
   uint32_t timeout_us;
   void *buffer;
   uint32_t block_size;
   uint32_t blocks_to_transfer;
} sdio_host_command_t;

typedef struct {
   bool success;
   uint32_t response0;
   uint32_t interrupt;
   uint32_t error;
} sdio_host_result_t;

typedef struct {
   uint32_t clock_target_rate_hz;
   uint32_t clock_actual_rate_hz;
   uint32_t clock_deadline_us;
   uint32_t clock_divider;
   uint32_t open_deadline_us;
   uint8_t clock_phase;
   uint8_t open_phase;
   uint8_t open_attempt;
   /* WL_REG_ON low time for the current attempt; escalates on retry because a
      warm restart has to discharge rails a cold boot starts with empty. */
   uint32_t open_wl_low_us;
} sdio_host_t;

/* Attempts at bringing the SDIO host up before giving up on WiFi entirely. */
#define SDIO_HOST_OPEN_MAX_ATTEMPTS 3u

int sdio_host_open(sdio_host_t *host);
int sdio_host_open_start(sdio_host_t *host);
int sdio_host_open_poll(sdio_host_t *host);
int sdio_host_set_clock(sdio_host_t *host, uint32_t target_rate_hz, uint32_t *actual_rate_hz);
/* Controller data-bus width.  Must be kept in step with CCCR 0x07 on the
   card; see sdio_host_set_bus_width() for why a mismatch is invisible to
   CMD52 and only shows up on data transfers. */
int sdio_host_set_bus_width(sdio_host_t *host, bool four_bit);
int sdio_host_set_clock_start(sdio_host_t *host, uint32_t target_rate_hz);
int sdio_host_set_clock_poll(sdio_host_t *host, uint32_t *actual_rate_hz);
int sdio_host_submit(sdio_host_t *host,
                     const sdio_host_command_t *command,
                     sdio_host_result_t *result);
/* Show or hide the in-band SDIO interrupt (DAT1) in EMMC_INTERRUPT.  Must stay
   hidden until the chip's firmware is running: the card signals by pulling
   DAT1 LOW, so a powered-down chip reads as permanently asserted. */
void sdio_host_set_card_interrupt(bool visible);
bool sdio_host_card_interrupt_asserted(void);
/* Write-1-clear the controller's card-interrupt latch.  EMMC_INTERRUPT is a
   LATCH, not a level: without this, the first genuine assertion reads as
   asserted forever - which is exactly what the earlier gate attempts measured
   (145k polls high, ~0 clear).  Call after servicing the card. */
void sdio_host_clear_card_interrupt(void);
const char *sdio_host_backend_name(void);
const char *sdio_host_last_error(void);

#endif