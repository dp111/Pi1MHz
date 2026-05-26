#include "webserver.h"

#include "cyw43.h"
#include "framebuffer_export.h"
#include "sdio.h"
#include "wifi.h"
#include "wifi_lwip.h"
#include "../BeebSCSI/fatfs/ff.h"
#include "../BeebSCSI/filesystem.h"
#include "../framebuffer/framebuffer.h"
#include "../rpi/info.h"

#include "lwip/err.h"
#include "lwip/tcp.h"

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WEBSERVER_REQUEST_MAX_LEN 1024u
#define WEBSERVER_RESPONSE_CHUNK_LEN 1024u
#define WEBSERVER_ERROR_TEXT_MAX_LEN 96u

typedef struct {
   struct tcp_pcb *pcb;
   char request[WEBSERVER_REQUEST_MAX_LEN + 1u];
   size_t request_length;
   char *response_data;
   size_t response_length;
   size_t response_queued;
   size_t response_acked;
   bool close_after_send;
} webserver_connection_t;

static struct tcp_pcb *g_webserver_listener;
static bool g_webserver_ready;
static char g_webserver_error[WEBSERVER_ERROR_TEXT_MAX_LEN + 1u];

static void webserver_set_error(const char *message);
static void webserver_close_connection(webserver_connection_t *connection, bool abort_connection);
static err_t webserver_queue_response(webserver_connection_t *connection);
static err_t webserver_connection_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t webserver_connection_poll(void *arg, struct tcp_pcb *tpcb);
static void webserver_connection_error(void *arg, err_t err);
static err_t webserver_connection_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static err_t webserver_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static bool webserver_request_complete(const char *request, size_t request_length);
static const char *webserver_status_text(uint16_t status_code);
static bool webserver_build_response_text(const char *request_text, char **response_text,
                                          size_t *response_length);
static size_t response_append(webserver_response_t *response, const char *format, ...)
   __attribute__((format(printf, 2, 3)));
static void response_begin_html(webserver_response_t *response, const char *title);
static void response_end_html(webserver_response_t *response);

static void webserver_set_error(const char *message)
{
   if (message == NULL) {
      g_webserver_error[0] = '\0';
      return;
   }

   strlcpy(g_webserver_error, message, sizeof(g_webserver_error));
}

static void webserver_connection_detach_callbacks(webserver_connection_t *connection)
{
   if (connection == NULL || connection->pcb == NULL)
      return;

   tcp_arg(connection->pcb, NULL);
   tcp_sent(connection->pcb, NULL);
   tcp_recv(connection->pcb, NULL);
   tcp_poll(connection->pcb, NULL, 0);
   tcp_err(connection->pcb, NULL);
}

static void webserver_close_connection(webserver_connection_t *connection, bool abort_connection)
{
   if (connection == NULL)
      return;

   webserver_connection_detach_callbacks(connection);
   if (connection->pcb != NULL) {
      err_t close_result = abort_connection ? ERR_ABRT : tcp_close(connection->pcb);

      if (abort_connection || close_result != ERR_OK)
         tcp_abort(connection->pcb);
   }

   free(connection->response_data);
   free(connection);
}

static bool webserver_request_complete(const char *request, size_t request_length)
{
   size_t index;

   if (request == NULL)
      return false;

   for (index = 0u; index + 3u < request_length; ++index) {
      if (request[index] == '\r' && request[index + 1u] == '\n'
         && request[index + 2u] == '\r' && request[index + 3u] == '\n') {
         return true;
      }
   }

   for (index = 0u; index + 1u < request_length; ++index) {
      if (request[index] == '\n' && request[index + 1u] == '\n')
         return true;
   }

   return false;
}

static const char *webserver_status_text(uint16_t status_code)
{
   switch (status_code) {
      case 200:
         return "OK";
      case 400:
         return "Bad Request";
      case 404:
         return "Not Found";
      case 405:
         return "Method Not Allowed";
      case 413:
         return "Payload Too Large";
      case 500:
         return "Internal Server Error";
      case 501:
         return "Not Implemented";
      default:
         return "OK";
   }
}

static bool webserver_build_error_response(uint16_t status_code, const char *title,
                                           const char *detail, char **response_text,
                                           size_t *response_length)
{
   webserver_response_t response;
   char header[192];
   int header_length;
   char *full_response;

   response_begin_html(&response, title);
   response.status_code = status_code;
   response_append(&response, "<p>%s</p>", detail);
   response_end_html(&response);

   header_length = snprintf(header, sizeof(header),
                            "HTTP/1.1 %u %s\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %lu\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            (unsigned int)response.status_code,
                            webserver_status_text(response.status_code),
                            response.content_type,
                            (unsigned long)response.body_length);
   if (header_length <= 0 || (size_t)header_length >= sizeof(header))
      return false;

   full_response = malloc((size_t)header_length + response.body_length);
   if (full_response == NULL)
      return false;

   memcpy(full_response, header, (size_t)header_length);
   memcpy(full_response + header_length, response.body, response.body_length);
   *response_text = full_response;
   *response_length = (size_t)header_length + response.body_length;
   return true;
}

static bool webserver_build_response_text(const char *request_text, char **response_text,
                                          size_t *response_length)
{
   webserver_request_t request;
   webserver_response_t response;
   char method[8];
   char path[256];
   char request_line[320];
   char *line_end;
   char header[192];
   int header_length;
   char *full_response;

   if (request_text == NULL || response_text == NULL || response_length == NULL)
      return false;

   *response_text = NULL;
   *response_length = 0u;

   strlcpy(request_line, request_text, sizeof(request_line));
   line_end = strstr(request_line, "\r\n");
   if (line_end == NULL)
      line_end = strchr(request_line, '\n');
   if (line_end != NULL)
      *line_end = '\0';

   method[0] = '\0';
   path[0] = '\0';
   if (sscanf(request_line, "%7s %255s", method, path) != 2) {
      return webserver_build_error_response(400u, "Bad Request",
                                            "Could not parse the HTTP request line.",
                                            response_text, response_length);
   }

   if (strcmp(method, "GET") != 0) {
      return webserver_build_error_response(405u, "Method Not Allowed",
                                            "Only HTTP GET is supported right now.",
                                            response_text, response_length);
   }

   memset(&request, 0, sizeof(request));
   request.method = method;
   request.path = path;

   if (!webserver_render(&request, &response)) {
      return webserver_build_error_response(500u, "Internal Server Error",
                                            "The webserver could not render a response.",
                                            response_text, response_length);
   }

   if (response.content_type == NULL)
      response.content_type = "text/plain; charset=utf-8";

   header_length = snprintf(header, sizeof(header),
                            "HTTP/1.1 %u %s\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %lu\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            (unsigned int)response.status_code,
                            webserver_status_text(response.status_code),
                            response.content_type,
                            (unsigned long)response.body_length);
   if (header_length <= 0 || (size_t)header_length >= sizeof(header))
      return false;

   full_response = malloc((size_t)header_length + response.body_length);
   if (full_response == NULL)
      return false;

   memcpy(full_response, header, (size_t)header_length);
   memcpy(full_response + header_length, response.body, response.body_length);
   *response_text = full_response;
   *response_length = (size_t)header_length + response.body_length;
   return true;
}

static err_t webserver_queue_response(webserver_connection_t *connection)
{
   err_t result = ERR_OK;

   if (connection == NULL || connection->pcb == NULL)
      return ERR_CLSD;

   while (connection->response_queued < connection->response_length) {
      size_t remaining = connection->response_length - connection->response_queued;
      u16_t send_window = tcp_sndbuf(connection->pcb);
      size_t send_length = remaining;

      if (send_window == 0u)
         break;

      if (send_length > (size_t)send_window)
         send_length = send_window;
      if (send_length > WEBSERVER_RESPONSE_CHUNK_LEN)
         send_length = WEBSERVER_RESPONSE_CHUNK_LEN;

      result = tcp_write(connection->pcb,
                         connection->response_data + connection->response_queued,
                         (u16_t)send_length,
                         TCP_WRITE_FLAG_COPY);
      if (result != ERR_OK)
         break;

      connection->response_queued += send_length;
   }

   if (result == ERR_OK && connection->response_queued != 0u)
      result = tcp_output(connection->pcb);

   if (result == ERR_OK
      && connection->close_after_send
      && connection->response_acked >= connection->response_length) {
      webserver_close_connection(connection, false);
      return ERR_OK;
   }

   return result;
}

static err_t webserver_connection_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
   webserver_connection_t *connection = (webserver_connection_t *)arg;

   (void)tpcb;

   if (connection == NULL)
      return ERR_OK;

   connection->response_acked += len;
   if (connection->close_after_send && connection->response_acked >= connection->response_length) {
      webserver_close_connection(connection, false);
      return ERR_OK;
   }

   return webserver_queue_response(connection);
}

static err_t webserver_connection_poll(void *arg, struct tcp_pcb *tpcb)
{
   webserver_connection_t *connection = (webserver_connection_t *)arg;

   (void)tpcb;

   if (connection == NULL)
      return ERR_OK;

   if (connection->response_data == NULL) {
      webserver_close_connection(connection, true);
      return ERR_ABRT;
   }

   return webserver_queue_response(connection);
}

static void webserver_connection_error(void *arg, err_t err)
{
   webserver_connection_t *connection = (webserver_connection_t *)arg;

   (void)err;

   if (connection == NULL)
      return;

   connection->pcb = NULL;
   free(connection->response_data);
   free(connection);
}

static err_t webserver_connection_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
   webserver_connection_t *connection = (webserver_connection_t *)arg;

   if (connection == NULL)
      return ERR_ARG;

   if (err != ERR_OK) {
      if (p != NULL)
         pbuf_free(p);
      webserver_close_connection(connection, true);
      return ERR_ABRT;
   }

   if (p == NULL) {
      webserver_close_connection(connection, false);
      return ERR_OK;
   }

   if (connection->request_length + p->tot_len > WEBSERVER_REQUEST_MAX_LEN) {
      tcp_recved(tpcb, p->tot_len);
      pbuf_free(p);
      if (!webserver_build_error_response(413u, "Request Too Large",
                                          "The request headers exceeded the supported size.",
                                          &connection->response_data,
                                          &connection->response_length)) {
         webserver_close_connection(connection, true);
         return ERR_ABRT;
      }
      connection->close_after_send = true;
      return webserver_queue_response(connection);
   }

   pbuf_copy_partial(p, &connection->request[connection->request_length], p->tot_len, 0u);
   connection->request_length += p->tot_len;
   connection->request[connection->request_length] = '\0';
   tcp_recved(tpcb, p->tot_len);
   pbuf_free(p);

   if (!webserver_request_complete(connection->request, connection->request_length))
      return ERR_OK;

   if (!webserver_build_response_text(connection->request,
                                      &connection->response_data,
                                      &connection->response_length)) {
      if (!webserver_build_error_response(500u, "Internal Server Error",
                                          "The webserver could not build an HTTP response.",
                                          &connection->response_data,
                                          &connection->response_length)) {
         webserver_close_connection(connection, true);
         return ERR_ABRT;
      }
   }

   connection->close_after_send = true;
   return webserver_queue_response(connection);
}

static err_t webserver_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
   webserver_connection_t *connection;

   (void)arg;

   if (err != ERR_OK || newpcb == NULL)
      return ERR_VAL;

   connection = calloc(1u, sizeof(*connection));
   if (connection == NULL) {
      tcp_abort(newpcb);
      return ERR_ABRT;
   }

   connection->pcb = newpcb;
   tcp_arg(newpcb, connection);
   tcp_recv(newpcb, webserver_connection_recv);
   tcp_sent(newpcb, webserver_connection_sent);
   tcp_poll(newpcb, webserver_connection_poll, 4u);
   tcp_err(newpcb, webserver_connection_error);
   return ERR_OK;
}

static size_t response_append(webserver_response_t *response, const char *format, ...)
{
   va_list args;
   int written;
   size_t remaining;

   if (response->body_length >= sizeof(response->body))
      return 0;

   remaining = sizeof(response->body) - response->body_length;

   va_start(args, format);
   written = vsnprintf(&response->body[response->body_length], remaining, format, args);
   va_end(args);

   if (written <= 0)
      return 0;

   if ((size_t)written >= remaining) {
      response->body_length = sizeof(response->body) - 1;
      return remaining - 1;
   }

   response->body_length += (size_t)written;
   return (size_t)written;
}

static void response_begin_html(webserver_response_t *response, const char *title)
{
   response->status_code = 200;
   response->content_type = "text/html; charset=utf-8";
   response->body_length = 0;
   response->body[0] = '\0';

   response_append(response,
                   "<!doctype html><html><head><meta charset=\"utf-8\">"
                   "<title>%s</title>"
                   "<style>body{font-family:sans-serif;max-width:48rem;margin:2rem auto;padding:0 1rem;}"
                   "table{border-collapse:collapse;width:100%%;}"
                   "th,td{border:1px solid #999;padding:.35rem;text-align:left;}"
                   "code{background:#eee;padding:.1rem .2rem;}"
                   "nav a{margin-right:1rem;}</style></head><body>"
                   "<nav><a href=\"/\">Home</a><a href=\"/status\">Status</a>"
                   "<a href=\"/files\">Files</a><a href=\"/framebuffer\">Framebuffer</a></nav>"
                   "<h1>%s</h1>",
                   title, title);
}

static void response_end_html(webserver_response_t *response)
{
   response_append(response, "</body></html>");
}

static const char *sdpcm_channel_name(uint8_t channel)
{
   switch (channel) {
      case 0:
         return "control";
      case 1:
         return "event";
      case 2:
         return "data";
      default:
         return "unknown";
   }
}

static const char *ethertype_name(uint16_t ethertype)
{
   switch (ethertype) {
      case 0x0800:
         return "IPv4";
      case 0x0806:
         return "ARP";
      case 0x8100:
         return "802.1Q";
      case 0x86dd:
         return "IPv6";
      case 0x886c:
         return "Broadcom event";
      case 0x888e:
         return "EAPOL";
      default:
         return "other";
   }
}

static const char *cdc_command_name(uint32_t command)
{
   switch (command) {
      case 0u:
         return "WLC_GET_MAGIC";
      case 1u:
         return "WLC_GET_VERSION";
      case 2u:
         return "WLC_UP";
      case 268u:
         return "WLC_SET_WSEC_PMK";
      case 3u:
         return "WLC_DOWN";
      case 20u:
         return "WLC_SET_INFRA";
      case 22u:
         return "WLC_SET_AUTH";
      case 26u:
         return "WLC_SET_SSID";
      case 165u:
         return "WLC_SET_WPA_AUTH";
      case 134u:
         return "WLC_SET_WSEC";
      case 262u:
         return "WLC_GET_VAR";
      case 263u:
         return "WLC_SET_VAR";
      default:
         return "other";
   }
}

static const char *tx_probe_command_name(wifi_sdio_tx_probe_command_t command)
{
   switch (command) {
      case WIFI_SDIO_TX_PROBE_COMMAND_MAGIC:
         return "WLC_GET_MAGIC";
      case WIFI_SDIO_TX_PROBE_COMMAND_INFRA:
         return "WLC_SET_INFRA";
      case WIFI_SDIO_TX_PROBE_COMMAND_AUTH:
         return "WLC_SET_AUTH";
      case WIFI_SDIO_TX_PROBE_COMMAND_SSID:
         return "WLC_SET_SSID";
      case WIFI_SDIO_TX_PROBE_COMMAND_WPA_AUTH:
         return "WLC_SET_WPA_AUTH";
      case WIFI_SDIO_TX_PROBE_COMMAND_WSEC:
         return "WLC_SET_WSEC";
      case WIFI_SDIO_TX_PROBE_COMMAND_PMK:
         return "WLC_SET_WSEC_PMK";
      case WIFI_SDIO_TX_PROBE_COMMAND_POWERSAVE_OFF:
         return "WLC_SET_PM(0)";
      case WIFI_SDIO_TX_PROBE_COMMAND_TXGLOM_OFF:
         return "bus:txglom=0";
      case WIFI_SDIO_TX_PROBE_COMMAND_ROAM_OFF:
         return "roam_off=1";
      case WIFI_SDIO_TX_PROBE_COMMAND_JOIN:
         return "JOIN_SEQUENCE";
      case WIFI_SDIO_TX_PROBE_COMMAND_UP:
         return "WLC_UP";
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WSEC:
         return "WLC_GET_WSEC";
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_WPA_AUTH:
         return "WLC_GET_WPA_AUTH";
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_AUTH:
         return "WLC_GET_AUTH";
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_INFRA:
         return "WLC_GET_INFRA";
      case WIFI_SDIO_TX_PROBE_COMMAND_GET_SUP_WPA:
         return "GET bsscfg:sup_wpa";
      case WIFI_SDIO_TX_PROBE_COMMAND_VERSION:
      default:
         return "WLC_GET_VERSION";
   }
}

static const char *brcm_event_probe_result(const sdio_probe_result_t *sdio_probe)
{
   if (!sdio_probe->sdpcm_brcm_event_probe_attempted)
      return "not Broadcom event traffic";

   if (!sdio_probe->sdpcm_brcm_event_probe_success)
      return "frame too short or read failed";

   if (!sdio_probe->sdpcm_brcm_event_oui_match)
      return "OUI mismatch";

   if (!sdio_probe->sdpcm_brcm_event_version_valid)
      return "unexpected version";

   return "Broadcom event header decoded";
}

static const char *brcm_event_name(uint32_t event_type)
{
   switch (event_type) {
      case 0x7ffffffeu:
         return "WLC_E_NONE";
      case 0u:
         return "WLC_E_SET_SSID";
      case 1u:
         return "WLC_E_JOIN";
      case 2u:
         return "WLC_E_START";
      case 3u:
         return "WLC_E_AUTH";
      case 4u:
         return "WLC_E_AUTH_IND";
      case 5u:
         return "WLC_E_DEAUTH";
      case 6u:
         return "WLC_E_DEAUTH_IND";
      case 7u:
         return "WLC_E_ASSOC";
      case 8u:
         return "WLC_E_ASSOC_IND";
      case 9u:
         return "WLC_E_REASSOC";
      case 10u:
         return "WLC_E_REASSOC_IND";
      case 11u:
         return "WLC_E_DISASSOC";
      case 12u:
         return "WLC_E_DISASSOC_IND";
      case 16u:
         return "WLC_E_LINK";
      case 46u:
         return "WLC_E_PSK_SUP";
      case 54u:
         return "WLC_E_IF";
      default:
         return "other";
   }
}

static const char *brcm_event_status_name(uint32_t event_type, uint32_t status)
{
   if (event_type == 46u) {
      switch (status) {
         case 256u:
            return "WLC_SUP_DISCONNECTED";
         case 257u:
            return "WLC_SUP_CONNECTING";
         case 258u:
            return "WLC_SUP_IDREQUIRED";
         case 259u:
            return "WLC_SUP_AUTHENTICATING";
         case 260u:
            return "WLC_SUP_AUTHENTICATED / WLC_SUP_KEYXCHANGE_WAIT_M1";
         case 261u:
            return "WLC_SUP_KEYXCHANGE / WLC_SUP_KEYXCHANGE_PREP_M2";
         case 262u:
            return "WLC_SUP_KEYED";
         case 263u:
            return "WLC_SUP_TIMEOUT";
         case 264u:
            return "WLC_SUP_LAST_BASIC_STATE / WLC_SUP_KEYXCHANGE_WAIT_M3";
         case 265u:
            return "WLC_SUP_KEYXCHANGE_PREP_M4";
         case 266u:
            return "WLC_SUP_KEYXCHANGE_WAIT_G1";
         case 267u:
            return "WLC_SUP_KEYXCHANGE_PREP_G2";
         default:
            return "other supplicant status";
      }
   }

   if (status >= 512u) {
      switch (status) {
         case 512u:
            return "WLC_DOT11_SC_SUCCESS";
         case 513u:
            return "WLC_DOT11_SC_FAILURE";
         case 522u:
            return "WLC_DOT11_SC_CAP_MISMATCH";
         case 523u:
            return "WLC_DOT11_SC_REASSOC_FAIL";
         case 524u:
            return "WLC_DOT11_SC_ASSOC_FAIL";
         case 525u:
            return "WLC_DOT11_SC_AUTH_MISMATCH";
         case 526u:
            return "WLC_DOT11_SC_AUTH_SEQ";
         case 527u:
            return "WLC_DOT11_SC_AUTH_CHALLENGE_FAIL";
         case 528u:
            return "WLC_DOT11_SC_AUTH_TIMEOUT";
         case 549u:
            return "WLC_DOT11_SC_DECLINED";
         case 550u:
            return "WLC_DOT11_SC_INVALID_PARAMS";
         case 555u:
            return "WLC_DOT11_SC_INVALID_AKMP";
         case 566u:
            return "WLC_DOT11_SC_INVALID_MDID";
         default:
            return "other 802.11 status";
      }
   }

   switch (status) {
      case 0u:
         return "WLC_E_STATUS_SUCCESS";
      case 1u:
         return "WLC_E_STATUS_FAIL";
      case 2u:
         return "WLC_E_STATUS_TIMEOUT";
      case 3u:
         return "WLC_E_STATUS_NO_NETWORKS";
      case 4u:
         return "WLC_E_STATUS_ABORT";
      case 5u:
         return "WLC_E_STATUS_NO_ACK";
      case 6u:
         return "WLC_E_STATUS_UNSOLICITED";
      case 7u:
         return "WLC_E_STATUS_ATTEMPT";
      case 8u:
         return "WLC_E_STATUS_PARTIAL";
      case 9u:
         return "WLC_E_STATUS_NEWSCAN";
      case 10u:
         return "WLC_E_STATUS_NEWASSOC";
      case 11u:
         return "WLC_E_STATUS_11HQUIET";
      case 12u:
         return "WLC_E_STATUS_SUPPRESS";
      case 13u:
         return "WLC_E_STATUS_NOCHANS";
      case 14u:
         return "WLC_E_STATUS_CCXFASTRM";
      case 15u:
         return "WLC_E_STATUS_CS_ABORT";
      case 16u:
         return "WLC_E_STATUS_ERROR";
      default:
         return "other event status";
   }
}

static const char *brcm_link_reason_name(uint32_t reason)
{
   switch (reason) {
      case 0u:
         return "initial assoc";
      case 1u:
         return "beacon loss";
      case 2u:
         return "disassoc";
      case 3u:
         return "assoc recreate failed";
      case 4u:
         return "bsscfg down";
      default:
         return "other link reason";
   }
}

static const char *join_event_result(uint32_t event_type, uint32_t event_status, uint32_t event_reason)
{
   switch (event_type) {
      case 0u:
         switch (event_status) {
            case 0u:
               return "SSID accepted, waiting for auth/assoc";
            case 3u:
               return "SSID not found";
            case 2u:
               return "SSID join timed out";
            default:
               return "SSID join request failed";
         }
      case 3u:
         if (event_status == 0u)
            return "802.11 authentication succeeded";
         if (event_status == 6u)
            return "auth frame was unsolicited";
         return "802.11 authentication failed";
      case 7u:
      case 9u:
         if (event_status == 0u)
            return "association succeeded";
         return "association failed";
      case 16u:
         if (event_reason == 0u)
            return "link reported up";
         return brcm_link_reason_name(event_reason);
      case 46u:
         if (event_status == 262u)
            return "WPA key exchange completed";
         if (event_status == 260u && event_reason == 527u)
            return "waiting for M1 timed out";
         if (event_status == 264u && event_reason == 527u)
            return "waiting for M3 timed out; passphrase likely wrong";
         if (event_status == 266u && event_reason == 527u)
            return "waiting for G1 timed out";
         return "WPA key exchange still in progress or failed";
      default:
         return NULL;
   }
}

static bool join_event_result_prefers_span(uint32_t event_type, uint32_t event_status, uint32_t event_reason)
{
   switch (event_type) {
      case 0u:
         return event_status == 0u;
      case 3u:
         return event_status == 0u || event_status == 6u;
      case 7u:
      case 9u:
         return event_status == 0u;
      case 46u:
         if (event_status == 262u)
            return false;
         if ((event_status == 260u || event_status == 264u || event_status == 266u) && event_reason == 527u)
            return false;
         return true;
      default:
         return false;
   }
}

static const char *join_event_phase(uint32_t event_type, uint32_t event_status, uint32_t event_reason)
{
   switch (event_type) {
      case 0u:
         if (event_status == 0u)
            return "ssid accepted";
         return "ssid failed";
      case 3u:
         if (event_status == 0u)
            return "auth ok";
         return "auth failed";
      case 7u:
      case 9u:
         if (event_status == 0u)
            return "assoc ok";
         return "assoc failed";
      case 16u:
         if (event_reason == 0u)
            return "link up";
         return "link issue";
      case 46u:
         if (event_status == 262u)
            return "wpa complete";
         if ((event_status == 260u || event_status == 264u || event_status == 266u) && event_reason == 527u)
            return "wpa failed";
         return "wpa pending";
      default:
         return "unclassified";
   }
}

static const char *join_probe_phase(const sdio_probe_result_t *sdio_probe)
{
   static char phase_text[96];
   const char *first_phase;
   const char *last_phase;

   if (!sdio_probe->tx_control_probe_attempted)
      return "not attempted";

   if (!sdio_probe->tx_control_probe_success)
      return "write failed";

   if (sdio_probe->sdpcm_brcm_event_count == 0u)
      return "no event phase observed";

   first_phase = join_event_phase(sdio_probe->sdpcm_brcm_event_first_type,
                                  sdio_probe->sdpcm_brcm_event_first_status,
                                  sdio_probe->sdpcm_brcm_event_first_reason);
   last_phase = join_event_phase(sdio_probe->sdpcm_brcm_event_type,
                                 sdio_probe->sdpcm_brcm_event_status,
                                 sdio_probe->sdpcm_brcm_event_reason);

   if (sdio_probe->sdpcm_brcm_event_count == 1u || strcmp(first_phase, last_phase) == 0)
      return last_phase;

   snprintf(phase_text, sizeof(phase_text), "%s -> %s", first_phase, last_phase);
   return phase_text;
}

static const char *join_probe_span_result(const sdio_probe_result_t *sdio_probe)
{
   static char span_text[192];
   const char *first_result;
   const char *last_result;

   if (!sdio_probe->tx_control_probe_attempted)
      return "not attempted";

   if (!sdio_probe->tx_control_probe_success)
      return "CMD53 write failed before join response";

   if (sdio_probe->sdpcm_brcm_event_count == 0u)
      return "no Broadcom event span observed";

   first_result = join_event_result(sdio_probe->sdpcm_brcm_event_first_type,
                                    sdio_probe->sdpcm_brcm_event_first_status,
                                    sdio_probe->sdpcm_brcm_event_first_reason);
   last_result = join_event_result(sdio_probe->sdpcm_brcm_event_type,
                                   sdio_probe->sdpcm_brcm_event_status,
                                   sdio_probe->sdpcm_brcm_event_reason);

   if (first_result == NULL)
      first_result = "first event not classified yet";
   if (last_result == NULL)
      last_result = "last event not classified yet";

   if (sdio_probe->sdpcm_brcm_event_count == 1u)
      return first_result;

   snprintf(span_text, sizeof(span_text), "progressed from %s to %s", first_result, last_result);
   return span_text;
}

static const char *join_probe_result(const sdio_probe_result_t *sdio_probe)
{
   const char *event_result;
   const char *first_event_result;

   if (!sdio_probe->tx_control_probe_attempted)
      return "not attempted";

   if (!sdio_probe->tx_control_probe_success)
      return "CMD53 write failed before join response";

   if (sdio_probe->tx_control_probe_multi_step
      && sdio_probe->tx_control_probe_steps_completed < sdio_probe->tx_control_probe_steps_requested) {
      return "join burst stopped before final step";
   }

   if (sdio_probe->sdpcm_brcm_event_msg_probe_success) {
      event_result = join_event_result(sdio_probe->sdpcm_brcm_event_type,
                                       sdio_probe->sdpcm_brcm_event_status,
                                       sdio_probe->sdpcm_brcm_event_reason);
      if (event_result != NULL) {
         first_event_result = join_event_result(sdio_probe->sdpcm_brcm_event_first_type,
                                                sdio_probe->sdpcm_brcm_event_first_status,
                                                sdio_probe->sdpcm_brcm_event_first_reason);
         if (sdio_probe->sdpcm_brcm_event_count > 1u
            && first_event_result != NULL
            && strcmp(first_event_result, event_result) != 0
            && join_event_result_prefers_span(sdio_probe->sdpcm_brcm_event_type,
                                              sdio_probe->sdpcm_brcm_event_status,
                                              sdio_probe->sdpcm_brcm_event_reason)) {
            return join_probe_span_result(sdio_probe);
         }
         return event_result;
      }
   }

   if (sdio_probe->sdpcm_cdc_header_probe_success) {
      if ((sdio_probe->sdpcm_cdc_flags & 0x01u) != 0u)
         return "firmware returned CDC ioctl error";
      if (sdio_probe->sdpcm_cdc_status != 0u)
         return "firmware returned non-zero CDC status";
   }

   if (sdio_probe->frame_header_probe_success
      && !(sdio_probe->frame_header_size == 0u && sdio_probe->frame_header_size_complement == 0u)) {
      return "response frame captured but join outcome is not classified yet";
   }

   return "no join response observed yet";
}

static const char *brcm_event_msg_result(const sdio_probe_result_t *sdio_probe)
{
   if (!sdio_probe->sdpcm_brcm_event_msg_probe_attempted)
      return "not attempted";

   if (!sdio_probe->sdpcm_brcm_event_msg_probe_success)
      return "frame too short or read failed";

   if (!sdio_probe->sdpcm_brcm_event_msg_datalen_sane)
      return "payload extends past frame";

   return "event message decoded";
}

static const char *tx_post_write_effect(const sdio_probe_result_t *sdio_probe)
{
   if (!sdio_probe->tx_control_probe_attempted)
      return "not attempted";

   if (!sdio_probe->tx_control_probe_success)
      return "write failed before response sampling";

   if (!sdio_probe->tx_control_post_state_probe_success)
      return "write completed but post-state sampling failed";

   if (sdio_probe->frame_header_probe_success
      && !(sdio_probe->frame_header_size == 0u && sdio_probe->frame_header_size_complement == 0u)) {
      return "response frame available";
   }

   if (sdio_probe->tx_control_post_read_frame_byte_count != sdio_probe->read_frame_byte_count)
      return "frame-count changed";

   if (sdio_probe->tx_control_post_int_status != sdio_probe->sdio_int_status)
      return "interrupt status changed";

   if (sdio_probe->tx_control_post_to_sb_mailbox != sdio_probe->sdio_to_sb_mailbox)
      return "to-SB mailbox changed";

   if (sdio_probe->tx_control_post_to_host_mailbox_data != sdio_probe->sdio_to_host_mailbox_data)
      return "to-host mailbox changed";

   return "no immediate observable change";
}

static void render_home_page(webserver_response_t *response)
{
   const wifi_status_t status = wifi_get_status();

   response_begin_html(response, "Pi1MHz WiFi");
   response_append(response,
                   "<p>This HTTP layer is ready to be connected to the future TCP/IP stack.</p>"
                   "<p>Current WiFi state: <strong>%s</strong></p>"
                   "<ul>"
                   "<li><a href=\"/files\">Copy files to and from the SD card</a></li>"
                   "<li><a href=\"/framebuffer\">Inspect framebuffer state and future PNG export</a></li>"
                   "<li><a href=\"/status\">Review WiFi and board status</a></li>"
                   "</ul>",
                   wifi_state_name(status.state));
   response_end_html(response);
}

static void render_status_page(webserver_response_t *response)
{
   const wifi_config_t *config = wifi_get_config();
   const wifi_status_t status = wifi_get_status();
   const wifi_lwip_context_t *lwip_context = wifi_lwip_get_context();
   const sdio_probe_result_t *sdio_probe = sdio_get_probe_result();

   response_begin_html(response, "Status");
   response_append(response,
                   "<table>"
                   "<tr><th>Build info</th><td>%s</td></tr>"
                   "<tr><th>WiFi state</th><td>%s</td></tr>"
                   "<tr><th>SSID configured</th><td>%s</td></tr>"
                   "<tr><th>Password configured</th><td>%s</td></tr>"
                   "<tr><th>Hostname</th><td>%s</td></tr>"
                   "<tr><th>IP mode</th><td>%s</td></tr>"
                   "<tr><th>Static IP</th><td>%s</td></tr>"
                   "<tr><th>Netmask</th><td>%s</td></tr>"
                   "<tr><th>Gateway</th><td>%s</td></tr>"
                   "<tr><th>DNS</th><td>%s</td></tr>"
                   "<tr><th>HTTP port</th><td>%u</td></tr>"
                   "<tr><th>SDIO probe enabled</th><td>%s</td></tr>"
                   "<tr><th>SDIO TX probe enabled</th><td>%s</td></tr>"
                   "<tr><th>SDIO host backend</th><td>%s</td></tr>"
                   "<tr><th>SDIO host status</th><td>%s</td></tr>"
                   "<tr><th>SDIO RX sweep cap</th><td>%u frames</td></tr>"
                   "<tr><th>SDIO TX probe command</th><td>%s</td></tr>"
                   "<tr><th>SDIO probe attempted</th><td>%s</td></tr>"
                   "<tr><th>SDIO probe result</th><td>%s</td></tr>"
                   "<tr><th>SDIO OCR</th><td>0x%08lx</td></tr>"
                   "<tr><th>SDIO functions</th><td>%u</td></tr>"
                   "<tr><th>SDIO memory present</th><td>%s</td></tr>"
                   "<tr><th>SDIO 1.8V support</th><td>%s</td></tr>"
                   "<tr><th>CCCR reads</th><td>%s</td></tr>"
                   "<tr><th>CCCR revision</th><td>%u</td></tr>"
                   "<tr><th>SDIO revision</th><td>%u</td></tr>"
                   "<tr><th>IO enable</th><td>0x%02x</td></tr>"
                   "<tr><th>IO ready</th><td>0x%02x</td></tr>"
                   "<tr><th>Bus control</th><td>0x%02x</td></tr>"
                   "<tr><th>Function setup attempted</th><td>%s</td></tr>"
                   "<tr><th>Function setup result</th><td>%s</td></tr>"
                   "<tr><th>Requested IO enable</th><td>0x%02x</td></tr>"
                   "<tr><th>Configured IO enable</th><td>0x%02x</td></tr>"
                   "<tr><th>Configured IO ready</th><td>0x%02x</td></tr>"
                   "<tr><th>Function 1 block size</th><td>%u</td></tr>"
                   "<tr><th>Function 2 block size</th><td>%u</td></tr>"
                   "<tr><th>Clock probe</th><td>%s</td></tr>"
                   "<tr><th>Clock probe result</th><td>%s</td></tr>"
                   "<tr><th>Chip clock CSR initial</th><td>0x%02x</td></tr>"
                   "<tr><th>Chip clock CSR requested</th><td>0x%02x</td></tr>"
                   "<tr><th>Chip clock CSR final</th><td>0x%02x</td></tr>"
                   "<tr><th>Power probe</th><td>%s</td></tr>"
                   "<tr><th>Wakeup control</th><td>0x%02x</td></tr>"
                   "<tr><th>Sleep CSR</th><td>0x%02x</td></tr>"
                   "<tr><th>KSO probe</th><td>%s</td></tr>"
                   "<tr><th>KSO probe result</th><td>%s</td></tr>"
                   "<tr><th>KSO requested</th><td>0x%02x</td></tr>"
                   "<tr><th>KSO final</th><td>0x%02x</td></tr>"
                   "<tr><th>Mailbox probe</th><td>%s</td></tr>"
                   "<tr><th>SDIO core base</th><td>0x%08lx</td></tr>"
                   "<tr><th>SDIO int status</th><td>0x%08lx</td></tr>",
                   get_info_string(),
                   wifi_state_name(status.state),
                   status.has_ssid ? "yes" : "no",
                   status.has_password ? "yes" : "no",
                   config->hostname,
                   wifi_ip_mode_name(config->ip_mode),
                   config->ip_address[0] != '\0' ? config->ip_address : "not set",
                   config->netmask[0] != '\0' ? config->netmask : "not set",
                   config->gateway[0] != '\0' ? config->gateway : "not set",
                   config->dns[0] != '\0' ? config->dns : "not set",
                   (unsigned int)status.http_port,
                   status.sdio_probe_enabled ? "yes" : "no",
                   status.sdio_tx_probe_enabled ? "yes" : "no",
                   sdio_host_backend_name(),
                   sdio_host_last_error()[0] != '\0' ? sdio_host_last_error() : "ready",
                   (unsigned int)config->sdio_rx_sweep_limit,
                   tx_probe_command_name(status.sdio_tx_probe_command),
                   sdio_probe->attempted ? "yes" : "no",
                   sdio_probe->attempted ? (sdio_probe->success ? "CMD5 responded" : "CMD5 failed") : "not attempted",
                   (unsigned long)sdio_probe->ocr.raw_ocr,
                   (unsigned int)sdio_probe->ocr.function_count,
                   sdio_probe->ocr.memory_present ? "yes" : "no",
                   sdio_probe->ocr.supports_1p8v ? "yes" : "no",
                   sdio_probe->cccr_read_success ? "CMD52 reads succeeded" : "not available",
                   (unsigned int)sdio_probe->cccr_revision,
                   (unsigned int)sdio_probe->sd_revision,
                   (unsigned int)sdio_probe->io_enable,
                   (unsigned int)sdio_probe->io_ready,
                   (unsigned int)sdio_probe->bus_interface_control,
                   sdio_probe->function_setup_attempted ? "yes" : "no",
                   sdio_probe->function_setup_attempted ? (sdio_probe->function_setup_success ? "functions 1 and 2 enabled" : "setup failed") : "not attempted",
                   (unsigned int)sdio_probe->requested_io_enable,
                   (unsigned int)sdio_probe->configured_io_enable,
                   (unsigned int)sdio_probe->configured_io_ready,
                   (unsigned int)sdio_probe->function1_block_size,
                   (unsigned int)sdio_probe->function2_block_size,
                   sdio_probe->clock_probe_attempted ? "yes" : "no",
                   sdio_probe->clock_probe_attempted ? (sdio_probe->clock_probe_success ? "ALP available" : "ALP request failed") : "not attempted",
                   (unsigned int)sdio_probe->chip_clock_csr_initial,
                   (unsigned int)sdio_probe->chip_clock_csr_requested,
                   (unsigned int)sdio_probe->chip_clock_csr_final,
                   sdio_probe->power_probe_success ? "wakeup and sleep registers read" : "not available",
                   (unsigned int)sdio_probe->wakeup_control,
                   (unsigned int)sdio_probe->sleep_control_status,
                   sdio_probe->kso_probe_attempted ? "yes" : "no",
                   sdio_probe->kso_probe_attempted ? (sdio_probe->kso_probe_success ? "wl kso and devon asserted" : "wake polling failed") : "not attempted",
                   (unsigned int)sdio_probe->kso_control_requested,
                   (unsigned int)sdio_probe->kso_control_final,
                   sdio_probe->mailbox_probe_success ? "sdio core registers read" : "not available",
                   (unsigned long)sdio_probe->sdio_core_base,
                   (unsigned long)sdio_probe->sdio_int_status);
   response_append(response,
                   "<tr><th>Interrupt ack</th><td>%s</td></tr>"
                   "<tr><th>Interrupt ack result</th><td>%s</td></tr>"
                   "<tr><th>Interrupt ack value</th><td>0x%08lx</td></tr>"
                   "<tr><th>SDIO int status after ack</th><td>0x%08lx</td></tr>"
                   "<tr><th>SDIO int host mask</th><td>0x%08lx</td></tr>"
                   "<tr><th>Interrupt mask write</th><td>%s</td></tr>"
                   "<tr><th>Interrupt mask result</th><td>%s</td></tr>"
                   "<tr><th>Requested int host mask</th><td>0x%08lx</td></tr>"
                   "<tr><th>SDIO int host mask after write</th><td>0x%08lx</td></tr>"
                   "<tr><th>To SB mailbox</th><td>0x%08lx</td></tr>"
                   "<tr><th>To host mailbox data</th><td>0x%08lx</td></tr>"
                   "<tr><th>Function 2 probe</th><td>%s</td></tr>"
                   "<tr><th>Function 2 info</th><td>0x%02x</td></tr>"
                   "<tr><th>Function 2 watermark</th><td>0x%02x</td></tr>"
                   "<tr><th>Read frame byte count</th><td>%u</td></tr>"
                   "<tr><th>Frame header probe</th><td>%s</td></tr>"
                   "<tr><th>Frame header result</th><td>%s</td></tr>"
                   "<tr><th>Frame size</th><td>%u</td></tr>"
                   "<tr><th>Frame size complement</th><td>0x%04x</td></tr>"
                   "<tr><th>SDPCM header</th><td>%s</td></tr>"
                   "<tr><th>SDPCM sequence</th><td>%u</td></tr>"
                   "<tr><th>SDPCM channel</th><td>%s (%u)</td></tr>"
                   "<tr><th>SDPCM channel/flags</th><td>0x%02x</td></tr>"
                   "<tr><th>SDPCM next length</th><td>%u</td></tr>"
                   "<tr><th>SDPCM header length</th><td>%u</td></tr>"
                   "<tr><th>Expected SDPCM header length</th><td>%u</td></tr>"
                   "<tr><th>SDPCM header layout</th><td>%s</td></tr>"
                   "<tr><th>SDPCM header sanity</th><td>%s</td></tr>"
                   "<tr><th>Post-header probe</th><td>%s</td></tr>"
                   "<tr><th>Post-header result</th><td>%s</td></tr>"
                   "<tr><th>Post-header bytes</th><td>%u</td></tr>"
                   "<tr><th>Post-header prefix</th><td>%02x %02x %02x %02x</td></tr>"
                   "<tr><th>CDC command prefix</th><td>%s</td></tr>"
                   "<tr><th>BDC header</th><td>%s</td></tr>"
                   "<tr><th>SDPCM flow control</th><td>0x%02x</td></tr>"
                   "<tr><th>SDPCM bus credit</th><td>%u</td></tr>",
                   sdio_probe->interrupt_ack_attempted ? "yes" : "no",
                   sdio_probe->sdio_interrupt_ack_value == 0 ? "no pending host interrupt bits" : (sdio_probe->interrupt_ack_success ? "status write completed" : "status write failed"),
                   (unsigned long)sdio_probe->sdio_interrupt_ack_value,
                   (unsigned long)sdio_probe->sdio_int_status_after_ack,
                   (unsigned long)sdio_probe->sdio_int_host_mask,
                   sdio_probe->interrupt_mask_write_attempted ? "yes" : "no",
                   sdio_probe->interrupt_mask_write_attempted ? (sdio_probe->interrupt_mask_write_success ? "mask configured" : "mask write failed") : "not attempted",
                   (unsigned long)sdio_probe->sdio_int_host_mask_requested,
                   (unsigned long)sdio_probe->sdio_int_host_mask_after_write,
                   (unsigned long)sdio_probe->sdio_to_sb_mailbox,
                   (unsigned long)sdio_probe->sdio_to_host_mailbox_data,
                   sdio_probe->function2_probe_success ? "f2 and frame-count registers read" : "not available",
                   (unsigned int)sdio_probe->function2_info,
                   (unsigned int)sdio_probe->function2_watermark,
                   (unsigned int)sdio_probe->read_frame_byte_count,
                   sdio_probe->frame_header_probe_attempted ? "yes" : "no",
                   !sdio_probe->frame_header_probe_success ? "header read failed" : ((sdio_probe->frame_header_size == 0 && sdio_probe->frame_header_size_complement == 0) ? "no frame available" : (sdio_probe->frame_header_valid ? "valid frame tag" : "invalid frame tag")),
                   (unsigned int)sdio_probe->frame_header_size,
                   (unsigned int)sdio_probe->frame_header_size_complement,
                   sdio_probe->sdpcm_header_read_success ? "decoded" : "not decoded",
                   (unsigned int)sdio_probe->sdpcm_sequence,
                   sdpcm_channel_name(sdio_probe->sdpcm_channel),
                   (unsigned int)sdio_probe->sdpcm_channel,
                   (unsigned int)sdio_probe->sdpcm_channel_and_flags,
                   (unsigned int)sdio_probe->sdpcm_next_length,
                   (unsigned int)sdio_probe->sdpcm_header_length,
                   (unsigned int)sdio_probe->sdpcm_expected_header_length,
                   sdio_probe->sdpcm_header_read_success ? (sdio_probe->sdpcm_header_length_expected ? "matches channel" : "unexpected for channel") : "not decoded",
                   sdio_probe->sdpcm_header_read_success ? (sdio_probe->sdpcm_header_sane ? "sane" : "not sane") : "not decoded",
                   sdio_probe->sdpcm_post_header_probe_attempted ? "yes" : "no",
                   !sdio_probe->sdpcm_post_header_probe_attempted ? "not needed" : (sdio_probe->sdpcm_post_header_probe_success ? "prefix captured" : "prefix read failed or frame too short"),
                   (unsigned int)sdio_probe->sdpcm_post_header_bytes_requested,
                   (unsigned int)sdio_probe->sdpcm_post_header_prefix0,
                   (unsigned int)sdio_probe->sdpcm_post_header_prefix1,
                   (unsigned int)sdio_probe->sdpcm_post_header_prefix2,
                   (unsigned int)sdio_probe->sdpcm_post_header_prefix3,
                   sdio_probe->sdpcm_cdc_prefix_decoded ? "decoded" : "not control",
                   sdio_probe->sdpcm_bdc_header_decoded ? "decoded" : "not data",
                   (unsigned int)sdio_probe->sdpcm_wireless_flow_control,
                   (unsigned int)sdio_probe->sdpcm_bus_data_credit);
   response_append(response,
                   "<tr><th>CDC header probe</th><td>%s</td></tr>"
                   "<tr><th>CDC header result</th><td>%s</td></tr>"
                   "<tr><th>CDC command value</th><td>0x%08lx</td></tr>"
                   "<tr><th>CDC command name</th><td>%s</td></tr>"
                   "<tr><th>CDC length</th><td>0x%08lx</td></tr>"
                   "<tr><th>CDC request bytes</th><td>%u</td></tr>"
                   "<tr><th>CDC response bytes</th><td>%u</td></tr>"
                   "<tr><th>CDC payload bytes in frame</th><td>%u</td></tr>"
                   "<tr><th>CDC payload fit</th><td>%s</td></tr>"
                   "<tr><th>CDC flags</th><td>0x%08lx</td></tr>"
                   "<tr><th>CDC interface</th><td>%u</td></tr>"
                   "<tr><th>CDC request ID</th><td>%u</td></tr>"
                   "<tr><th>CDC set flag</th><td>%s</td></tr>"
                   "<tr><th>CDC error flag</th><td>%s</td></tr>"
                   "<tr><th>CDC status</th><td>0x%08lx</td></tr>"
                   "<tr><th>CDC status check</th><td>%s</td></tr>"
                   "<tr><th>CDC payload word0 probe</th><td>%s</td></tr>"
                   "<tr><th>CDC payload word0 result</th><td>%s</td></tr>"
                   "<tr><th>CDC payload word0</th><td>0x%08lx</td></tr>"
                   "<tr><th>CDC payload word1 probe</th><td>%s</td></tr>"
                   "<tr><th>CDC payload word1 result</th><td>%s</td></tr>"
                   "<tr><th>CDC payload word1</th><td>0x%08lx</td></tr>"
                   "<tr><th>CDC magic check</th><td>%s</td></tr>"
                   "<tr><th>CDC version check</th><td>%s</td></tr>"
                   "<tr><th>CDC request metadata match</th><td>%s</td></tr>"
                   "<tr><th>TX control template</th><td>%s</td></tr>"
                   "<tr><th>TX control probe</th><td>%s</td></tr>"
                   "<tr><th>TX control probe result</th><td>%s</td></tr>"
                   "<tr><th>TX control probe response</th><td>0x%08lx</td></tr>"
                   "<tr><th>TX control probe interrupt</th><td>0x%08lx</td></tr>"
                   "<tr><th>TX control probe error</th><td>0x%08lx</td></tr>"
                   "<tr><th>TX post-state probe</th><td>%s</td></tr>"
                   "<tr><th>TX post-state result</th><td>%s</td></tr>"
                   "<tr><th>TX post-state read frame count</th><td>%u</td></tr>"
                   "<tr><th>TX post-state int status</th><td>0x%08lx</td></tr>"
                   "<tr><th>TX post-state to SB mailbox</th><td>0x%08lx</td></tr>"
                   "<tr><th>TX post-state to host mailbox</th><td>0x%08lx</td></tr>"
                   "<tr><th>RX frame sweep</th><td>%s</td></tr>"
                   "<tr><th>RX frame sweep result</th><td>%s</td></tr>"
                   "<tr><th>RX frames decoded</th><td>%u/%u</td></tr>"
                   "<tr><th>RX sweep tail state</th><td>%s</td></tr>"
                   "<tr><th>TX post-write effect</th><td>%s</td></tr>"
                   "<tr><th>TX response command match</th><td>%s</td></tr>"
                   "<tr><th>TX template command</th><td>0x%08lx (%s)</td></tr>"
                   "<tr><th>TX template payload bytes</th><td>%u</td></tr>"
                   "<tr><th>TX template request ID</th><td>%u</td></tr>"
                   "<tr><th>TX template interface</th><td>%u</td></tr>"
                   "<tr><th>TX template frame size</th><td>%u</td></tr>"
                   "<tr><th>TX template frame complement</th><td>0x%04x</td></tr>"
                   "<tr><th>TX template SDPCM seq</th><td>%u</td></tr>"
                   "<tr><th>TX template SDPCM channel/flags</th><td>0x%02x</td></tr>"
                   "<tr><th>TX template SDPCM next length</th><td>%u</td></tr>"
                   "<tr><th>TX template SDPCM header length</th><td>%u</td></tr>"
                   "<tr><th>TX template flow control</th><td>0x%02x</td></tr>"
                   "<tr><th>TX template bus credit</th><td>%u</td></tr>"
                   "<tr><th>TX template CDC length</th><td>0x%08lx</td></tr>"
                   "<tr><th>TX template CDC flags</th><td>0x%08lx</td></tr>"
                   "<tr><th>TX template CDC status</th><td>0x%08lx</td></tr>"
                   "<tr><th>TX template payload word0</th><td>0x%08lx</td></tr>",
                   sdio_probe->sdpcm_cdc_header_probe_attempted ? "yes" : "no",
                   sdio_probe->sdpcm_cdc_header_probe_attempted ? (sdio_probe->sdpcm_cdc_header_probe_success ? "full CDC header captured" : "frame too short or read failed") : "not control",
                   (unsigned long)sdio_probe->sdpcm_cdc_cmd_prefix,
                   cdc_command_name(sdio_probe->sdpcm_cdc_cmd_prefix),
                   (unsigned long)sdio_probe->sdpcm_cdc_length,
                   (unsigned int)sdio_probe->sdpcm_cdc_request_length,
                   (unsigned int)sdio_probe->sdpcm_cdc_response_length,
                   (unsigned int)sdio_probe->sdpcm_cdc_payload_bytes_available,
                   sdio_probe->sdpcm_cdc_header_probe_success ? (sdio_probe->sdpcm_cdc_response_length_sane ? "fits frame" : "extends past frame") : "not decoded",
                   (unsigned long)sdio_probe->sdpcm_cdc_flags,
                   (unsigned int)sdio_probe->sdpcm_cdc_interface,
                   (unsigned int)sdio_probe->sdpcm_cdc_request_id,
                   sdio_probe->sdpcm_cdc_header_probe_success ? ((sdio_probe->sdpcm_cdc_flags & 0x02u) != 0u ? "yes" : "no") : "not decoded",
                   sdio_probe->sdpcm_cdc_header_probe_success ? ((sdio_probe->sdpcm_cdc_flags & 0x01u) != 0u ? "yes" : "no") : "not decoded",
                   (unsigned long)sdio_probe->sdpcm_cdc_status,
                   !sdio_probe->sdpcm_cdc_header_probe_success ? "not decoded"
                      : ((sdio_probe->sdpcm_cdc_flags & 0x01u) != 0u ? "firmware reported ioctl error"
                      : (sdio_probe->sdpcm_cdc_status == 0u ? "status clear" : "non-zero status without error flag")),
                   sdio_probe->sdpcm_cdc_payload_word0_probe_attempted ? "yes" : "no",
                   sdio_probe->sdpcm_cdc_payload_word0_probe_attempted ? (sdio_probe->sdpcm_cdc_payload_word0_probe_success ? "first payload word captured" : "payload read failed") : "not attempted",
                   (unsigned long)sdio_probe->sdpcm_cdc_payload_word0,
                   sdio_probe->sdpcm_cdc_payload_word1_probe_attempted ? "yes" : "no",
                   sdio_probe->sdpcm_cdc_payload_word1_probe_attempted ? (sdio_probe->sdpcm_cdc_payload_word1_probe_success ? "second payload word captured" : "payload read failed") : "not attempted",
                   (unsigned long)sdio_probe->sdpcm_cdc_payload_word1,
                   sdio_probe->sdpcm_cdc_cmd_prefix != 0u ? "not WLC_GET_MAGIC"
                      : (!sdio_probe->sdpcm_cdc_payload_word0_probe_success ? "magic payload not captured"
                      : (sdio_probe->sdpcm_cdc_payload_word0_magic_valid ? "matches WLC_IOCTL_MAGIC" : "unexpected magic value")),
                   sdio_probe->sdpcm_cdc_cmd_prefix != 1u ? "not WLC_GET_VERSION"
                      : (!sdio_probe->sdpcm_cdc_payload_word0_probe_success ? "version payload not captured"
                      : ((sdio_probe->sdpcm_cdc_payload_word0 == 1u || sdio_probe->sdpcm_cdc_payload_word0 == 2u)
                         ? "known ioctl version"
                         : "unexpected ioctl version")),
                   !sdio_probe->tx_control_probe_attempted ? "not attempted"
                      : (!sdio_probe->sdpcm_cdc_header_probe_success ? "response was not a decoded control header"
                      : ((sdio_probe->sdpcm_cdc_request_id == sdio_probe->tx_control_template_request_id)
                         && (sdio_probe->sdpcm_cdc_interface == sdio_probe->tx_control_template_interface)
                         ? "request ID and interface match"
                         : "request ID or interface differ")),
                   sdio_probe->tx_control_template_ready ? "prepared" : "not prepared",
                   sdio_probe->tx_control_probe_attempted ? "yes" : "no",
                   sdio_probe->tx_control_probe_attempted ? (sdio_probe->tx_control_probe_success ? "CMD53 write completed" : "CMD53 write failed") : "not attempted",
                   (unsigned long)sdio_probe->tx_control_probe_response0,
                   (unsigned long)sdio_probe->tx_control_probe_interrupt,
                   (unsigned long)sdio_probe->tx_control_probe_error,
                   sdio_probe->tx_control_post_state_probe_attempted ? "yes" : "no",
                   sdio_probe->tx_control_post_state_probe_attempted ? (sdio_probe->tx_control_post_state_probe_success ? "post-write state captured" : "post-write state read failed") : "not attempted",
                   (unsigned int)sdio_probe->tx_control_post_read_frame_byte_count,
                   (unsigned long)sdio_probe->tx_control_post_int_status,
                   (unsigned long)sdio_probe->tx_control_post_to_sb_mailbox,
                   (unsigned long)sdio_probe->tx_control_post_to_host_mailbox_data,
                   sdio_probe->rx_frame_sweep_attempted ? "yes" : "no",
                   !sdio_probe->rx_frame_sweep_attempted ? "not attempted"
                      : (sdio_probe->rx_frame_sweep_success ? "bounded sweep completed" : "frame sweep stopped by read failure"),
                   (unsigned int)sdio_probe->rx_frames_decoded,
                   (unsigned int)sdio_probe->rx_frame_sweep_limit,
                   !sdio_probe->rx_frame_sweep_attempted ? "not attempted"
                      : (!sdio_probe->rx_frame_sweep_success ? "unknown"
                      : (sdio_probe->rx_frame_sweep_more_pending ? "fixed limit reached with more data pending" : "queue drained or no more frames pending")),
                   tx_post_write_effect(sdio_probe),
                   !sdio_probe->tx_control_probe_attempted ? "not attempted"
                      : (!sdio_probe->frame_header_probe_success ? "no response frame decoded"
                      : (!sdio_probe->sdpcm_cdc_header_probe_success ? "response was not a decoded control header"
                      : (sdio_probe->sdpcm_cdc_cmd_prefix == sdio_probe->tx_control_template_command
                         ? "matches transmitted command"
                         : "different response command"))),
                   (unsigned long)sdio_probe->tx_control_template_command,
                   cdc_command_name(sdio_probe->tx_control_template_command),
                   (unsigned int)sdio_probe->tx_control_template_payload_length,
                   (unsigned int)sdio_probe->tx_control_template_request_id,
                   (unsigned int)sdio_probe->tx_control_template_interface,
                   (unsigned int)sdio_probe->tx_control_template_frame_size,
                   (unsigned int)sdio_probe->tx_control_template_frame_size_complement,
                   (unsigned int)sdio_probe->tx_control_template_sequence,
                   (unsigned int)sdio_probe->tx_control_template_channel_and_flags,
                   (unsigned int)sdio_probe->tx_control_template_next_length,
                   (unsigned int)sdio_probe->tx_control_template_header_length,
                   (unsigned int)sdio_probe->tx_control_template_wireless_flow_control,
                   (unsigned int)sdio_probe->tx_control_template_bus_data_credit,
                   (unsigned long)sdio_probe->tx_control_template_cdc_length,
                   (unsigned long)sdio_probe->tx_control_template_cdc_flags,
                   (unsigned long)sdio_probe->tx_control_template_cdc_status,
                   (unsigned long)sdio_probe->tx_control_template_payload_word0);
   response_append(response,
                   "<tr><th>TX probe mode</th><td>%s</td></tr>"
                   "<tr><th>TX probe steps</th><td>%u/%u</td></tr>"
                   "<tr><th>TX last command</th><td>0x%08lx (%s)</td></tr>"
                   "<tr><th>TX last request ID</th><td>%u</td></tr>"
                   "<tr><th>TX last SDPCM seq</th><td>%u</td></tr>",
                   sdio_probe->tx_control_probe_multi_step ? "multi-step join burst" : "single ioctl",
                   (unsigned int)sdio_probe->tx_control_probe_steps_completed,
                   (unsigned int)sdio_probe->tx_control_probe_steps_requested,
                   (unsigned long)sdio_probe->tx_control_probe_last_command,
                   cdc_command_name(sdio_probe->tx_control_probe_last_command),
                   (unsigned int)sdio_probe->tx_control_probe_last_request_id,
                   (unsigned int)sdio_probe->tx_control_probe_last_sequence);
   response_append(response,
                   "<tr><th>BDC version</th><td>%u</td></tr>"
                   "<tr><th>BDC version check</th><td>%s</td></tr>"
                   "<tr><th>BDC flags</th><td>0x%02x</td></tr>"
                   "<tr><th>BDC priority</th><td>0x%02x</td></tr>"
                   "<tr><th>BDC flags2</th><td>0x%02x</td></tr>"
                   "<tr><th>BDC data offset</th><td>%u</td></tr>"
                   "<tr><th>BDC data offset bytes</th><td>%u</td></tr>"
                   "<tr><th>BDC offset check</th><td>%s</td></tr>"
                   "<tr><th>Data ethertype probe</th><td>%s</td></tr>"
                   "<tr><th>Data ethertype result</th><td>%s</td></tr>"
                   "<tr><th>Data ethertype</th><td>0x%04x (%s)</td></tr>"
                   "<tr><th>Broadcom event probe</th><td>%s</td></tr>"
                   "<tr><th>Broadcom event result</th><td>%s</td></tr>"
                   "<tr><th>Broadcom event subtype</th><td>0x%04x</td></tr>"
                   "<tr><th>Broadcom event length</th><td>%u</td></tr>"
                   "<tr><th>Broadcom event version</th><td>%u</td></tr>"
                   "<tr><th>Broadcom event OUI</th><td>%02x:%02x:%02x</td></tr>"
                   "<tr><th>Broadcom event user subtype</th><td>0x%04x</td></tr>"
                   "<tr><th>Broadcom event message probe</th><td>%s</td></tr>"
                   "<tr><th>Broadcom event message result</th><td>%s</td></tr>"
                   "<tr><th>Join summary</th><td>%s</td></tr>"
                   "<tr><th>Join phase</th><td>%s</td></tr>"
                   "<tr><th>Join event span</th><td>%s</td></tr>"
                   "<tr><th>Broadcom events seen</th><td>%u</td></tr>"
                   "<tr><th>Broadcom first event</th><td>0x%08lx (%s)</td></tr>"
                   "<tr><th>Broadcom first status</th><td>0x%08lx (%s)</td></tr>"
                   "<tr><th>Broadcom first reason</th><td>0x%08lx (%s)</td></tr>"
                   "<tr><th>Broadcom event msg version</th><td>%u</td></tr>"
                   "<tr><th>Broadcom event msg flags</th><td>0x%04x</td></tr>"
                   "<tr><th>Broadcom event type</th><td>0x%08lx (%s)</td></tr>"
                   "<tr><th>Broadcom event status</th><td>0x%08lx (%s)</td></tr>"
                   "<tr><th>Broadcom event reason</th><td>0x%08lx (%s)</td></tr>"
                   "<tr><th>Broadcom event auth type</th><td>0x%08lx</td></tr>"
                   "<tr><th>Broadcom event data length</th><td>%lu</td></tr>"
                   "<tr><th>Broadcom event addr</th><td>%02x:%02x:%02x:%02x:%02x:%02x</td></tr>"
                   "<tr><th>Broadcom event ifname</th><td>%s%s</td></tr>"
                   "<tr><th>Broadcom event ifidx</th><td>%u</td></tr>"
                   "<tr><th>Broadcom event bsscfgidx</th><td>%u</td></tr>"
                   "<tr><th>Broadcom event payload bytes</th><td>%lu</td></tr>"
                   "<tr><th>Broadcom event payload fit</th><td>%s</td></tr>"
                   "<tr><th>Read abort</th><td>%s</td></tr>"
                   "<tr><th>Read abort result</th><td>%s</td></tr>"
                   "<tr><th>Backplane probe</th><td>%s</td></tr>"
                   "<tr><th>Chipcommon ID reg</th><td>0x%08lx</td></tr>"
                   "<tr><th>Chip ID</th><td>%u</td></tr>"
                   "<tr><th>Chip revision</th><td>%u</td></tr>"
                   "<tr><th>HTTP ready</th><td>%s</td></tr>"
                   "<tr><th>lwIP adapter</th><td>%s</td></tr>"
                   "<tr><th>lwIP core</th><td>%s</td></tr>"
                   "<tr><th>netif added</th><td>%s</td></tr>"
                   "<tr><th>lwIP timers</th><td>%s</td></tr>"
                   "<tr><th>Service calls</th><td>%lu</td></tr>"
                   "<tr><th>Link state</th><td>%s</td></tr>"
                   "<tr><th>Address ready</th><td>%s</td></tr>"
                   "<tr><th>DHCP started</th><td>%s</td></tr>"
                   "<tr><th>Firmware image</th><td>%s (%lu bytes)</td></tr>"
                   "<tr><th>NVRAM image</th><td>%s (%lu bytes)</td></tr>"
                   "<tr><th>Last error</th><td>%s</td></tr>"
                   "</table>",
                   (unsigned int)sdio_probe->sdpcm_bdc_version,
                   sdio_probe->sdpcm_bdc_header_decoded ? (sdio_probe->sdpcm_bdc_version_valid ? "version 2" : "unexpected version") : "not data",
                   (unsigned int)sdio_probe->sdpcm_bdc_flags,
                   (unsigned int)sdio_probe->sdpcm_bdc_priority,
                   (unsigned int)sdio_probe->sdpcm_bdc_flags2,
                   (unsigned int)sdio_probe->sdpcm_bdc_data_offset,
                   (unsigned int)sdio_probe->sdpcm_bdc_data_offset_bytes,
                   sdio_probe->sdpcm_bdc_header_decoded ? (sdio_probe->sdpcm_bdc_data_offset_sane ? "within frame" : "past frame") : "not data",
                   sdio_probe->sdpcm_data_ethertype_probe_attempted ? "yes" : "no",
                   !sdio_probe->sdpcm_data_ethertype_probe_attempted ? "not data" : (sdio_probe->sdpcm_data_ethertype_probe_success ? "ethernet header reached" : "frame too short or read failed"),
                   (unsigned int)sdio_probe->sdpcm_data_ethertype,
                   ethertype_name(sdio_probe->sdpcm_data_ethertype),
                   sdio_probe->sdpcm_brcm_event_probe_attempted ? "yes" : "no",
                   brcm_event_probe_result(sdio_probe),
                   (unsigned int)sdio_probe->sdpcm_brcm_event_subtype,
                   (unsigned int)sdio_probe->sdpcm_brcm_event_length,
                   (unsigned int)sdio_probe->sdpcm_brcm_event_version,
                   (unsigned int)sdio_probe->sdpcm_brcm_event_oui0,
                   (unsigned int)sdio_probe->sdpcm_brcm_event_oui1,
                   (unsigned int)sdio_probe->sdpcm_brcm_event_oui2,
                   (unsigned int)sdio_probe->sdpcm_brcm_event_usr_subtype,
                   sdio_probe->sdpcm_brcm_event_msg_probe_attempted ? "yes" : "no",
                   brcm_event_msg_result(sdio_probe),
                   join_probe_result(sdio_probe),
                   join_probe_phase(sdio_probe),
                   join_probe_span_result(sdio_probe),
                   (unsigned int)sdio_probe->sdpcm_brcm_event_count,
                   (unsigned long)sdio_probe->sdpcm_brcm_event_first_type,
                   sdio_probe->sdpcm_brcm_event_count == 0u ? "none observed" : brcm_event_name(sdio_probe->sdpcm_brcm_event_first_type),
                   (unsigned long)sdio_probe->sdpcm_brcm_event_first_status,
                   sdio_probe->sdpcm_brcm_event_count == 0u ? "no event status"
                      : brcm_event_status_name(sdio_probe->sdpcm_brcm_event_first_type,
                                               sdio_probe->sdpcm_brcm_event_first_status),
                   (unsigned long)sdio_probe->sdpcm_brcm_event_first_reason,
                   sdio_probe->sdpcm_brcm_event_count == 0u ? "no event reason"
                      : (sdio_probe->sdpcm_brcm_event_first_type == 16u
                         ? brcm_link_reason_name(sdio_probe->sdpcm_brcm_event_first_reason)
                         : "context-specific"),
                   (unsigned int)sdio_probe->sdpcm_brcm_event_msg_version,
                   (unsigned int)sdio_probe->sdpcm_brcm_event_msg_flags,
                   (unsigned long)sdio_probe->sdpcm_brcm_event_type,
                   brcm_event_name(sdio_probe->sdpcm_brcm_event_type),
                   (unsigned long)sdio_probe->sdpcm_brcm_event_status,
                   brcm_event_status_name(sdio_probe->sdpcm_brcm_event_type,
                                          sdio_probe->sdpcm_brcm_event_status),
                   (unsigned long)sdio_probe->sdpcm_brcm_event_reason,
                   sdio_probe->sdpcm_brcm_event_type == 16u
                      ? brcm_link_reason_name(sdio_probe->sdpcm_brcm_event_reason)
                      : "context-specific",
                   (unsigned long)sdio_probe->sdpcm_brcm_event_auth_type,
                   (unsigned long)sdio_probe->sdpcm_brcm_event_datalen,
                   (unsigned int)sdio_probe->sdpcm_brcm_event_addr[0],
                   (unsigned int)sdio_probe->sdpcm_brcm_event_addr[1],
                   (unsigned int)sdio_probe->sdpcm_brcm_event_addr[2],
                   (unsigned int)sdio_probe->sdpcm_brcm_event_addr[3],
                   (unsigned int)sdio_probe->sdpcm_brcm_event_addr[4],
                   (unsigned int)sdio_probe->sdpcm_brcm_event_addr[5],
                   sdio_probe->sdpcm_brcm_event_ifname,
                   sdio_probe->sdpcm_brcm_event_ifname_truncated ? "..." : "",
                   (unsigned int)sdio_probe->sdpcm_brcm_event_ifidx,
                   (unsigned int)sdio_probe->sdpcm_brcm_event_bsscfgidx,
                   (unsigned long)sdio_probe->sdpcm_brcm_event_payload_bytes_available,
                   sdio_probe->sdpcm_brcm_event_msg_probe_success ? (sdio_probe->sdpcm_brcm_event_msg_datalen_sane ? "fits frame" : "extends past frame") : "not decoded",
                   sdio_probe->frame_read_abort_attempted ? "yes" : "no",
                   sdio_probe->frame_read_abort_attempted ? (sdio_probe->frame_read_abort_success ? "fifo terminated" : "abort failed") : "not needed",
                   sdio_probe->backplane_probe_success ? "chipcommon read succeeded" : "not available",
                   (unsigned long)sdio_probe->chipcommon_id_register,
                   (unsigned int)sdio_probe->chip_id,
                   (unsigned int)sdio_probe->chip_revision,
                   status.can_start_http ? "yes" : "no",
                   lwip_context->prepared ? (lwip_context->use_dhcp ? "prepared for DHCP" : "prepared for static IPv4") : "not prepared",
                   lwip_context->initialized ? (lwip_context->static_configured ? "initialized with static config" : (lwip_context->dhcp_started ? "initialized with DHCP client" : "initialized")) : "not initialized",
                   lwip_context->netif_added ? "yes" : "no",
                   lwip_context->timers_running ? "registered in Pi1MHz poll loop" : "not running",
                   (unsigned long)lwip_context->service_calls,
                   lwip_context->link_up ? "up" : "down",
                   lwip_context->address_ready ? "yes" : "no",
                   lwip_context->dhcp_started ? "yes" : "no",
                   g_cyw43_firmware_path,
                   (unsigned long)g_cyw43_firmware_length,
                   g_cyw43_nvram_path,
                   (unsigned long)g_cyw43_nvram_length,
                   status.last_error[0] != '\0' ? status.last_error : "none");
   response_end_html(response);
}

static void render_file_listing(webserver_response_t *response)
{
   FRESULT fr;
   DIR dir;
   FILINFO info;
   unsigned int shown = 0;

   response_begin_html(response, "Files");
   response_append(response,
                   "<p>This page reuses the existing FAT filesystem layer. Upload and download handlers still need the TCP body parser.</p>"
                   "<p>Planned endpoints: <code>POST /files/upload</code>, <code>GET /files/download?name=...</code>.</p>");

   if (!filesystemMount()) {
      response_append(response, "<p>Filesystem mount failed.</p>");
      response_end_html(response);
      return;
   }

   fr = f_opendir(&dir, "/");
   if (fr != FR_OK) {
      response_append(response, "<p>Could not open root directory. FatFs error %u.</p>", (unsigned int)fr);
      response_end_html(response);
      return;
   }

   response_append(response, "<table><tr><th>Name</th><th>Type</th><th>Size</th></tr>");
   for (;;) {
      fr = f_readdir(&dir, &info);
      if (fr != FR_OK || info.fname[0] == '\0' || shown >= 24)
         break;

      response_append(response,
                      "<tr><td>%s</td><td>%s</td><td>%lu</td></tr>",
                      info.fname,
                      (info.fattrib & AM_DIR) != 0 ? "directory" : "file",
                      (unsigned long)info.fsize);
      ++shown;
   }
   response_append(response, "</table>");
   f_closedir(&dir);
   response_end_html(response);
}

static void render_framebuffer_page(webserver_response_t *response)
{
   framebuffer_export_info_t info;
   bool have_info = framebuffer_export_get_info(&info);
   screen_mode_t *mode = fb_get_current_screen_mode();

   response_begin_html(response, "Framebuffer");
   response_append(response,
                   "<p>This page is ready to sit on top of the existing framebuffer subsystem.</p>"
                   "<table>"
                   "<tr><th>Framebuffer address</th><td>0x%08lx</td></tr>"
                   "<tr><th>Mode number</th><td>%d</td></tr>"
                   "<tr><th>Dimensions</th><td>%d x %d</td></tr>"
                   "<tr><th>Pitch</th><td>%lu bytes</td></tr>"
                   "<tr><th>Bits per pixel</th><td>%lu</td></tr>"
                   "<tr><th>Snapshot size</th><td>%lu bytes</td></tr>"
                   "<tr><th>Colours</th><td>%u</td></tr>"
                   "<tr><th>Raw snapshot</th><td><a href=\"/framebuffer/save.raw\">save to Pi1MHz/framebuffer.raw</a></td></tr>"
                   "<tr><th>PNG export</th><td>planned after encoder integration</td></tr>"
                   "</table>"
                   "<p>PNG export still needs a snapshot-to-PNG encoder. A raw dump path is available now for validation.</p>",
                   have_info ? (unsigned long)info.address : (unsigned long)fb_get_address(),
                   mode != NULL ? mode->mode_num : -1,
                   mode != NULL ? mode->width : 0,
                   mode != NULL ? mode->height : 0,
                   have_info ? (unsigned long)info.pitch : 0ul,
                   have_info ? (unsigned long)info.bits_per_pixel : 0ul,
                   have_info ? (unsigned long)info.size : 0ul,
                   mode != NULL ? mode->ncolour : 0u);
   response_end_html(response);
}

static void render_framebuffer_save_raw_page(webserver_response_t *response)
{
   framebuffer_export_info_t info;
   bool saved;

   response_begin_html(response, "Framebuffer Raw Snapshot");
   saved = framebuffer_export_save_raw("Pi1MHz/framebuffer.raw", &info);
   if (saved) {
      response_append(response,
                      "<p>Saved raw framebuffer snapshot to <code>Pi1MHz/framebuffer.raw</code>.</p>"
                      "<table>"
                      "<tr><th>Size</th><td>%lu bytes</td></tr>"
                      "<tr><th>Dimensions</th><td>%lu x %lu</td></tr>"
                      "<tr><th>Bits per pixel</th><td>%lu</td></tr>"
                      "</table>",
                      (unsigned long)info.size,
                      (unsigned long)info.width,
                      (unsigned long)info.height,
                      (unsigned long)info.bits_per_pixel);
   } else {
      response_append(response,
                      "<p>Could not save framebuffer snapshot. Check that the framebuffer is active and the filesystem is writable.</p>");
   }
   response_end_html(response);
}

static void render_not_implemented(webserver_response_t *response, const char *title, const char *detail)
{
   response_begin_html(response, title);
   response->status_code = 501;
   response_append(response, "<p>%s</p>", detail);
   response_end_html(response);
}

void webserver_init(void)
{
   const wifi_config_t *config = wifi_get_config();
   struct tcp_pcb *listener;

   g_webserver_ready = false;
   webserver_set_error(NULL);

   if (g_webserver_listener != NULL) {
      tcp_arg(g_webserver_listener, NULL);
      tcp_accept(g_webserver_listener, NULL);
      tcp_close(g_webserver_listener);
      g_webserver_listener = NULL;
   }

   listener = tcp_new_ip_type(IPADDR_TYPE_V4);
   if (listener == NULL) {
      webserver_set_error("lwIP could not allocate a TCP listener PCB");
      return;
   }

   if (tcp_bind(listener, IP_ADDR_ANY, config->http_port) != ERR_OK) {
      tcp_close(listener);
      webserver_set_error("lwIP could not bind the HTTP listener port");
      return;
   }

   g_webserver_listener = tcp_listen_with_backlog(listener, 2u);
   if (g_webserver_listener == NULL) {
      tcp_close(listener);
      webserver_set_error("lwIP could not switch the HTTP PCB into listen mode");
      return;
   }

   tcp_arg(g_webserver_listener, NULL);
   tcp_accept(g_webserver_listener, webserver_accept);
   g_webserver_ready = true;
   wifi_note_http_ready();
}

bool webserver_is_ready(void)
{
   return g_webserver_ready;
}

const char *webserver_last_error(void)
{
   return g_webserver_error;
}

bool webserver_render(const webserver_request_t *request, webserver_response_t *response)
{
   if (request == NULL || response == NULL || request->path == NULL)
      return false;

   if (strcmp(request->path, "/") == 0) {
      render_home_page(response);
      return true;
   }

   if (strcmp(request->path, "/status") == 0) {
      render_status_page(response);
      return true;
   }

   if (strcmp(request->path, "/files") == 0) {
      render_file_listing(response);
      return true;
   }

   if (strcmp(request->path, "/framebuffer") == 0) {
      render_framebuffer_page(response);
      return true;
   }

    if (strcmp(request->path, "/framebuffer/save.raw") == 0) {
      render_framebuffer_save_raw_page(response);
      return true;
   }

   if (strcmp(request->path, "/files/upload") == 0) {
      render_not_implemented(response, "Upload", "Multipart upload handling is not wired into the network stack yet.");
      return true;
   }

   if (strcmp(request->path, "/framebuffer.png") == 0) {
      render_not_implemented(response, "Framebuffer PNG", "PNG encoding is not implemented yet.");
      return true;
   }

   response_begin_html(response, "Not Found");
   response->status_code = 404;
   response_append(response, "<p>No route for %s</p>", request->path);
   response_end_html(response);
   return true;
}