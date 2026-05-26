#ifndef WIFI_WIFI_H
#define WIFI_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_SSID_MAX_LEN 64
#define WIFI_PASSWORD_MAX_LEN 64
#define WIFI_HOSTNAME_MAX_LEN 32
#define WIFI_IPV4_TEXT_MAX_LEN 15
#define WIFI_ERROR_TEXT_MAX_LEN 96

typedef enum {
   WIFI_IP_MODE_DHCP = 0,
   WIFI_IP_MODE_STATIC
} wifi_ip_mode_t;

typedef enum {
   WIFI_SDIO_TX_PROBE_COMMAND_VERSION = 0,
   WIFI_SDIO_TX_PROBE_COMMAND_MAGIC,
   /* Issue WLC_DOWN at the very start of the join sequence so the chip
      is in a known-down state before any parameter setup.  brcmfmac's
      convention; required by some firmware builds before BSS-level
      params can be safely committed. */
   WIFI_SDIO_TX_PROBE_COMMAND_DOWN,
   WIFI_SDIO_TX_PROBE_COMMAND_UP,
   WIFI_SDIO_TX_PROBE_COMMAND_INFRA,
   WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA,
   WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA2_EAPVER,
   WIFI_SDIO_TX_PROBE_COMMAND_SUP_WPA_TMO,
   WIFI_SDIO_TX_PROBE_COMMAND_AUTH,
   WIFI_SDIO_TX_PROBE_COMMAND_MFP,
   WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS,
   /* Global event mask (no bsscfg prefix).  brcmfmac sets this; some
      BCM43430 firmware builds AND it with the per-bsscfg mask, so an
      all-zero global mask gates out every event regardless of what
      bsscfg:event_msgs allows.  This is the matching iovar that
      pairs with bsscfg:event_msgs above. */
   WIFI_SDIO_TX_PROBE_COMMAND_GLOBAL_EVENT_MSGS,
   /* Extended event mask with cyw43-driver-style framing:
        name + bsscfg_idx (4) + version(1) + cmd(1) + length(2) + mask[16].
      Some firmware builds only honor this _ext form for events to
      actually fire, even when the simpler event_msgs iovars return
      status=0. */
   WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_EXT,
   /* GET-VAR readback of bsscfg:event_msgs.  Used after the SET above to
      verify that the chip actually applied the per-bsscfg event mask;
      if the readback comes back all-zero the SET was a silent no-op
      (which is the prime suspect for the "join accepted, chip silent"
      symptom on the BCM43430 firmware build). */
   WIFI_SDIO_TX_PROBE_COMMAND_EVENT_MSGS_VERIFY,
   WIFI_SDIO_TX_PROBE_COMMAND_SSID,
   /* WLC_GET_SSID readback after SET_SSID.  Echoes the wlc_ssid_t the
      chip currently has cached - confirms whether the SET took effect
      or was silently dropped. */
   WIFI_SDIO_TX_PROBE_COMMAND_GET_SSID,
   WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH,
   WIFI_SDIO_TX_PROBE_COMMAND_WSEC,
   WIFI_SDIO_TX_PROBE_COMMAND_PMK,
   WIFI_SDIO_TX_PROBE_COMMAND_POWERSAVE_OFF,
   WIFI_SDIO_TX_PROBE_COMMAND_TXGLOM_OFF,
   /* mpc=0 forces the radio to stay powered on regardless of association
      state.  Default mpc=1 lets the firmware park the radio in low-power
      mode when not associated, which on some BCM43430 builds causes
      scans and join attempts to fail silently because the radio is asleep
      when the join state machine tries to transmit. */
   WIFI_SDIO_TX_PROBE_COMMAND_MPC_OFF,
   WIFI_SDIO_TX_PROBE_COMMAND_ROAM_OFF,
   WIFI_SDIO_TX_PROBE_COMMAND_COUNTRY,
   /* Pre-join radio/regulatory setup, ported from the working bare-metal
      PicoWi driver (picowi_join.c join_start / join_restart). */
   WIFI_SDIO_TX_PROBE_COMMAND_ANTDIV,
   WIFI_SDIO_TX_PROBE_COMMAND_GMODE,
   WIFI_SDIO_TX_PROBE_COMMAND_BAND,
   WIFI_SDIO_TX_PROBE_COMMAND_APSTA,
   WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_BA_WSIZE,
   WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_MPDU,
   WIFI_SDIO_TX_PROBE_COMMAND_AMPDU_RX_FACTOR,
   WIFI_SDIO_TX_PROBE_COMMAND_MCAST_LIST,
   WIFI_SDIO_TX_PROBE_COMMAND_PM2_SLEEP_RET,
   WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_BCN,
   WIFI_SDIO_TX_PROBE_COMMAND_BCN_LI_DTIM,
   WIFI_SDIO_TX_PROBE_COMMAND_ASSOC_LISTEN,
   /* WLC_SCAN diagnostic.  Issued at the end of the join sequence so
      we can tell whether the radio ever transmits a probe request -
      if WLC_E_ESCAN_RESULT events fire, the radio works and the
      silence after SET_SSID is specific to the join state machine.
      If even WLC_SCAN produces nothing the chip is fundamentally
      wedged (regulatory, PMU, antenna). */
   WIFI_SDIO_TX_PROBE_COMMAND_SCAN,
   /* Heartbeat state probes - sent from the poll loop every ~1s while
      link is down so the cdc-rsp log surfaces the chip's current
      channel and BSSID.  All-zero BSSID confirms the chip never
      associated; non-zero BSSID with link_up=0 means the events are
      being lost on the host side rather than not generated. */
   WIFI_SDIO_TX_PROBE_COMMAND_GET_BSSID,
   WIFI_SDIO_TX_PROBE_COMMAND_GET_CHANSPEC,
   /* WLC_GET_VAR cur_etheraddr - dumps the MAC the chip is currently
      using for TX.  All-zero readback proves NVRAM didn't apply
      during firmware boot (PA/antenna config also lives in NVRAM and
      would be missing for the same reason, which would explain a
      silent radio).  Real-looking 6-byte MAC means NVRAM applied and
      the silence is somewhere else. */
   WIFI_SDIO_TX_PROBE_COMMAND_GET_MAC,
   /* Heartbeat readbacks of the WPA/security setup state.  These prove
      whether the SET_WSEC / SET_WPA_AUTH / SET_AUTH / SET_INFRA /
      bsscfg:sup_wpa SET commands during the join sequence actually
      committed to the chip - "ack=status=0" alone doesn't guarantee
      the value was applied; some BCM43430 firmware builds quietly
      ignore SETs issued in the wrong order and return 0 anyway.  An
      all-zero readback with the heartbeat = the SET was a silent
      no-op and the chip's join state machine has nothing to work
      with, which produces the silent post-SET_SSID symptom. */
   WIFI_SDIO_TX_PROBE_COMMAND_GET_WSEC,
   WIFI_SDIO_TX_PROBE_COMMAND_GET_WPA_AUTH,
   WIFI_SDIO_TX_PROBE_COMMAND_GET_AUTH,
   WIFI_SDIO_TX_PROBE_COMMAND_GET_INFRA,
   WIFI_SDIO_TX_PROBE_COMMAND_GET_SUP_WPA,
   /* WLC_GET_RADIO readback, issued straight after WLC_UP.  The chip
      returns a radio-disable bitmask:
        0x0000 = radio enabled (a NOTUP after this is not the radio)
        0x0001 = WL_RADIO_SW_DISABLE
        0x0002 = WL_RADIO_HW_DISABLE
        0x0004 = WL_RADIO_MPC_DISABLE   (minimum-power-consumption park)
        0x0008 = WL_RADIO_COUNTRY_DISABLE (no valid regulatory domain)
      This pinpoints why WLC_UP returns status 0 yet the interface
      never actually comes up. */
   WIFI_SDIO_TX_PROBE_COMMAND_GET_RADIO,
   /* GET-VAR readback of the "country" iovar, issued straight after the
      SET.  The chip echoes back the wl_country_t it currently holds
      (country_abbrev[4] + int32 rev + ccode[4]).  If the SET was a
      silent no-op the readback shows the stale/empty domain; the
      response length also reveals the exact struct size this firmware
      build expects, which pins down why the SET returns BCME_BADARG. */
   WIFI_SDIO_TX_PROBE_COMMAND_GET_COUNTRY,
   WIFI_SDIO_TX_PROBE_COMMAND_JOIN
} wifi_sdio_tx_probe_command_t;

typedef struct {
   uint8_t octets[4];
} wifi_ipv4_addr_t;

typedef struct {
   wifi_ip_mode_t ip_mode;
   bool valid;
   bool has_address;
   bool has_netmask;
   bool has_gateway;
   bool has_dns;
   wifi_ipv4_addr_t address;
   wifi_ipv4_addr_t netmask;
   wifi_ipv4_addr_t gateway;
   wifi_ipv4_addr_t dns;
} wifi_network_config_t;

typedef enum {
   WIFI_STATE_DISABLED = 0,
   WIFI_STATE_CONFIGURED,
   WIFI_STATE_IMAGES_READY,
   WIFI_STATE_SDIO_READY,
   WIFI_STATE_FIRMWARE_READY,
   WIFI_STATE_NETWORK_READY,
   WIFI_STATE_HTTP_READY,
   WIFI_STATE_ERROR
} wifi_state_t;

typedef struct {
   bool enabled;
   bool config_present;
   bool ip_config_valid;
   bool sdio_probe_enabled;
   bool sdio_tx_probe_enabled;
   bool allow_emulator_fallback;
   char country[8];
   wifi_sdio_tx_probe_command_t sdio_tx_probe_command;
   uint8_t sdio_rx_sweep_limit;
   char ssid[WIFI_SSID_MAX_LEN + 1];
   char password[WIFI_PASSWORD_MAX_LEN + 1];
   char hostname[WIFI_HOSTNAME_MAX_LEN + 1];
   wifi_ip_mode_t ip_mode;
   char ip_address[WIFI_IPV4_TEXT_MAX_LEN + 1];
   char netmask[WIFI_IPV4_TEXT_MAX_LEN + 1];
   char gateway[WIFI_IPV4_TEXT_MAX_LEN + 1];
   char dns[WIFI_IPV4_TEXT_MAX_LEN + 1];
   uint16_t http_port;
} wifi_config_t;

typedef struct {
   wifi_state_t state;
   bool has_ssid;
   bool has_password;
   bool uses_dhcp;
   bool has_static_ip;
   bool can_start_http;
   bool sdio_probe_enabled;
   bool sdio_tx_probe_enabled;
   wifi_sdio_tx_probe_command_t sdio_tx_probe_command;
   uint16_t http_port;
   const char *last_error;
} wifi_status_t;

bool wifi_config_load(wifi_config_t *config);
bool wifi_debug_enabled(void);
void wifi_debug_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
void wifi_emulator_init(uint8_t instance, uint8_t address);
void wifi_init(void);
void wifi_boot(void);
void wifi_note_sdio_ready(void);
void wifi_note_firmware_ready(void);
void wifi_note_network_ready(void);
void wifi_note_http_ready(void);
void wifi_set_error(const char *message);

const wifi_config_t *wifi_get_config(void);
const wifi_network_config_t *wifi_get_network_config(void);
wifi_status_t wifi_get_status(void);
wifi_state_t wifi_get_state(void);
const char *wifi_state_name(wifi_state_t state);
const char *wifi_ip_mode_name(wifi_ip_mode_t mode);
uint32_t wifi_ipv4_to_u32(const wifi_ipv4_addr_t *address);
const char *wifi_last_error(void);

#endif