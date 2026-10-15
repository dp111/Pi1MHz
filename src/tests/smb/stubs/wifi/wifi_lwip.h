#ifndef WIFI_LWIP_H
#define WIFI_LWIP_H
#include <stdbool.h>
typedef struct { bool address_ready; } wifi_lwip_context_t;
void wifi_lwip_poll(void);
bool wifi_lwip_in_poll(void);
const wifi_lwip_context_t *wifi_lwip_get_context(void);
#endif
