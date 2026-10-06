#pragma once
/* Host-test stub of wifi/wifi_lwip.h - the two calls the provider makes. */
#include <stdbool.h>
typedef struct { bool address_ready; } wifi_lwip_context_t;
const wifi_lwip_context_t *wifi_lwip_get_context(void);
void wifi_lwip_rx_kick(void);
