/* lwip_hooks.h - declarations for the hooks lwipopts.h routes to this
 * firmware (LWIP_HOOK_FILENAME).  Included by lwIP's own sources, so it
 * must stay small and self-contained. */
#ifndef LWIP_HOOKS_H
#define LWIP_HOOKS_H

#include "lwip/arch.h"

struct pbuf;

/* LWIP_HOOK_DHCP_PARSE_OPTION: every DHCP option lwIP does not decode. */
void wifi_lwip_dhcp_option(u8_t msg_type, u8_t option, u8_t len,
                           struct pbuf *p, u16_t offset);

#endif
