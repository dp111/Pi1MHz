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
} sdio_host_t;

int sdio_host_open(sdio_host_t *host);
int sdio_host_open_start(sdio_host_t *host);
int sdio_host_open_poll(sdio_host_t *host);
int sdio_host_set_clock(sdio_host_t *host, uint32_t target_rate_hz, uint32_t *actual_rate_hz);
int sdio_host_set_clock_start(sdio_host_t *host, uint32_t target_rate_hz);
int sdio_host_set_clock_poll(sdio_host_t *host, uint32_t *actual_rate_hz);
int sdio_host_submit(sdio_host_t *host,
                     const sdio_host_command_t *command,
                     sdio_host_result_t *result);
const char *sdio_host_backend_name(void);
const char *sdio_host_last_error(void);

#endif