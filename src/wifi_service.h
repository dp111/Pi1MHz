#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <stdint.h>

/* WiFi control over the Pi1MHz &FCA6 services mailbox, speaking the ElkWiFi
   ROM's ABI.  The numbers and command-block layouts below are the ROM's and
   can only change in lockstep with it; the names are ours and mean nothing
   to it.  PING and DATETIME are in the range but implemented by
   net_service's utilities - see net_service.h. */
void wifi_service_init(uint8_t instance, uint8_t address);
void wifi_service_command(uint32_t command_pointer, uint32_t address,
                          uint8_t data);

#define WIFI_SVC_CMD_FIRST        80u
#define WIFI_SVC_CMD_STATUS       WIFI_SVC_CMD_FIRST
#define WIFI_SVC_CMD_SCAN         81u
#define WIFI_SVC_CMD_JOIN         82u
#define WIFI_SVC_CMD_IFCFG        83u
#define WIFI_SVC_CMD_LAPOPT       87u
#define WIFI_SVC_CMD_PING         88u
#define WIFI_SVC_CMD_DATETIME     89u
#define WIFI_SVC_CMD_CANCEL       90u
#define WIFI_SVC_CMD_RADIO        91u
#define WIFI_SVC_CMD_ONLINE       92u
/* The UEF cluster, handled in uef_service.c.  86 sits inside the range; 93
   sits one past ONLINE, which is why the range now ends there rather than at
   92.  Both were held vacant until a resumable inflater made the tape stream
   affordable - see src/uzlib/NOTES.md. */
#define WIFI_SVC_CMD_GUARD        86u
#define WIFI_SVC_CMD_UEF          93u
#define WIFI_SVC_CMD_LAST         WIFI_SVC_CMD_UEF

#define WIFI_SVC_OK               0x00u
#define WIFI_SVC_BUSY             0x80u
#define WIFI_SVC_ERR_PARAM        0x40u
#define WIFI_SVC_ERR_IO           0x41u
#define WIFI_SVC_ERR_UNSUPPORTED  0x42u
#define WIFI_SVC_ERR_NETWORK      0x43u
#define WIFI_SVC_ERR_NO_WIFI      0x44u

#define WIFI_SVC_JOIN_QUERY       0u
#define WIFI_SVC_JOIN_SET         1u
#define WIFI_SVC_JOIN_LEAVE       2u
#define WIFI_SVC_JOIN_RADIO_OFF   3u

#endif
