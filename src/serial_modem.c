/*
  serial_modem.c - a Hayes-style modem on the redirected RS423 port

  The Beeb's comms software talks AT commands to what it thinks is a modem;
  ATD opens a TCP connection through net_service's C API instead of
  dialling.  Byte transport is serial_redirect.c; this file is the modem's
  state machine, and touches nothing else, so the host tests
  (src/tests/modem) drive it with fakes for both sides.

     command --ATD--> dialling --connected--> online
        ^                |                      |  +++ (guard time either
        |           NO CARRIER                  |       side) -> command,
        +------------<---+----- ATH / peer FIN -+       still connected
                                                 <- ATO

  Dial strings (ATD, ATDT and ATDP are the same):
     host:port           ATDTbbs.example.com:6502
     host                port 23
     12 digits           ATDT192168001005 -> 192.168.1.5:23, for diallers
                         that only accept digits
     1-2 digits          phonebook slot n, "modem_phone_n=host:port" in
                         Pi1MHz.cfg
  ATNET1 dials telnet:// (IAC handling in net_service) instead of raw TCP.

  Commands: A/ AT Z &F E0/1 V0/1 Q0/1 H O I D Sn=v Sn? NET0/1, and &C &D &K
  &W X M L accepted and ignored.  S2 is the escape character, S7 the dial
  timeout in seconds, S12 the escape guard time in 1/50 s.  Inbound calls
  (RING, ATA, S0) are not implemented: the C API has no listen.

  Needs net_enable=1; without it ATD answers NO DIALTONE.
*/
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "net_service.h"
#include "serial_modem.h"
#include "serial_redirect.h"

enum { M_COMMAND, M_DIALLING, M_ONLINE };

enum {   /* result codes, numeric = index */
   R_OK = 0, R_CONNECT = 1, R_RING = 2, R_NO_CARRIER = 3, R_ERROR = 4,
   R_NO_DIALTONE = 6, R_BUSY = 7, R_NO_ANSWER = 8
};
static const char *const result_text[] = {
   "OK", "CONNECT", "RING", "NO CARRIER", "ERROR", "", "NO DIALTONE", "BUSY",
   "NO ANSWER"
};

#define S_REGS      13u
#define LINE_MAX    80u
#define URL_MAX     160u             /* "telnet://" + a long host + ":65535" */
#define NETBUF_SIZE 256u
/* The longest result, "\r\nNO DIALTONE\r\n" with room to spare.  Data from the
   network leaves this much of the Beeb's buffer free, so a result written
   while online - NO CARRIER, or OK after +++ - arrives whole. */
#define RESULT_ROOM 24u

static uint8_t  m_state;
static bool     m_connected;         /* a line is up (online, or command mode during a call) */
static bool     m_have_net;
static int      m_net;
static char     m_url[URL_MAX];
static uint32_t m_dial_us;
static uint8_t  m_net_err;           /* the last call's net_service error, for /status */

static bool     m_echo, m_verbose, m_quiet, m_telnet;
static uint8_t  m_s[S_REGS];

static char     m_line[LINE_MAX + 1u];
static unsigned m_len;
static char     m_last[LINE_MAX + 1u];   /* for A/ */

static uint32_t m_byte_us;           /* last byte from the Beeb while online */
static uint32_t m_plus_us;
static uint8_t  m_plus;              /* escape characters seen in a row */

static uint8_t  m_out[NETBUF_SIZE];  /* Beeb -> network, not yet taken */
static unsigned m_out_len;

static void defaults(void)
{
   m_echo = true;
   m_verbose = true;
   m_quiet = false;
   m_telnet = false;
   memset(m_s, 0, sizeof m_s);
   m_s[2] = '+';
   m_s[3] = '\r';
   m_s[4] = '\n';
   m_s[5] = 8;
   m_s[7] = 50;
   m_s[12] = 50;
}

static void put(const char *s)
{
   (void)serial_redirect_write((const uint8_t *)s, strlen(s));
}

static void result(int code)
{
   char buf[RESULT_ROOM];
   if (m_quiet)
      return;
   if (m_verbose)
      snprintf(buf, sizeof buf, "\r\n%s\r\n", result_text[code]);
   else
      snprintf(buf, sizeof buf, "%d\r", code);
   put(buf);
}

static void hang_up(void)
{
   if (m_have_net)
      net_capi_close(m_net);
   m_have_net = false;
   m_connected = false;
   m_out_len = 0u;
   m_plus = 0u;
   m_state = M_COMMAND;
}

void modem_reset(void)
{
   hang_up();
   defaults();
   m_len = 0u;
   m_last[0] = '\0';
}

/* "host[:port]" -> m_url; false if it will not fit. */
static bool make_url(const char *target)
{
   const char *colon = strrchr(target, ':');
   int n = snprintf(m_url, sizeof m_url, colon ? "%s://%s" : "%s://%s:23",
                    m_telnet ? "telnet" : "tcp", target);
   return n > 0 && (size_t)n < sizeof m_url;
}

/* The dial string after D -> m_url, or the result code to give instead. */
static int parse_dial(const char *p)
{
   char target[URL_MAX];
   char digits[24];
   unsigned t = 0u, d = 0u;
   bool numeric = true;

   /* One T or P (tone/pulse) at most: a host name can start with either,
      so "ATDTtelehack.com" must keep its t - and "ATDpost" is pulse-dialling
      "ost", as on a real modem, hence ATDT in front of such names. */
   while (*p == ' ')
      p++;
   if (toupper((unsigned char)*p) == 'T' || toupper((unsigned char)*p) == 'P')
      p++;
   for (; *p; p++) {
      if (*p == ' ')
         continue;
      if (t + 1u >= sizeof target)
         return R_ERROR;
      target[t++] = *p;
      if (isdigit((unsigned char)*p)) {
         if (d + 1u < sizeof digits)
            digits[d++] = *p;
      } else if (!strchr("-(),", *p)) {
         numeric = false;
      }
   }
   target[t] = '\0';
   digits[d] = '\0';
   if (t == 0u)
      return R_ERROR;

   if (numeric) {
      if (d == 12u) {                               /* aaabbbcccddd */
         unsigned o[4];
         for (unsigned i = 0u; i < 4u; i++) {
            o[i] = (unsigned)(digits[i * 3u] - '0') * 100u
                 + (unsigned)(digits[i * 3u + 1u] - '0') * 10u
                 + (unsigned)(digits[i * 3u + 2u] - '0');
            if (o[i] > 255u)
               return R_NO_CARRIER;
         }
         snprintf(target, sizeof target, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
      } else if (d >= 1u && d <= 2u) {              /* phonebook */
         char key[16];
         snprintf(key, sizeof key, "modem_phone_%.2s", digits);
         const char *v = config_get(key);
         if (v == NULL || v[0] == '\0' || strlen(v) >= sizeof target)
            return R_NO_CARRIER;
         snprintf(target, sizeof target, "%s", v);
      } else {
         return R_NO_CARRIER;
      }
   }
   return make_url(target) ? R_OK : R_ERROR;
}

static int start_dial(const char *p)
{
   m_net_err = 0u;
   int r = parse_dial(p);
   if (r != R_OK)
      return r;
   if (!net_capi_enabled())
      return R_NO_DIALTONE;
   m_net = net_capi_alloc_modem();
   if (m_net < 0)
      return R_BUSY;
   m_have_net = true;
   m_state = M_DIALLING;
   return -1;                                       /* result comes later */
}

static unsigned number(const char **pp)
{
   unsigned v = 0u;
   while (isdigit((unsigned char)**pp)) {
      v = v * 10u + (unsigned)(**pp - '0');
      if (v > 999u)
         v = 999u;
      (*pp)++;
   }
   return v;
}

/* Run one command line; the result code goes to the Beeb, except after a
   dial or ATO, which answer later or with CONNECT. */
static void execute(const char *line, uint32_t now_us)
{
   const char *p = line;
   while (*p == ' ')
      p++;
   if (toupper((unsigned char)p[0]) != 'A' || toupper((unsigned char)p[1]) != 'T')
      return;                                       /* not for us: ignored, as a modem does */
   snprintf(m_last, sizeof m_last, "%s", p);
   p += 2;

   while (*p) {
      char c = (char)toupper((unsigned char)*p++);
      unsigned n;
      switch (c) {
      case ' ':
         break;
      case 'Z':
         hang_up();
         defaults();
         break;
      case 'E': m_echo    = number(&p) != 0u; break;
      case 'V': m_verbose = number(&p) != 0u; break;
      case 'Q': m_quiet   = number(&p) != 0u; break;
      case 'H':
         (void)number(&p);
         hang_up();
         break;
      case 'O':
         (void)number(&p);
         if (!m_connected) { result(R_NO_CARRIER); return; }
         m_state = M_ONLINE;
         m_byte_us = now_us;
         result(R_CONNECT);
         return;
      case 'I':
         (void)number(&p);
         put("\r\nPi1MHz modem");
         break;
      case 'D': {
         if (m_connected) { result(R_ERROR); return; }
         int r = start_dial(p);
         if (r >= 0) { result(r); return; }
         m_dial_us = now_us;
         return;
      }
      case 'S':
         n = number(&p);
         if (n >= S_REGS) { result(R_ERROR); return; }
         if (*p == '=') {
            p++;
            unsigned v = number(&p);
            if (v > 255u) { result(R_ERROR); return; }
            m_s[n] = (uint8_t)v;
         } else if (*p == '?') {
            char buf[12];
            p++;
            snprintf(buf, sizeof buf, "\r\n%03u", (unsigned)m_s[n]);
            put(buf);
         } else {
            result(R_ERROR);
            return;
         }
         break;
      case '&':
         c = (char)toupper((unsigned char)*p);
         if (c == '\0') { result(R_ERROR); return; }
         p++;
         if (c == 'F')
            defaults();
         else if (!strchr("CDKW", c)) { result(R_ERROR); return; }
         (void)number(&p);
         break;
      case 'N':
         if (toupper((unsigned char)p[0]) != 'E' || toupper((unsigned char)p[1]) != 'T') {
            result(R_ERROR);
            return;
         }
         p += 2;
         m_telnet = number(&p) != 0u;
         break;
      case 'X': case 'M': case 'L':
         (void)number(&p);
         break;
      default:
         result(R_ERROR);
         return;
      }
   }
   result(R_OK);
}

/* Command mode: build a line from the Beeb's bytes, echoing if E1. */
static void command_byte(uint8_t c, uint32_t now_us)
{
   if (m_echo)
      (void)serial_redirect_write(&c, 1u);
   if (c == m_s[3]) {
      m_line[m_len] = '\0';
      m_len = 0u;
      execute(m_line, now_us);
   } else if (c == m_s[5] || c == 127u) {
      if (m_len > 0u)
         m_len--;
   } else if (c == '/' && m_len == 1u && toupper((unsigned char)m_line[0]) == 'A') {
      m_len = 0u;
      if (m_echo)
         put("\r");
      if (m_last[0])
         execute(m_last, now_us);
   } else if (c >= 32u && c < 127u && m_len < LINE_MAX) {
      m_line[m_len++] = (char)c;
   }
}

static void dialling(uint32_t now_us)
{
   uint8_t c;
   /* Any key abandons the call - except the LF of a CR LF line ending. */
   if (serial_redirect_read(&c, 1u) != 0u && c != m_s[4]) {
      hang_up();
      result(R_NO_CARRIER);
      return;
   }
   uint8_t r = net_capi_open(m_net, m_url, NET_OPEN_RW, NULL);
   if (r == NET_OK) {
      m_state = M_ONLINE;
      m_connected = true;
      m_byte_us = now_us;
      m_plus = 0u;
      result(R_CONNECT);
   } else if (r != NET_PENDING) {
      m_net_err = r;
      hang_up();
      result(r == NET_ERR_DNS ? R_NO_ANSWER : R_NO_CARRIER);
   } else if ((now_us - m_dial_us) / 1000000u >= m_s[7]) {
      hang_up();
      result(R_NO_ANSWER);
   }
}

static void online(uint32_t now_us)
{
   uint32_t guard = (uint32_t)m_s[12] * 20000u;

   /* Beeb -> network, watching for the escape sequence as it passes. */
   if (m_out_len < NETBUF_SIZE) {
      size_t n = serial_redirect_read(m_out + m_out_len, NETBUF_SIZE - m_out_len);
      for (size_t i = 0u; i < n; i++) {
         uint8_t c = m_out[m_out_len + i];
         uint32_t idle = now_us - m_byte_us;
         m_byte_us = now_us;
         if (c == m_s[2] && m_s[2] < 128u
             && (m_plus == 0u ? idle >= guard : m_plus < 3u)) {
            m_plus++;
            m_plus_us = now_us;
         } else {
            m_plus = 0u;
         }
      }
      m_out_len += (unsigned)n;
   }
   if (m_out_len != 0u) {
      uint32_t done = 0u;
      uint8_t r = net_capi_write(m_net, m_out, m_out_len, &done);
      if (r != NET_OK) {
         hang_up();
         result(R_NO_CARRIER);
         return;
      }
      if (done != 0u) {
         memmove(m_out, m_out + done, m_out_len - done);
         m_out_len -= (unsigned)done;
      }
   }
   if (m_plus == 3u && now_us - m_plus_us >= guard) {
      m_plus = 0u;
      m_state = M_COMMAND;                          /* the line stays up */
      result(R_OK);
      return;
   }

   /* network -> Beeb, as far as the redirect has room beyond RESULT_ROOM.  A
      hang-up is only reported by a read that finds no data, so it is only
      looked for with that room free: the NO CARRIER always fits. */
   size_t room = serial_redirect_room();
   if (room >= RESULT_ROOM) {
      uint8_t buf[NETBUF_SIZE];
      uint32_t got = 0u;
      room -= RESULT_ROOM;
      uint32_t max = room < sizeof buf ? (uint32_t)room : (uint32_t)sizeof buf;
      uint8_t r = net_capi_read(m_net, buf, max, &got);
      if (got != 0u)
         (void)serial_redirect_write(buf, got);
      if (r != NET_OK) {
         m_net_err = r;
         hang_up();
         result(R_NO_CARRIER);
      }
   }
}

void modem_poll(uint32_t now_us)
{
   switch (m_state) {
   case M_COMMAND: {
      /* One byte at a time, so a command that changes state (ATD, ATO)
         leaves what follows it in the redirect for the new state to read:
         data for the line after ATO, a key that abandons the dial after
         ATD.  Taken as a batch, the rest of it was lost. */
      uint8_t c;
      for (unsigned i = 0u; i < 64u && m_state == M_COMMAND
                            && serial_redirect_read(&c, 1u) != 0u; i++)
         command_byte(c, now_us);
      /* A call held in command mode can still drop: a zero-length read
         reports it without taking any of the data waiting. */
      if (m_state == M_COMMAND && m_connected) {
         uint32_t got;
         uint8_t dummy;
         if (net_capi_read(m_net, &dummy, 0u, &got) != NET_OK) {
            hang_up();
            result(R_NO_CARRIER);
         }
      }
      break;
   }
   case M_DIALLING:
      dialling(now_us);
      break;
   case M_ONLINE:
      online(now_us);
      break;
   default:
      m_state = M_COMMAND;
      break;
   }
}

void modem_status(char *buf, size_t len)
{
   static const char *const state[] = { "command", "dialling", "online" };
   snprintf(buf, len, "%s%s net err %02x", state[m_state < 3u ? m_state : 0u],
            (m_state == M_COMMAND && m_connected) ? " (line up)" : "",
            (unsigned int)m_net_err);
}
