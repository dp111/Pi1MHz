#include <stdint.h>

void harddisc_emulator_init( uint8_t instance , uint8_t address);
uint8_t harddisc_emulator_get_address(void);

/* Service the audio between sectors of a long transfer (see .c) */
void hd_audio_service(void);

/* Transfer diagnostics for /status */
void hd_juke_service(void);
void hd_juke_request(uint8_t dir);
uint32_t hd_diag_flags(void);
extern uint32_t hd_ev_writes, hd_ev_reads, hd_ev_sel;
extern uint32_t hd_ack_timeouts;     /* host never ACKed: transfer abandoned */
extern uint32_t hd_service_max_us;   /* longest audio_pump() inside a transfer */
extern uint32_t hd_ack_wait_max_us;  /* longest single wait for the host ACK */
