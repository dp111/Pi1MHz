/* Host tests for the ElkWiFi service's WiFi.profile (wifi_service.c).
 *
 * The profile overrides wifi_ssid / wifi_password in Pi1MHz.cfg, so what goes
 * into it decides which network the Pi joins after every reset.  *JOIN writes
 * it on purpose; *LAPOPT only means to save a display setting.  The real
 * service and parser run over an in-memory card; the WiFi driver is a fake
 * whose live configuration the test sets, as Pi1MHz.cfg or a JOIN would. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "Pi1MHz.h"
#include "config.h"
#include "services.h"
#include "net_service.h"
#include "uef_service.h"
#include "wifi_service.h"
#include "wifi/sdio.h"
#include "wifi/wifi.h"
#include "wifi/wifi_lwip.h"
#include "BeebSCSI/filesystem.h"
#include "BeebSCSI/fatfs/ff.h"

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
   printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
   else { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PROFILE "/Pi1MHz/WiFi.profile"

/* ---- the card: one file -------------------------------------------------- */
static bool card_has;
static char card[1024];
static int  writes;

static void card_set(const char *text)
{
   card_has = text != NULL;
   snprintf(card, sizeof card, "%s", text ? text : "");
}
uint32_t filesystemReadFile(const char *filename, uint8_t **address, unsigned int max_size)
{
   (void)max_size;
   size_t n = strcmp(filename, PROFILE) == 0 && card_has ? strlen(card) : 0;
   *address = malloc(n + 1);
   memcpy(*address, card, n);
   return (uint32_t)n;
}
uint32_t filesystemWriteFile(const char *filename, const uint8_t *address, uint32_t max_size)
{ (void)filename; (void)address; return max_size; }
bool filesystemWriteFileSafe(const char *filename, const uint8_t *address, uint32_t length)
{
   if (strcmp(filename, PROFILE) != 0 || length >= sizeof card) return false;
   memcpy(card, address, length);
   card[length] = '\0';
   card_has = true;
   writes++;
   return true;
}
FRESULT f_stat(const char *path, FILINFO *fno)
{
   if (strcmp(path, PROFILE) != 0 || !card_has) return FR_NO_FILE;
   fno->fsize = (uint32_t)strlen(card);
   return FR_OK;
}

/* ---- Pi1MHz, services, config ---------------------------------------------- */
static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;
uint64_t g_now_us;
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data) { pi.Memory[addr] = data; }
static func_ptr poll_cb;
void Pi1MHz_Register_Poll(func_ptr f, const char *name) { (void)name; poll_cb = f; }
bool services_register(uint8_t first, uint8_t last, service_command_fn handler)
{ (void)first; (void)last; (void)handler; return true; }
const char *config_get(const char *key) { (void)key; return NULL; }
bool config_get_bool(const char *key) { return strcmp(key, "wifi_service_enable") == 0; }

/* ---- the WiFi driver: its live configuration ------------------------------- */
static wifi_config_t live;
static wifi_network_config_t live_net;
static int  rejoins;
static char rejoined_ssid[WIFI_SSID_MAX_LEN + 1];

static void live_set(const char *ssid, const char *password)
{
   snprintf(live.ssid, sizeof live.ssid, "%s", ssid);
   snprintf(live.password, sizeof live.password, "%s", password);
   live.security = WIFI_SECURITY_AUTO;
}
const wifi_config_t *wifi_get_config(void) { return &live; }
const wifi_network_config_t *wifi_get_network_config(void) { return &live_net; }
bool wifi_profile_is_valid(const char *s, const char *p, wifi_security_t t)
{ (void)s; (void)p; (void)t; return true; }
bool wifi_reconfigure_and_rejoin(const char *ssid, const char *password, wifi_security_t security)
{
   rejoins++;
   snprintf(rejoined_ssid, sizeof rejoined_ssid, "%s", ssid);
   live_set(ssid, password);
   live.security = security;
   return true;
}
bool wifi_enable_radio(void) { return true; }
bool wifi_disable_radio(void) { return true; }
bool wifi_disconnect(void) { live_set("", ""); return true; }
wifi_state_t wifi_get_state(void) { return (wifi_state_t)0; }
bool sdio_runtime_started(void) { return true; }
bool sdio_runtime_rejoin_busy(void) { return false; }
bool sdio_runtime_link_is_up(void) { return true; }
bool sdio_runtime_get_chip_mac(uint8_t mac[6]) { memset(mac, 0, 6); return true; }
sdio_runtime_status_t sdio_runtime_get_status(void) { sdio_runtime_status_t s = {0}; return s; }
bool sdio_runtime_scan_start(void) { return false; }
void sdio_runtime_scan_cancel(void) {}
bool sdio_runtime_scan_busy(void) { return false; }
bool sdio_runtime_scan_complete(void) { return false; }
uint8_t sdio_runtime_scan_results(sdio_wifi_scan_result_t *o, uint8_t c) { (void)o; (void)c; return 0; }
static const wifi_lwip_context_t lwip_ctx;
const wifi_lwip_context_t *wifi_lwip_get_context(void) { return &lwip_ctx; }
void wifi_lwip_rx_kick(void) {}

/* ---- net_service utilities and the UEF service: unused here ---------------- */
bool net_ping_start(const char *host) { (void)host; return false; }
net_util_state_t net_ping_poll(uint32_t *ms) { (void)ms; return NET_UTIL_IDLE; }
void net_ping_cancel(void) {}
bool net_time_start(const char *server) { (void)server; return false; }
net_util_state_t net_time_poll(uint32_t *s) { (void)s; return NET_UTIL_IDLE; }
void net_time_cancel(void) {}
void uef_service_reset(void) {}
uint8_t uef_service_stream_command(uint32_t cp) { (void)cp; return 0; }
uint8_t uef_service_guard_command(uint32_t cp) { (void)cp; return 0; }

/* ---- helpers ----------------------------------------------------------------- */
#define CP  0x1000u
#define REG 0x50u

static uint8_t lapopt(uint8_t fields)
{
   pi.JIM_ram[CP] = WIFI_SVC_CMD_LAPOPT;
   pi.JIM_ram[CP + 1u] = fields;
   wifi_service_command(CP, REG, 0);
   poll_cb();
   return pi.Memory[REG];
}
static uint8_t join(const char *ssid, const char *password)
{
   pi.JIM_ram[CP] = WIFI_SVC_CMD_JOIN;
   pi.JIM_ram[CP + 1u] = WIFI_SVC_JOIN_SET;
   size_t n = strlen(ssid) + 1u;
   memcpy(&pi.JIM_ram[CP + 2u], ssid, n);
   memcpy(&pi.JIM_ram[CP + 2u + n], password, strlen(password) + 1u);
   wifi_service_command(CP, REG, 0);
   poll_cb();
   return pi.Memory[REG];
}
static void reset_beeb(void) { rejoins = 0; rejoined_ssid[0] = '\0'; wifi_service_init(0, 0); }
static bool card_says(const char *s) { return strstr(card, s) != NULL; }

int main(void)
{
   pi.JIM_ram_size = 2;
   pi.JIM_ram = calloc((size_t)pi.JIM_ram_size * JIM_RAM_STEP, 1);

   printf("== *LAPOPT with the network from Pi1MHz.cfg, no profile ==\n");
   card_set(NULL);
   live_set("CfgNet", "cfgpass");               /* from Pi1MHz.cfg */
   reset_beeb();
   CHECK(rejoins == 0 && !card_has, "no profile: init leaves the cfg's network alone");
   CHECK(lapopt(7) == WIFI_SVC_OK, "*LAPOPT 7 answers OK");
   CHECK(card_has && card_says("scanfields=7"), "the setting is saved:\n%s", card);
   CHECK(!card_says("CfgNet") && !card_says("cfgpass"),
         "the cfg's SSID and password are not copied into the profile");
   live_set("EditedNet", "editedpass");         /* the user edits Pi1MHz.cfg */
   reset_beeb();
   CHECK(rejoins == 0, "after a cfg edit the profile does not drag the Pi back (%d rejoins, to '%s')",
         rejoins, rejoined_ssid);
   CHECK(lapopt(127) == WIFI_SVC_OK && card_says("scanfields=127") && !card_says("EditedNet"),
         "a second *LAPOPT rewrites the setting alone");

   printf("== *LAPOPT with a network saved by *JOIN ==\n");
   card_set(NULL);
   live_set("CfgNet", "cfgpass");
   reset_beeb();
   CHECK(join("Joined", "joinpw") == WIFI_SVC_OK && card_says("ssid=Joined"), "*JOIN saves its network");
   (void)wifi_disconnect();                     /* *JOIN - : the live config empties */
   CHECK(lapopt(7) == WIFI_SVC_OK, "*LAPOPT 7 answers OK");
   CHECK(card_says("ssid=Joined") && card_says("password=joinpw") && card_says("scanfields=7"),
         "the saved network survives, whatever the live config says:\n%s", card);
   reset_beeb();
   CHECK(rejoins == 1 && strcmp(rejoined_ssid, "Joined") == 0,
         "and the next reset still joins it ('%s')", rejoined_ssid);

   printf("== *LAPOPT over a profile it cannot read back ==\n");
   card_set("ssid=Joined\npassword=joinpw\nsecurity=BOGUS\nscanfields=127\n");
   writes = 0;
   CHECK(lapopt(7) != WIFI_SVC_OK, "refused rather than rewritten (%02X)", pi.Memory[REG]);
   CHECK(writes == 0 && card_says("security=BOGUS"), "the file is left as it was");

   free(pi.JIM_ram);
   printf("%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
