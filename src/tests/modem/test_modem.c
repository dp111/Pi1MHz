/* test_modem.c - host tests for the serial modem (serial_modem.c).

   The Beeb side is a pair of byte queues standing in for serial_redirect's
   API; the network is a fake net_capi_* with one connection whose opening,
   data and closing the test controls.  Time is whatever the test passes to
   modem_poll. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "net_service.h"
#include "serial_modem.h"
#include "serial_redirect.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
   printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the Beeb's side of the redirect ---------------------------------- */
static uint8_t to_pi[4096];   static size_t to_pi_head, to_pi_tail;
static char    to_beeb[8192]; static size_t to_beeb_len;
static size_t  beeb_room = 1024;

size_t serial_redirect_read(uint8_t *dst, size_t max)
{
   size_t n = 0;
   while (n < max && to_pi_head < to_pi_tail)
      dst[n++] = to_pi[to_pi_head++];
   return n;
}
/* With fifo_cap set, the redirect's buffer is modelled as a FIFO of that size
   which the Beeb drains drain_per_ms bytes a millisecond; otherwise the room
   is beeb_room on every call. */
static size_t  fifo_cap, fifo_level, drain_per_ms;
size_t serial_redirect_room(void) { return fifo_cap ? fifo_cap - fifo_level : beeb_room; }
size_t serial_redirect_write(const uint8_t *src, size_t len)
{
   size_t room = serial_redirect_room();
   if (len > room) len = room;
   memcpy(to_beeb + to_beeb_len, src, len);
   to_beeb_len += len;
   if (fifo_cap) fifo_level += len;
   to_beeb[to_beeb_len] = '\0';
   return len;
}

/* ---- config ------------------------------------------------------------ */
const char *config_get(const char *key)
{
   if (strcmp(key, "modem_phone_1") == 0) return "bbs.test:6502";
   return NULL;
}

/* ---- the network ------------------------------------------------------- */
static bool    net_on = true;
static int     net_taken = -1;          /* the one handle, or -1 */
static int     open_calls, open_delay;  /* NET_PENDING this many times */
static uint8_t open_result = NET_OK;
static char    opened_url[256];
static bool    peer_closed;
static uint8_t net_rx[4096];  static size_t net_rx_head, net_rx_len;
static uint8_t net_tx[4096];  static size_t net_tx_len;
static uint32_t write_accept = 0xFFFFFFFFu;

bool net_capi_enabled(void) { return net_on; }
int net_capi_alloc(void) { return -1; }   /* FujiNet's pool: not the modem's */
int net_capi_alloc_modem(void)
{
   if (net_taken >= 0) return -1;
   net_taken = NET_BEEB_HANDLES + NET_CAPI_HANDLES;
   open_calls = 0;
   return net_taken;
}
uint8_t net_capi_open(int h, const char *url, uint8_t mode, const net_http_opts_t *o)
{
   (void)mode; (void)o;
   if (h != net_taken) return NET_ERR_PARAM;
   snprintf(opened_url, sizeof opened_url, "%s", url);
   if (open_calls++ < open_delay) return NET_PENDING;
   return open_result;
}
uint8_t net_capi_read(int h, uint8_t *dst, uint32_t max, uint32_t *got)
{
   *got = 0;
   if (h != net_taken) return NET_ERR_PARAM;
   uint32_t n = 0;
   while (n < max && net_rx_head < net_rx_len)
      dst[n++] = net_rx[net_rx_head++];
   *got = n;
   if (n == 0 && peer_closed) return NET_EOF;
   return NET_OK;
}
uint8_t net_capi_write(int h, const uint8_t *src, uint32_t len, uint32_t *done)
{
   if (h != net_taken) return NET_ERR_NOTOPEN;
   if (len > write_accept) len = write_accept;
   memcpy(net_tx + net_tx_len, src, len);
   net_tx_len += len;
   *done = len;
   return NET_OK;
}
void net_capi_close(int h) { if (h == net_taken) net_taken = -1; }
uint16_t net_capi_http_code(int h) { (void)h; return 0; }

/* ---- helpers ------------------------------------------------------------ */
static uint32_t now;

static void reset_all(void)
{
   to_pi_head = to_pi_tail = 0;
   to_beeb_len = 0; to_beeb[0] = '\0';
   beeb_room = 1024;
   fifo_cap = fifo_level = drain_per_ms = 0;
   net_on = true; net_taken = -1; open_delay = 0; open_result = NET_OK;
   opened_url[0] = '\0'; peer_closed = false;
   net_rx_head = net_rx_len = 0; net_tx_len = 0; write_accept = 0xFFFFFFFFu;
   now = 1000000u;
   modem_reset();
}
static void type(const char *s)
{
   size_t n = strlen(s);
   memcpy(to_pi + to_pi_tail, s, n);
   to_pi_tail += n;
}
static void poll_for(uint32_t us)          /* run the modem for us, in 1 ms steps */
{
   uint32_t end = now + us;
   do {
      modem_poll(now);
      now += 1000u;
      fifo_level -= fifo_level < drain_per_ms ? fifo_level : drain_per_ms;
   } while ((int32_t)(end - now) > 0);
}
static void clear_out(void) { to_beeb_len = 0; to_beeb[0] = '\0'; }
static void peer_sends(const char *s)
{
   size_t n = strlen(s);
   memcpy(net_rx + net_rx_len, s, n);
   net_rx_len += n;
}
static bool out_has(const char *s) { return strstr(to_beeb, s) != NULL; }

/* ---- tests -------------------------------------------------------------- */
static void test_commands(void)
{
   printf("== commands ==\n");
   reset_all();
   type("AT\r"); poll_for(2000);
   CHECK(strcmp(to_beeb, "AT\r\r\nOK\r\n") == 0, "AT echoes and answers OK: [%s]", to_beeb);

   clear_out(); type("at\r"); poll_for(2000);
   CHECK(out_has("OK"), "lower case works");

   clear_out(); type("hello\r"); poll_for(2000);
   CHECK(strcmp(to_beeb, "hello\r") == 0, "a non-AT line is echoed and ignored: [%s]", to_beeb);

   clear_out(); type("ATE0\r"); poll_for(2000);
   clear_out(); type("AT\r"); poll_for(2000);
   CHECK(strcmp(to_beeb, "\r\nOK\r\n") == 0, "E0 stops the echo: [%s]", to_beeb);

   clear_out(); type("ATV0\r"); poll_for(2000);
   CHECK(strcmp(to_beeb, "0\r") == 0, "V0 gives numeric results: [%s]", to_beeb);
   clear_out(); type("ATJ\r"); poll_for(2000);
   CHECK(strcmp(to_beeb, "4\r") == 0, "unknown command: ERROR (4): [%s]", to_beeb);

   clear_out(); type("ATQ1\r"); poll_for(2000);
   clear_out(); type("AT\r"); poll_for(2000);
   CHECK(to_beeb_len == 0, "Q1 silences results");

   clear_out(); type("ATQ0Z\r"); poll_for(2000);
   CHECK(strcmp(to_beeb, "\r\nOK\r\n") == 0, "ATZ (typed with echo off): [%s]", to_beeb);
   clear_out(); type("AT\r"); poll_for(2000);
   CHECK(strcmp(to_beeb, "AT\r\r\nOK\r\n") == 0, "ATZ restored echo and verbose: [%s]", to_beeb);

   clear_out(); type("ATS7=30S7?\r"); poll_for(2000);
   CHECK(out_has("\r\n030") && out_has("OK"), "S register set and read: [%s]", to_beeb);
   clear_out(); type("ATS99=1\r"); poll_for(2000);
   CHECK(out_has("ERROR"), "S register out of range: ERROR");
   clear_out(); type("AT&F&C1&D2&K0X4M0L0\r"); poll_for(2000);
   CHECK(out_has("OK"), "&F and the ignored ones: [%s]", to_beeb);

   clear_out(); type("AT\x08I\r"); poll_for(2000);   /* "AT", backspace T, "I" -> "AI" */
   CHECK(!out_has("OK") && !out_has("ERROR"), "backspace edits the line: [%s]", to_beeb);

   clear_out(); type("ATI\r"); poll_for(2000);
   CHECK(out_has("Pi1MHz modem") && out_has("OK"), "ATI identifies");
   clear_out(); type("A/"); poll_for(2000);
   CHECK(out_has("Pi1MHz modem"), "A/ repeats the last command: [%s]", to_beeb);
}

static void test_dial_forms(void)
{
   printf("== dial strings ==\n");
   static const struct { const char *cmd, *url; } t[] = {
      { "ATDTbbs.example.com:6502\r", "tcp://bbs.example.com:6502" },
      { "ATD bbs.example.com\r",      "tcp://bbs.example.com:23" },
      { "ATDP192168001005\r",         "tcp://192.168.1.5:23" },
      { "ATDT 192-168-001-005\r",     "tcp://192.168.1.5:23" },
      { "ATDT1\r",                    "tcp://bbs.test:6502" },
      { "ATDTtelehack.com\r",         "tcp://telehack.com:23" },
      { "ATDTpost.test:1\r",          "tcp://post.test:1" },
      { "ATDpost.test:1\r",           "tcp://ost.test:1" },   /* P = pulse, as Hayes */
      { "ATD T tt.test\r",            "tcp://tt.test:23" },
   };
   for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
      reset_all();
      type(t[i].cmd); poll_for(3000);
      CHECK(strcmp(opened_url, t[i].url) == 0, "%s-> %s (got %s)", t[i].cmd, t[i].url, opened_url);
      CHECK(out_has("CONNECT"), "%s connects", t[i].cmd);
   }

   reset_all();
   type("ATNET1\r"); poll_for(2000);
   type("ATDbbs.test\r"); poll_for(3000);
   CHECK(strcmp(opened_url, "telnet://bbs.test:23") == 0, "NET1 dials telnet://: %s", opened_url);

   reset_all();
   type("ATDT2\r"); poll_for(3000);
   CHECK(out_has("NO CARRIER") && net_taken < 0, "empty phonebook slot: NO CARRIER");
   reset_all();
   type("ATDT999999999999\r"); poll_for(3000);
   CHECK(out_has("NO CARRIER"), "12 digits that are no address: NO CARRIER");
   reset_all();
   type("ATD\r"); poll_for(3000);
   CHECK(out_has("ERROR"), "nothing to dial: ERROR");
   reset_all();
   net_on = false;
   type("ATDhost:1\r"); poll_for(3000);
   CHECK(out_has("NO DIALTONE") && net_taken < 0, "net_enable=0: NO DIALTONE");
}

static void test_dial_outcomes(void)
{
   printf("== dialling ==\n");
   reset_all();
   open_delay = 50;
   type("ATDhost:1\r"); poll_for(20000);
   CHECK(!out_has("CONNECT") && !out_has("NO CARRIER"), "still dialling");
   type("\n"); poll_for(2000);
   CHECK(!out_has("NO CARRIER"), "the LF of CR LF does not abandon the call");
   poll_for(60000);
   CHECK(out_has("\r\nCONNECT\r\n"), "connects when the open completes");

   reset_all();
   open_delay = 1000000;
   type("ATDhost:1\r"); poll_for(5000);
   type("x"); poll_for(2000);
   CHECK(out_has("NO CARRIER") && net_taken < 0, "a key abandons the call");

   reset_all();
   open_delay = 1000000;
   type("ATS7=2\r"); poll_for(2000);
   type("ATDhost:1\r"); poll_for(2500000);
   CHECK(out_has("NO ANSWER") && net_taken < 0, "S7 times the dial out");

   reset_all();
   open_result = NET_ERR_CONN;
   type("ATDhost:1\r"); poll_for(3000);
   CHECK(out_has("NO CARRIER") && net_taken < 0, "refused: NO CARRIER");
   reset_all();
   open_result = NET_ERR_DNS;
   type("ATDnowhere:1\r"); poll_for(3000);
   CHECK(out_has("NO ANSWER") && net_taken < 0, "no such host: NO ANSWER");
}

static void connect_now(void)
{
   reset_all();
   type("ATDhost:1\r"); poll_for(3000);
   clear_out();
}

static void test_online(void)
{
   printf("== online ==\n");
   connect_now();
   type("hello"); poll_for(2000);
   CHECK(net_tx_len == 5 && memcmp(net_tx, "hello", 5) == 0, "Beeb -> network");
   CHECK(to_beeb_len == 0, "no echo online");
   peer_sends("welcome\xff");
   poll_for(2000);
   CHECK(to_beeb_len == 8 && memcmp(to_beeb, "welcome\xff", 8) == 0, "network -> Beeb, 8-bit clean");

   /* The Beeb's buffer is full: nothing is taken from the network. */
   connect_now();
   beeb_room = 0;
   peer_sends("abc"); poll_for(2000);
   CHECK(to_beeb_len == 0 && net_rx_head == 0, "no room: the data waits");
   beeb_room = 24; poll_for(1000);
   CHECK(to_beeb_len == 0 && net_rx_head == 0, "room only for a result: the data waits");
   beeb_room = 26; poll_for(1000);
   CHECK(to_beeb_len == 2, "room for 2 more: 2 delivered");

   /* A slow network: what it has not taken yet is kept, in order. */
   connect_now();
   write_accept = 3;
   type("abcdefgh"); poll_for(5000);
   CHECK(net_tx_len == 8 && memcmp(net_tx, "abcdefgh", 8) == 0, "partial writes keep order");

   connect_now();
   peer_closed = true; poll_for(2000);
   CHECK(out_has("NO CARRIER") && net_taken < 0, "peer closes: NO CARRIER");
   clear_out(); type("AT\r"); poll_for(2000);
   CHECK(out_has("OK"), "back in command mode");
}

/* The network fills the Beeb's buffer faster than the Beeb empties it: a
   result written then must still arrive whole, not cut to the room left. */
static void test_result_fits(void)
{
   printf("== results are never cut short ==\n");
   static char burst[4001];

   connect_now();
   fifo_cap = 1024; drain_per_ms = 1;
   memset(burst, 'x', 3000); burst[3000] = '\0';
   peer_sends(burst); peer_closed = true;
   poll_for(5000000);
   CHECK(to_beeb_len == 3000 + 14 && memcmp(to_beeb + 2999, "x\r\nNO CARRIER\r\n", 15) == 0,
         "burst then hang-up: every byte, then all of NO CARRIER (%u bytes, ends '%s')",
         (unsigned)to_beeb_len, to_beeb_len >= 14 ? to_beeb + to_beeb_len - 14 : to_beeb);
   CHECK(net_taken < 0, "and the line is down");

   connect_now();
   fifo_cap = 1024; drain_per_ms = 1;
   poll_for(1100000);                   /* guard before */
   memset(burst, 'y', 4000); burst[4000] = '\0';
   peer_sends(burst);
   type("+++"); poll_for(1100000);      /* guard after, the buffer kept full */
   poll_for(3000000);                   /* let the Beeb drain it */
   CHECK(out_has("y\r\nOK\r\n"), "+++ with the buffer full: all of OK");
   CHECK(net_taken >= 0, "the line stays up");
}

static void test_escape(void)
{
   printf("== +++ escape ==\n");
   connect_now();
   poll_for(1100000);                   /* guard before */
   type("+++"); poll_for(1100000);      /* guard after */
   CHECK(out_has("\r\nOK\r\n"), "+++ with guard time: OK");
   CHECK(net_taken >= 0, "the line stays up");
   CHECK(net_tx_len == 3, "the +++ went to the network too");
   clear_out(); type("ATO\r"); poll_for(2000);
   CHECK(out_has("CONNECT"), "ATO goes back online");
   type("more"); poll_for(2000);
   CHECK(net_tx_len == 7, "online again");

   poll_for(1100000); type("+++"); poll_for(1100000);
   clear_out(); type("ATH\r"); poll_for(2000);
   CHECK(out_has("OK") && net_taken < 0, "ATH hangs up");
   clear_out(); type("ATO\r"); poll_for(2000);
   CHECK(out_has("NO CARRIER"), "ATO with no line: NO CARRIER");

   connect_now();
   type("a+++"); poll_for(1100000);     /* no guard before */
   CHECK(!out_has("OK"), "+++ right after data stays data");
   poll_for(1100000); type("++"); poll_for(500000); type("+x"); poll_for(1100000);
   CHECK(!out_has("OK"), "+++ followed by data stays data");

   connect_now();
   poll_for(1100000); type("+++"); poll_for(1100000);
   clear_out(); peer_closed = true; poll_for(2000);
   CHECK(out_has("NO CARRIER"), "the line drops while in command mode: NO CARRIER");

   reset_all();
   type("ATS2=200\r"); poll_for(2000);  /* S2 > 127 disables the escape */
   type("ATDhost:1\r"); poll_for(3000);
   clear_out();
   poll_for(1100000); type("+++"); poll_for(1100000);
   CHECK(!out_has("OK"), "S2=200: +++ is only data");

   /* A BBC reset hangs up. */
   connect_now();
   modem_reset();
   CHECK(net_taken < 0, "reset hangs up");
}

/* Bytes that arrive in the same read as the command that changes state
   belong to the new state, as they would on a modem fed byte by byte.  After
   ATO they are data for the line.  After ATD they are keys typed while it
   dials, and a key abandons the call (V.250 6.3.1) - except the LF of a
   CR LF line ending. */
static void test_typeahead(void)
{
   printf("== bytes after a state change ==\n");
   connect_now();
   poll_for(1100000); type("+++"); poll_for(1100000);
   net_tx_len = 0;
   clear_out(); type("ATO\rabc"); poll_for(2000);
   CHECK(out_has("CONNECT"), "ATO goes back online");
   CHECK(net_tx_len == 3 && memcmp(net_tx, "abc", 3) == 0,
         "data after ATO\\r in the same read reaches the line (%u bytes)", (unsigned)net_tx_len);

   reset_all();
   open_delay = 1000000;
   type("ATDhost:1\rx"); poll_for(5000);
   CHECK(out_has("NO CARRIER") && net_taken < 0,
         "a key in the same read as ATD...\\r abandons the call: [%s]", to_beeb);

   reset_all();
   open_delay = 20;
   type("ATDhost:1\r\n"); poll_for(60000);
   CHECK(out_has("\r\nCONNECT\r\n"), "CR LF after ATD: the LF does not abandon the call");
   type("hi"); poll_for(2000);
   CHECK(net_tx_len == 2 && memcmp(net_tx, "hi", 2) == 0, "and is not sent down the line");

   reset_all();
   type("ATE0\rATV0\rAT\r"); poll_for(2000);
   CHECK(strcmp(to_beeb, "ATE0\r\r\nOK\r\n0\r0\r") == 0,
         "several commands in one read each run: [%s]", to_beeb);
}

int main(void)
{
   test_commands();
   test_dial_forms();
   test_dial_outcomes();
   test_online();
   test_escape();
   test_result_fits();
   test_typeahead();
   printf("%d checks, %d failures\n", checks, failures);
   return failures ? 1 : 0;
}
