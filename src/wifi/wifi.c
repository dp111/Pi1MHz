#include "wifi.h"

#include "cyw43.h"
#include "netname.h"
#include "sdio.h"
#include "webserver.h"
#include "wifi_lwip.h"

#include "../Pi1MHz.h"
#include "../rpi/info.h"
#include "../rpi/rpi.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static wifi_config_t g_wifi_config;
static wifi_network_config_t g_wifi_network_config;
static wifi_state_t g_wifi_state;
static bool g_wifi_debug_enabled;
static char g_wifi_error[WIFI_ERROR_TEXT_MAX_LEN + 1];
typedef struct {
   const char *name;
   wifi_sdio_tx_probe_command_t value;
} wifi_probe_command_entry_t;

typedef enum {
   WIFI_BOOT_STAGE_IDLE = 0,
   WIFI_BOOT_STAGE_START_SDIO,
   WIFI_BOOT_STAGE_WAIT_SDIO,
   WIFI_BOOT_STAGE_OPTIONAL_PROBE,
   WIFI_BOOT_STAGE_INIT_LWIP,
   WIFI_BOOT_STAGE_INIT_WEBSERVER,
   WIFI_BOOT_STAGE_COMPLETE
} wifi_boot_stage_t;

static wifi_boot_stage_t g_wifi_boot_stage;
static bool g_wifi_boot_poll_registered;
static bool g_wifi_images_preloaded;
/* WiFi is initialised exactly once.  A BBC RST re-runs init_emulator()
   - which calls every emulator's init again - but the WiFi connection
   has no BBC-bus state, so a reset must not tear it down and re-join. */
static bool g_wifi_init_done;
static void wifi_debug_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static bool wifi_equals_ignore_case(const char *left, const char *right);

/* Single poll entry-point for the whole WiFi stack: the four sub-poll
   functions all have early-exit guards (boot stage IDLE,
   timers_running, reboot_pending, g_ready), so calling them
   unconditionally is safe even before their owning sub-system has
   been initialised.  Calling them straight from here avoids using
   four slots in the small Pi1MHz poll table and keeps the call set
   visible in one place. */
static void wifi_dispatch_poll(void)
{
   wifi_boot();
   wifi_lwip_poll();
   webserver_poll();
   netname_poll();
}

static bool wifi_preload_images(void)
{
   if (g_wifi_images_preloaded)
      return true;

   wifi_debug_log("loading CYW43 firmware assets");
   if (!cyw43_preload_images())
      return false;

   g_wifi_images_preloaded = true;
   wifi_note_firmware_ready();
   wifi_debug_log("firmware assets loaded");
   return true;
}

static void wifi_boot_fail(const char *message)
{
   wifi_set_error(message);
   g_wifi_boot_stage = WIFI_BOOT_STAGE_COMPLETE;
}

static bool wifi_validate_config(void)
{
   if (!g_wifi_config.enabled) {
      g_wifi_state = WIFI_STATE_DISABLED;
      wifi_debug_log("disabled by configuration");
      return false;
   }

   if (g_wifi_config.ssid[0] == '\0') {
      wifi_set_error("wifi_ssid or SSID missing from cmdline.txt");
      return false;
   }

   if (g_wifi_config.password[0] == '\0') {
      wifi_set_error("wifi_password or SSIDpassword missing from cmdline.txt");
      return false;
   }

   if (!g_wifi_config.ip_config_valid) {
      wifi_set_error("wifi_ip must be 'dhcp' or a valid IPv4 address");
      return false;
   }

   if (!g_wifi_network_config.valid) {
      wifi_set_error("static network config requires a valid wifi_ip and wifi_netmask");
      return false;
   }

   /* No need to re-check that ip_address is non-empty when
      ip_mode == STATIC: wifi_parse_ip_mode only selects STATIC after
      successfully parsing an IPv4 into ip_address, so the field is
      always populated by the time we reach here. */

   g_wifi_state = WIFI_STATE_CONFIGURED;
   wifi_debug_log("configuration accepted ssid=%s hostname=%s",
                  g_wifi_config.ssid,
                  g_wifi_config.hostname);
   return true;
}

void wifi_debug_printf(const char *format, ...)
{
   va_list args;
   char line[192];
   int written;

   if (!g_wifi_debug_enabled)
      return;

   va_start(args, format);
   written = vsnprintf(line, sizeof(line), format, args);
   va_end(args);

   if (written <= 0)
      return;

   LOG_DEBUG("%s", line);
}

static void wifi_debug_log(const char *format, ...)
{
   va_list args;
   char line[192];
   int written;

   if (!g_wifi_debug_enabled)
      return;

   va_start(args, format);
   written = vsnprintf(line, sizeof(line), format, args);
   va_end(args);

   if (written <= 0)
      return;

   LOG_DEBUG("WIFI: %s\r\n", line);
}

static bool wifi_parse_ipv4(const char *value, char *dest, size_t dest_size,
                           wifi_ipv4_addr_t *parsed_address)
{
   unsigned int octet = 0;
   unsigned int octet_count = 0;
   bool have_digit = false;
   const char *src = value;

   if (value == NULL || value[0] == '\0' || dest == NULL || dest_size == 0)
      return false;

   while (*src != '\0') {
      if (*src >= '0' && *src <= '9') {
         have_digit = true;
         octet = (octet * 10u) + (unsigned int)(*src - '0');
         if (octet > 255u)
            return false;
      } else if (*src == '.') {
         if (!have_digit || octet_count >= 3u)
            return false;
         if (parsed_address != NULL)
            parsed_address->octets[octet_count] = (uint8_t)octet;
         ++octet_count;
         octet = 0;
         have_digit = false;
      } else {
         return false;
      }
      ++src;
   }

   if (!have_digit || octet_count != 3u)
      return false;

   if (parsed_address != NULL)
      parsed_address->octets[octet_count] = (uint8_t)octet;

   strlcpy(dest, value, dest_size);
   return true;
}

static bool wifi_parse_ip_mode(wifi_config_t *config)
{
   const char *prop = get_cmdline_prop("wifi_ip");

   if ((prop == NULL || prop[0] == '\0') && config->ip_address[0] != '\0') {
      config->ip_mode = WIFI_IP_MODE_STATIC;
      return true;
   }

   if (prop == NULL || prop[0] == '\0') {
      config->ip_mode = WIFI_IP_MODE_DHCP;
      return true;
   }

   if (wifi_equals_ignore_case(prop, "dhcp")) {
      config->ip_mode = WIFI_IP_MODE_DHCP;
      config->ip_address[0] = '\0';
      return true;
   }

   if (wifi_parse_ipv4(prop, config->ip_address, sizeof(config->ip_address), NULL)) {
      config->ip_mode = WIFI_IP_MODE_STATIC;
      return true;
   }

   return false;
}

static bool wifi_load_ipv4_prop(char *dest, size_t dest_size, const char *key, const char *fallback)
{
   const char *prop = get_cmdline_prop(key);

   if ((prop == NULL || prop[0] == '\0') && fallback != NULL)
      prop = get_cmdline_prop(fallback);

   if (prop == NULL || prop[0] == '\0')
      return false;

   if (!wifi_parse_ipv4(prop, dest, dest_size, NULL))
      return false;

   return true;
}

static bool wifi_parse_ipv4_text(const char *text, wifi_ipv4_addr_t *parsed_address)
{
   char ignored_text[WIFI_IPV4_TEXT_MAX_LEN + 1];

   ignored_text[0] = '\0';
   return wifi_parse_ipv4(text, ignored_text, sizeof(ignored_text), parsed_address);
}

static bool wifi_network_config_build(wifi_network_config_t *network_config,
                                      const wifi_config_t *config)
{
   memset(network_config, 0, sizeof(*network_config));
   network_config->ip_mode = config->ip_mode;
   network_config->valid = config->ip_config_valid;

   if (config->ip_mode == WIFI_IP_MODE_DHCP)
      return network_config->valid;

   network_config->has_address = wifi_parse_ipv4_text(config->ip_address, &network_config->address);
   network_config->has_netmask = wifi_parse_ipv4_text(config->netmask, &network_config->netmask);
   network_config->has_gateway = wifi_parse_ipv4_text(config->gateway, &network_config->gateway);
   network_config->has_dns = wifi_parse_ipv4_text(config->dns, &network_config->dns);

   network_config->valid = network_config->valid
      && network_config->has_address
      && network_config->has_netmask;

   return network_config->valid;
}

static bool wifi_copy_cmdline_prop(char *dest, size_t dest_size, const char *primary_key,
                                   const char *fallback_key)
{
   const char *prop = get_cmdline_prop(primary_key);

   if ((prop == NULL || prop[0] == '\0') && fallback_key != NULL)
      prop = get_cmdline_prop(fallback_key);

   if (prop == NULL || prop[0] == '\0')
      return false;

   strlcpy(dest, prop, dest_size);
   return true;
}

static uint16_t wifi_parse_port(const char *value, uint16_t default_port)
{
   unsigned int port = 0;

   if (value == NULL || value[0] == '\0')
      return default_port;

   while (*value != '\0') {
      if (*value < '0' || *value > '9')
         return default_port;
      port = (port * 10u) + (unsigned int)(*value - '0');
      if (port > 65535u)
         return default_port;
      ++value;
   }

   return (uint16_t)port;
}

static uint8_t wifi_parse_u8(const char *value, uint8_t default_value)
{
   unsigned int parsed = 0;

   if (value == NULL || value[0] == '\0')
      return default_value;

   while (*value != '\0') {
      if (*value < '0' || *value > '9')
         return default_value;
      parsed = (parsed * 10u) + (unsigned int)(*value - '0');
      if (parsed > 255u)
         return default_value;
      ++value;
   }

   if (parsed == 0u)
      return default_value;

   return (uint8_t)parsed;
}

static void wifi_clear_error(void)
{
   g_wifi_error[0] = '\0';
}

static bool wifi_cmdline_bool(const char *key)
{
   const char *prop = get_cmdline_prop(key);

   return prop != NULL && prop[0] != '\0' && prop[0] != '0';
}

static char wifi_ascii_tolower(char value)
{
   if (value >= 'A' && value <= 'Z')
      return (char)(value + ('a' - 'A'));

   return value;
}

static bool wifi_equals_ignore_case(const char *left, const char *right)
{
   if (left == NULL || right == NULL)
      return false;

   while (*left != '\0' && *right != '\0') {
      if (wifi_ascii_tolower(*left) != wifi_ascii_tolower(*right))
         return false;
      ++left;
      ++right;
   }

   return *left == '\0' && *right == '\0';
}

static wifi_sdio_tx_probe_command_t wifi_parse_sdio_tx_probe_command(void)
{
   static const wifi_probe_command_entry_t command_map[] = {
      { "magic", WIFI_SDIO_TX_PROBE_COMMAND_MAGIC },
      { "up", WIFI_SDIO_TX_PROBE_COMMAND_UP },
      { "infra", WIFI_SDIO_TX_PROBE_COMMAND_INFRA },
      { "auth", WIFI_SDIO_TX_PROBE_COMMAND_AUTH },
      { "ssid", WIFI_SDIO_TX_PROBE_COMMAND_SSID },
      { "wpa_auth", WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH },
      { "wsec", WIFI_SDIO_TX_PROBE_COMMAND_WSEC },
      { "pmk", WIFI_SDIO_TX_PROBE_COMMAND_PMK },
      { "join", WIFI_SDIO_TX_PROBE_COMMAND_JOIN }
   };

   const char *prop = get_cmdline_prop("wifi_sdio_tx_probe_command");
   size_t index;

   if (prop == NULL || prop[0] == '\0')
      return WIFI_SDIO_TX_PROBE_COMMAND_VERSION;

   for (index = 0; index < (sizeof(command_map) / sizeof(command_map[0])); ++index) {
      if (wifi_equals_ignore_case(prop, command_map[index].name))
         return command_map[index].value;
   }

   return WIFI_SDIO_TX_PROBE_COMMAND_VERSION;
}

bool wifi_config_load(wifi_config_t *config)
{
   const char *port_prop;
   bool have_ssid;
   bool have_password;

   if (config == NULL)
      return false;

   memset(config, 0, sizeof(*config));
   config->http_port = 80;
   config->ip_mode = WIFI_IP_MODE_DHCP;
   config->ip_config_valid = true;
   config->sdio_rx_sweep_limit = 16u;
   strlcpy(config->hostname, "Pi1MHz", sizeof(config->hostname));
   strlcpy(config->netmask, "255.255.255.0", sizeof(config->netmask));
   config->sdio_probe_enabled = wifi_cmdline_bool("wifi_sdio_probe");
   config->sdio_tx_probe_enabled = wifi_cmdline_bool("wifi_sdio_tx_probe");
   /* wifi_emulator=1 keeps the old lenient "proceed anyway" behaviour when
      the CYW43 firmware never reaches HT_AVAIL / fn2-ready (only useful when
      running under a test emulator with no real chip).  On real hardware,
      leaving it unset makes a firmware-boot failure report a clear error. */
   config->allow_emulator_fallback = wifi_cmdline_bool("wifi_emulator");
   /* Regulatory domain for the CYW43 "country" iovar.  Defaults to "GB":
      the brcmfmac43430 firmware validates the country code against its
      built-in regulatory table, which holds real ISO 3166 codes (GB,
      US, DE, ...) but NOT the "XX" worldwide placeholder - sending "XX"
      makes the country iovar return BCME_BADARG and leaves the radio
      country-disabled, so WLC_UP cannot bring the interface up.
      Override from cmdline.txt with e.g. wifi_country=US for wherever
      the Pi physically is. */
   {
      const char *country_prop = get_cmdline_prop("wifi_country");

      if (country_prop != NULL && country_prop[0] != '\0')
         strlcpy(config->country, country_prop, sizeof(config->country));
      else
         strlcpy(config->country, "GB", sizeof(config->country));
   }
   config->sdio_tx_probe_command = wifi_parse_sdio_tx_probe_command();
   config->sdio_rx_sweep_limit = wifi_parse_u8(get_cmdline_prop("wifi_sdio_rx_sweep_limit"),
                                               config->sdio_rx_sweep_limit);

   have_ssid = wifi_copy_cmdline_prop(config->ssid, sizeof(config->ssid), "wifi_ssid", "SSID");
   have_password = wifi_copy_cmdline_prop(config->password, sizeof(config->password), "wifi_password", "SSIDpassword");
   (void) wifi_copy_cmdline_prop(config->hostname, sizeof(config->hostname), "wifi_hostname", NULL);
   (void) wifi_load_ipv4_prop(config->ip_address, sizeof(config->ip_address), "static_ip", NULL);
   (void) wifi_load_ipv4_prop(config->netmask, sizeof(config->netmask), "wifi_netmask", NULL);
   (void) wifi_load_ipv4_prop(config->gateway, sizeof(config->gateway), "wifi_gateway", NULL);
   (void) wifi_load_ipv4_prop(config->dns, sizeof(config->dns), "wifi_dns", NULL);
   config->ip_config_valid = wifi_parse_ip_mode(config);

   /* WiFi only starts when an SSID is present.  Without wifi_ssid (or
      SSID) in cmdline.txt there is nothing to join, so the entire WiFi
      stack stays down - no SDIO bring-up, no firmware load, no polling.
      A password on its own is not enough to enable WiFi. */
   config->enabled = have_ssid;

   port_prop = get_cmdline_prop("wifi_http_port");
   config->http_port = wifi_parse_port(port_prop, config->http_port);
   config->config_present = have_ssid || have_password;
   (void) wifi_network_config_build(&g_wifi_network_config, config);

   return config->config_present;
}

bool wifi_debug_enabled(void)
{
   return g_wifi_debug_enabled;
}

void wifi_emulator_init(uint8_t instance, uint8_t address)
{
   (void)instance;
   (void)address;

   wifi_init();
}

void wifi_init(void)
{
   /* Initialise once only.  init_emulator() is re-run on every BBC RST;
      on the second and later calls (i.e. on a reset) this returns
      immediately, leaving the existing WiFi connection - and its
      registered poll hooks - running undisturbed. */
   if (g_wifi_init_done)
      return;
   g_wifi_init_done = true;

    (void) wifi_config_load(&g_wifi_config);
   g_wifi_boot_stage = WIFI_BOOT_STAGE_IDLE;
   g_wifi_images_preloaded = false;
   g_wifi_debug_enabled = wifi_cmdline_bool("wifi_debug");
   wifi_clear_error();

   if (g_wifi_debug_enabled)
      wifi_debug_printf("WIFI: debug enabled\r\n");

   wifi_debug_log("init enabled=%u has_ssid=%u has_password=%u ip_mode=%s http_port=%u",
                  g_wifi_config.enabled ? 1u : 0u,
                  g_wifi_config.ssid[0] != '\0' ? 1u : 0u,
                  g_wifi_config.password[0] != '\0' ? 1u : 0u,
                  wifi_ip_mode_name(g_wifi_config.ip_mode),
                  (unsigned int)g_wifi_config.http_port);

   if (!wifi_validate_config())
      return;

   wifi_debug_log("boot start state=%s", wifi_state_name(g_wifi_state));

   /* Register the dispatcher with the main poll table exactly once;
      every WiFi sub-system (wifi_boot, wifi_lwip_poll, webserver_poll,
      netname_poll) is called from wifi_dispatch_poll instead of
      taking its own slot. */
   if (!g_wifi_boot_poll_registered) {
      Pi1MHz_Register_Poll(wifi_dispatch_poll);
      g_wifi_boot_poll_registered = true;
   }

   g_wifi_boot_stage = WIFI_BOOT_STAGE_START_SDIO;
   wifi_debug_log("boot scheduled");
}

void wifi_boot(void)
{
   if (g_wifi_state == WIFI_STATE_DISABLED || g_wifi_state == WIFI_STATE_ERROR)
      return;

   switch (g_wifi_boot_stage) {
      case WIFI_BOOT_STAGE_START_SDIO:
         wifi_debug_log("starting SDIO runtime");
         if (!sdio_runtime_start()) {
            wifi_boot_fail(sdio_runtime_last_error());
            return;
         }

         if (!wifi_preload_images()) {
            wifi_boot_fail("CYW43 firmware preload failed");
            return;
         }

         g_wifi_boot_stage = WIFI_BOOT_STAGE_WAIT_SDIO;
         return;

      case WIFI_BOOT_STAGE_WAIT_SDIO:
         if (sdio_runtime_tick()) {
            /* State machine still has stages to run (including join) — come back next tick. */
            if (sdio_runtime_last_error()[0] != '\0') {
               wifi_boot_fail(sdio_runtime_last_error());
            }
            return;
         }
         /* tick() returned false: reached DONE or ERROR. */
         if (sdio_runtime_started()) {
            wifi_note_sdio_ready();
            wifi_debug_log("sdio runtime started");
            g_wifi_boot_stage = WIFI_BOOT_STAGE_OPTIONAL_PROBE;
         } else {
            wifi_boot_fail(sdio_runtime_last_error());
         }
         return;

      case WIFI_BOOT_STAGE_OPTIONAL_PROBE:
         if (g_wifi_config.sdio_probe_enabled
            && sdio_probe_card(g_wifi_config.sdio_tx_probe_enabled,
                               g_wifi_config.sdio_tx_probe_command,
                               NULL)) {
            wifi_note_sdio_ready();
            wifi_debug_log("optional SDIO probe completed");
         }

         g_wifi_boot_stage = WIFI_BOOT_STAGE_INIT_LWIP;
         return;

      case WIFI_BOOT_STAGE_INIT_LWIP:
         wifi_debug_log("preparing lwip");
         wifi_lwip_prepare();
         wifi_lwip_init_stack();
         if (g_wifi_state == WIFI_STATE_CONFIGURED)
            g_wifi_state = WIFI_STATE_IMAGES_READY;
         g_wifi_boot_stage = WIFI_BOOT_STAGE_INIT_WEBSERVER;
         return;

      case WIFI_BOOT_STAGE_INIT_WEBSERVER:
         wifi_debug_log("initialising webserver");
         webserver_init();
         wifi_debug_log("boot complete state=%s", wifi_state_name(g_wifi_state));
         g_wifi_boot_stage = WIFI_BOOT_STAGE_COMPLETE;
         return;

      case WIFI_BOOT_STAGE_IDLE:
      case WIFI_BOOT_STAGE_COMPLETE:
      default:
         return;
   }
}

void wifi_note_sdio_ready(void)
{
   if (g_wifi_state == WIFI_STATE_CONFIGURED)
      g_wifi_state = WIFI_STATE_SDIO_READY;
}

void wifi_note_firmware_ready(void)
{
   if (g_wifi_state == WIFI_STATE_CONFIGURED || g_wifi_state == WIFI_STATE_SDIO_READY)
      g_wifi_state = WIFI_STATE_FIRMWARE_READY;
}

void wifi_note_network_ready(void)
{
   if (g_wifi_state == WIFI_STATE_FIRMWARE_READY)
      g_wifi_state = WIFI_STATE_NETWORK_READY;

   if (g_wifi_state == WIFI_STATE_NETWORK_READY && webserver_is_ready())
      g_wifi_state = WIFI_STATE_HTTP_READY;
}

void wifi_note_http_ready(void)
{
   if (g_wifi_state == WIFI_STATE_NETWORK_READY)
      g_wifi_state = WIFI_STATE_HTTP_READY;
}

void wifi_set_error(const char *message)
{
   g_wifi_state = WIFI_STATE_ERROR;

   if (message == NULL) {
      g_wifi_error[0] = '\0';
      return;
   }

   strlcpy(g_wifi_error, message, sizeof(g_wifi_error));
   wifi_debug_log("error: %s", g_wifi_error);
}

const wifi_config_t *wifi_get_config(void)
{
   return &g_wifi_config;
}

const wifi_network_config_t *wifi_get_network_config(void)
{
   return &g_wifi_network_config;
}

wifi_status_t wifi_get_status(void)
{
   wifi_status_t status;

   status.state = g_wifi_state;
   status.has_ssid = g_wifi_config.ssid[0] != '\0';
   status.has_password = g_wifi_config.password[0] != '\0';
   status.uses_dhcp = g_wifi_config.ip_mode == WIFI_IP_MODE_DHCP;
   status.has_static_ip = g_wifi_config.ip_address[0] != '\0';
   status.can_start_http = g_wifi_state >= WIFI_STATE_NETWORK_READY && g_wifi_state < WIFI_STATE_ERROR;
   status.sdio_probe_enabled = g_wifi_config.sdio_probe_enabled;
   status.sdio_tx_probe_enabled = g_wifi_config.sdio_tx_probe_enabled;
   status.sdio_tx_probe_command = g_wifi_config.sdio_tx_probe_command;
   status.http_port = g_wifi_config.http_port;
   status.last_error = wifi_last_error();

   return status;
}

wifi_state_t wifi_get_state(void)
{
   return g_wifi_state;
}

const char *wifi_state_name(wifi_state_t state)
{
   switch (state)
   {
   case WIFI_STATE_DISABLED: return "disabled";
   case WIFI_STATE_CONFIGURED: return "configured";
   case WIFI_STATE_IMAGES_READY: return "images-ready";
   case WIFI_STATE_SDIO_READY: return "sdio-ready";
   case WIFI_STATE_FIRMWARE_READY: return "firmware-ready";
   case WIFI_STATE_NETWORK_READY: return "network-ready";
   case WIFI_STATE_HTTP_READY: return "http-ready";
   case WIFI_STATE_ERROR: return "error";
   default: return "unknown";
   }
}

const char *wifi_ip_mode_name(wifi_ip_mode_t mode)
{
   switch (mode)
   {
   case WIFI_IP_MODE_DHCP: return "dhcp";
   case WIFI_IP_MODE_STATIC: return "static";
   default: return "unknown";
   }
}

uint32_t wifi_ipv4_to_u32(const wifi_ipv4_addr_t *address)
{
   if (address == NULL)
      return 0;

   return ((uint32_t)address->octets[0] << 24)
      | ((uint32_t)address->octets[1] << 16)
      | ((uint32_t)address->octets[2] << 8)
      | (uint32_t)address->octets[3];
}

const char *wifi_last_error(void)
{
   return g_wifi_error[0] != '\0' ? g_wifi_error : "";
}