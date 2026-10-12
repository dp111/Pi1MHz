#ifndef WIFI_LWIP_H
#define WIFI_LWIP_H
#include <stdbool.h>
typedef struct { bool address_ready; bool link_up; } wifi_lwip_context_t;
const wifi_lwip_context_t *wifi_lwip_get_context(void);
void wifi_lwip_rx_kick(void);
const char *wifi_lwip_dhcp_ntp_server(void);
#endif
