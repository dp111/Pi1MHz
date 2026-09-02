#ifndef ELKWIFI_SERVICE_H
#define ELKWIFI_SERVICE_H

#include <stdint.h>

/* ElkWiFi-compatible operations on the Pi1MHz &FCA6 services mailbox. */
void elkwifi_service_init(uint8_t instance, uint8_t address);
void elkwifi_service_command(uint32_t command_pointer, uint32_t address,
                             uint8_t data);

#define ELKWIFI_CMD_FIRST        80u
#define ELKWIFI_CMD_STATUS       ELKWIFI_CMD_FIRST
#define ELKWIFI_CMD_SCAN         81u
#define ELKWIFI_CMD_JOIN         82u
#define ELKWIFI_CMD_IFCFG        83u
#define ELKWIFI_CMD_LAPOPT       87u
#define ELKWIFI_CMD_PING         88u
#define ELKWIFI_CMD_DATETIME     89u
#define ELKWIFI_CMD_CANCEL       90u
#define ELKWIFI_CMD_RADIO        91u
#define ELKWIFI_CMD_ONLINE       92u
/* 86 and 93 are deliberately left unclaimed: they carried the UEF stream and
   guard-image commands, which are held back until a resumable inflater makes
   the stream affordable. */
#define ELKWIFI_CMD_LAST         ELKWIFI_CMD_ONLINE

#define ELKWIFI_OK               0x00u
#define ELKWIFI_BUSY             0x80u
#define ELKWIFI_ERR_PARAM        0x40u
#define ELKWIFI_ERR_IO           0x41u
#define ELKWIFI_ERR_UNSUPPORTED  0x42u
#define ELKWIFI_ERR_NETWORK      0x43u
#define ELKWIFI_ERR_NO_WIFI      0x44u

#define ELKWIFI_JOIN_QUERY       0u
#define ELKWIFI_JOIN_SET         1u
#define ELKWIFI_JOIN_LEAVE       2u
#define ELKWIFI_JOIN_RADIO_OFF   3u

#endif
