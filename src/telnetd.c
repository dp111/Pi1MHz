/*
  telnetd.c - a telnet server for the BBC

  telnet <the Pi> gives the Beeb's keyboard and screen.  Helper 20 (6502code.asm)
  puts the redirect stub in keyboard mode (what the Pi sends goes into the
  MOS keyboard buffer) and turns on the RS423 copy of the VDU stream (*FX3,1);
  this file is the network end.  Byte transport is serial_redirect.c, the
  telnet and VDU translation is telnetd_proto.c (host-tested); what is left
  here is lwIP, one session at a time, and a login.

       client keys  -> telnetd_proto -> key ring -> serial_redirect_write -> Beeb keyboard
       Beeb output  -> serial_redirect_read -> telnetd_proto (ANSI) -> output buffer -> client

  Built on lwIP's raw API as the web server is, so it runs in the same main
  loop and never blocks.  Keys wait in the ring until the Beeb's keyboard
  buffer has room; the ring is also what holds back the client (the receive
  callback refuses a packet it has no room for, and lwIP offers it again).

  With nobody connected the Beeb's output is read and dropped: the Beeb waits
  on a full RS423 buffer, so something has to keep emptying it.  Unlike the
  modem, nothing is echoed back, since what the Pi sends is typed on the Beeb.

  Login: a session asks for telnetd_user and telnetd_password first, telnet's own
  (never the web interface's WebDAV ones: telnet is not encrypted, so the password
  crosses the network in the clear).  Without them every connection is refused,
  unless telnetd_login=0 opens the daemon to the whole network.  Three wrong
  passwords, on any connections within FAIL_DECAY_US of each other, lock it for
  LOCKOUT_US, and each wrong one holds its session for FAIL_DELAY_US.

     telnetd_port=23      listen here; absent or 0: off
     telnetd_user=        the login: a user name, and a telnetd_password line (which may be
     telnetd_password=    empty: the password is then just RETURN); up to 64 characters each
     telnetd_ansi=1       0: the Beeb's bytes as they are, no ANSI
     telnetd_utf8=1       the pound sign as UTF-8; 0: Latin-1
     telnetd_login=1      0: no login at all, open to the network
     telnetd_mode=7       the Beeb's screen mode (0-7) until it sends a MODE: its size
                          cannot be read, and the daemon needs it to follow the text
     telnetd_copycursor=1 0: do not mark the copy cursor in the terminal
     telnetd_debug=0      1: the diagnostics on the status page (keys, MODE changes, TCP)
     telnetd_tek=1        draw the Beeb's graphics (VDU 25) in the terminal's Tektronix 4014
                          window when it says it is xterm (asked by CSI > q); 0: never
*/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "Pi1MHz.h"
#include "config.h"
#include "serial_redirect.h"
#include "telnetd.h"
#include "telnetd_proto.h"

#define KRING_SIZE   4096u               /* power of two */
#define KRING_MASK   (KRING_SIZE - 1u)
#define OBUF_SIZE    4096u
/* Output is sent in batches: once OUT_BATCH bytes are waiting or the oldest has waited OUT_HOLD_US.
   A segment per main-loop pass, a few bytes each, is a Wi-Fi frame each, and the frames, not the
   bytes, are what the link runs out of (a stream from the Beeb reached the client at ~350 bytes/s). */
#define OUT_BATCH    1024u
#define OUT_HOLD_US  8000u
#define LOGIN_MAX    64u                 /* as the web interface's user name and password may be */
#define LOGIN_US     30000000u           /* a session must be logged in by then */
#define LOCKOUT_US   60000000u           /* after every third wrong password, whichever connections they came on */
#define FAIL_DECAY_US 600000000u         /* wrong passwords are forgotten after this long without one */
#define FAIL_DELAY_US 2000000u           /* and each one holds the session this long before it can try again */
#define BEEB_PER_POLL 256u               /* Beeb bytes translated per main-loop pass */
#define KEEPALIVE_US 30000000u           /* a telnet NOP this often when idle, see telnetd_poll */
#define STALE_US     10000000u           /* a session that has not moved for this long, with
                                            something waiting on the client, may be replaced
                                            by a new connection from the same address */
#define ABORT_US     30000000u           /* ...and is dropped outright after this long */

enum { S_LOGIN_USER, S_LOGIN_PASS, S_LIVE };

static struct tcp_pcb *g_listener;
static struct tcp_pcb *g_pcb;            /* the one session */
static uint16_t        g_port;
static bool            g_ansi = true;
static bool            g_tek = true;          /* telnetd_tek: graphics to an xterm's Tek window */
static bool            g_debug;               /* telnetd_debug=1: the diagnostics on the status page */
static bool            g_utf8 = true;
static bool            g_copy = true;         /* show the copy cursor */

/* With ROM build 11 the Beeb tells each cursor key, COPY or RETURN a program takes from its keyboard
   buffer (serial_redirect_key_gen): the ones sent wait here and move the copy cursor only when taken,
   in order - so a key that only waits in the buffer (a listing in paged mode waiting for SHIFT)
   moves nothing, whatever is on the screen. */
#define CKQ_SIZE 32u
static uint8_t  g_ckq[CKQ_SIZE];
static uint32_t g_ckq_head, g_ckq_tail;
static uint32_t g_kgen_seen;                  /* the reports already used */

/* ROM build 13 tells every byte a program takes (serial_redirect_key_other_gen), so the Pi knows the keys
   waiting in the Beeb's buffer (put in less taken) and when it last took one: the status page says when
   the Beeb has stopped taking keys (a hang).  And a key taken that prints nothing (a program that writes
   the screen itself, WordWise-Plus's editor) has the screen read again, silently and merged, so the
   terminal follows it.  A byte above 127 goes behind LITKEY (TND_KEY_LIT). */
#define LIT_KEY        0xFBu                     /* the ROM's LITKEY: the next byte is read as it is */
#define KEYS_HUNG_US   5000000u                  /* keys waiting this long, none taken: "not taking keys" */
#define SILENT_KEY_US  250000u                   /* a key taken, then no character drawn this long: read the screen */
#define SILENT_SCAN_US 1000000u                  /* ... at most this often */
static bool     g_lit_next;                      /* the next key is a byte above 127 */
static uint32_t g_keys_base;                     /* put in less taken when the buffer was last emptied */
static uint32_t g_taken_seen, g_taken_us;        /* keys taken, and when the count last moved */
static uint32_t g_other_seen;                    /* other keys taken already looked at */
static bool     g_silent_wait;                   /* a key was taken: watching for output after it */
static uint32_t g_silent_us, g_silent_tx, g_silent_scan_us;   /* g_silent_tx: g_out.drawn when the key was taken */
static bool            g_repaint;             /* a new terminal is being drawn from the model */
static bool            g_scan_pending;        /* a new session still has to ask the Beeb for what is on its screen */
static bool            g_scan_merge;          /* ... and compare it with the model, which has been drawn */
static bool            g_unseen;              /* the ROM filing system printed where RS423 cannot see: a scan, once it stops */
#define UNSEEN_QUIET_US 100000u
static bool            g_break_asked;         /* F12 was pressed: the next hook follows a BREAK */
static tnd_banner_t    g_ban;                     /* what the Beeb printed just after the helper hooked in */
static uint32_t        g_banner_until;            /* banners are read until then */
static unsigned        g_repaint_row;
static uint8_t         g_mode = 7u;           /* the Beeb's mode, until it is told otherwise */

static uint8_t   g_stage;
static uint32_t  g_start_us;

/* The client's window size (NAWS, asked for in the greeting).  The screen is not drawn until the
   first size has come, or NAWS_WAIT_US has gone by (a client that says nothing): a window smaller
   than the Beeb's mode is told so before the screen, as one line.  A later size (a resize) has
   the terminal drawn again from the model, since a console that shrinks moves its rows. */
#define NAWS_WAIT_US 500000u
static uint8_t  g_win_gen;                      /* the last size acted on (tnd_in_t's win_gen) */
static bool     g_win_first;                    /* the first size has been acted on */

/* A new session (once logged in) is "connecting" for a moment: under the boot banners the line
   "Pi1MHz telnet - connecting" gets a dot every CONNECT_DOT_US, for at least CONNECT_MIN_US and
   until the client's window size and terminal type are in; the Beeb's output waits meanwhile.
   Then the terminal is cleared and the Beeb's screen drawn in place, row by row, so its rows are
   the Beeb's from the start (nothing jumps when the Beeb next scrolls or clears). */
#define CONNECT_DOT_US   300000u
#define CONNECT_MIN_US  1000000u
#define CONNECT_WARN_US 3000000u                 /* how long a too-small window's warning stays */
static bool     g_connecting;
static uint32_t g_conn_us;                       /* when it started */
static uint32_t g_dot_us;                        /* the last dot */
static uint32_t g_warn_until;                    /* a warning is being shown until then */
static bool     g_warned;
static uint32_t  g_tx_us;                /* the last time bytes went to the client */
static uint32_t  g_lockout_us;           /* no sessions until then, 0 = none */
static bool      g_locked;
static char      g_line[LOGIN_MAX + 1u];
static char      g_user[LOGIN_MAX + 1u];
static unsigned  g_len;
static unsigned  g_fails;                /* wrong passwords, kept across connections (FAIL_DECAY_US) */
static uint32_t  g_fail_us;              /* the last one */
static uint32_t  g_hold_until;           /* after a wrong password nothing is read until then */
static bool      g_hold;
static bool      g_close_after;          /* close once the output has gone */
static bool      g_att_known, g_att;     /* last attachment told to the client */
static char      g_peer[20];
static uint32_t  g_sessions, g_refused, g_replaced, g_dropped;
static const char *g_why = "none";       /* why the last session ended, for the status page */
static int       g_why_err;              /* lwIP's error code when it was lwIP's doing */
static uint32_t  g_wmem, g_room0;        /* tcp_write said ERR_MEM; there was output and no send room */
static unsigned  g_drop_sndq, g_drop_sndbuf, g_drop_wnd;   /* the TCP state when a session was dropped as stalled */
static unsigned long g_drop_obuf;
static int       g_drop_rxblk;
static uint32_t  g_prog_us;              /* the last time the session moved, or began to wait */
static bool      g_rx_blocked;           /* the key ring is full and client data is waiting for room */
static struct pbuf *g_pend;              /* client data taken from lwIP but not yet turned into keys */
static struct pbuf *g_pq;                /* ... the segment of it being read */
static u16_t     g_pqoff;                /* ... and how far into that segment */

static tnd_in_t  g_in;
static tnd_out_t g_out;

/* The model's MODE changes, for the status page (telnetd_debug=1): how many, the last one (from, to, and
   how: 'v' a VDU 22 in what the Beeb printed, 'h' a hook or reset report), and the Beeb's last bytes
   before it.  A model in the wrong MODE draws everything at the wrong width (seen once: 20 columns
   while the Beeb was in MODE 0). */
#define MC_RING 24u
static uint8_t  g_mc_ring[MC_RING], g_mc_last[MC_RING];
static uint32_t g_mc_n, g_mc_count, g_mc_us;
static uint8_t  g_mc_mode, g_mc_from, g_mc_how;

static void mode_changed(uint8_t how)
{
   for (unsigned i = 0u; i < MC_RING; i++)      /* oldest first */
      g_mc_last[i] = g_mc_ring[(g_mc_n + i) % MC_RING];
   g_mc_from = g_mc_mode;
   g_mc_mode = g_out.mode;
   g_mc_how = how;
   g_mc_count++;
   g_mc_us = Pi1MHz_now_us;
}
static uint8_t   g_kring[KRING_SIZE];    /* keys for the Beeb */
static uint32_t  g_khead, g_ktail;
static uint8_t   g_obuf[OBUF_SIZE];      /* bytes for the client */
static size_t    g_olen, g_opos;
static uint32_t  g_obuf_us;              /* when the oldest byte not yet given to TCP was put in */

/* ---- buffers ------------------------------------------------------------ */

static void o_compact(void)
{
   if (g_opos != 0u) {
      memmove(g_obuf, g_obuf + g_opos, g_olen - g_opos);
      g_olen -= g_opos;
      g_opos = 0u;
   }
}

static size_t o_room(void)
{
   return OBUF_SIZE - g_olen + g_opos;
}

static void o_put(const void *p, size_t n)
{
   if (n > o_room())
      n = o_room();
   if (g_olen == g_opos && n != 0u)
      g_obuf_us = Pi1MHz_now_us;
   o_compact();
   memcpy(g_obuf + g_olen, p, n);
   g_olen += n;
}

static void o_str(const char *s)
{
   o_put(s, strlen(s));
}

/* The byte the Pi puts in the Beeb's keyboard buffer to ask for the screen: the auto-start
   ROM takes it out of REMV before any program sees it. */
#define SCAN_KEY 0xFFu


static uint32_t g_scan_keys;                       /* scan keys queued and not yet told taken: not silent keys */

static void k_push(uint8_t k)
{
   if (k != SCAN_KEY)
      g_banner_until = Pi1MHz_now_us;              /* somebody is typing: the start-up is over */
   else
      g_scan_keys++;
   if (g_ktail - g_khead < KRING_SIZE) {
      g_kring[g_ktail & KRING_MASK] = k;
      g_ktail++;
   }
}

/* ---- the session -------------------------------------------------------- */

static bool cfg_flag(const char *key, bool dflt)
{
   const char *v = config_get(key);
   if (v == NULL || v[0] == '\0')
      return dflt;
   return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

/* Telnet has its own user name and password (telnetd_user, telnetd_password), never the web
   interface's: telnet sends them in the clear.  Without them a session is refused, unless
   telnetd_login=0 says the daemon is to be open to the network. */
static bool login_open(void)
{
   return !cfg_flag("telnetd_login", true);
}

/* A user name, and a telnetd_password line, which may be empty: then the password is RETURN. */
static bool login_configured(void)
{
   const char *u = config_get("telnetd_user"), *p = config_get("telnetd_password");
   return u != NULL && u[0] != '\0' && p != NULL;
}

/* Equal strings, in a time that depends only on the attempt's length, not on where they differ. */
static bool same_secret(const char *got, const char *want)
{
   size_t   gl = strlen(got), wl = strlen(want);
   unsigned diff = (unsigned)(gl != wl);
   for (size_t i = 0u; i < gl; i++)
      diff |= (unsigned)((uint8_t)got[i] ^ (uint8_t)(i < wl ? want[i] : 0u));
   return diff == 0u;
}

static bool credentials_ok(void)
{
   const char *u = config_get("telnetd_user"), *p = config_get("telnetd_password");
   bool user_ok, pass_ok;
   if (u == NULL || p == NULL)
      return false;
   user_ok = same_secret(g_user, u);
   pass_ok = same_secret(g_line, p);            /* both compared, whichever is wrong */
   return user_ok && pass_ok;
}

static void pend_free(void)
{
   if (g_pend != NULL)
      pbuf_free(g_pend);
   g_pend = g_pq = NULL;
   g_pqoff = 0u;
}

static void session_reset(void)
{
   g_repaint = false;
   g_connecting = false;
   pend_free();
   g_khead = g_ktail = 0u;
   g_olen = g_opos = 0u;
   g_len = 0u;
   g_hold = false;                               /* g_fails is not reset: a reconnect must not clear it */
   g_close_after = false;
   g_att_known = false;
   g_rx_blocked = false;
   g_stage = S_LIVE;
}

/* Something is waiting on the client: output it has not acknowledged.  Keys refused because the
   ring is full are not: that is the Beeb being slow to take them, with the client alive. */
static bool session_pending(void)
{
   return g_pcb != NULL && g_pcb->snd_queuelen != 0u;
}

/* Returns true if it had to abort: a callback that does must return ERR_ABRT. */
static bool session_close(bool abort_conn)
{
   bool aborted = false;
   struct tcp_pcb *pcb = g_pcb;

   g_pcb = NULL;
   session_reset();
   if (pcb == NULL)
      return false;
   tcp_arg(pcb, NULL);
   tcp_recv(pcb, NULL);
   tcp_sent(pcb, NULL);
   tcp_poll(pcb, NULL, 0);
   tcp_err(pcb, NULL);
   if (abort_conn) {
      tcp_abort(pcb);
      aborted = true;
   } else if (tcp_close(pcb) != ERR_OK) {
      tcp_abort(pcb);
      aborted = true;
   }
   return aborted;
}

static void connect_start(void)
{
   g_connecting = true;
   g_conn_us = g_dot_us = Pi1MHz_now_us;
   g_warn_until = 0u;
   g_warned = false;
   o_str("\r\nPi1MHz telnet - connecting");
}

static void send_banners(void)                   /* what came up when the Beeb started */
{
   for (unsigned i = 0u; i < g_ban.len; i++) {
      if (g_ban.text[i] == '\n')
         o_str("\r\n");
      else
         o_put(&g_ban.text[i], 1u);
   }
}

static void login_key(uint8_t k)
{
   if (g_hold) {                                 /* after a wrong password: typing is ignored for a while */
      if ((int32_t)(Pi1MHz_now_us - g_hold_until) < 0)
         return;
      g_hold = false;
   }
   if (k == 13u) {
      if (g_stage == S_LOGIN_USER) {
         g_line[g_len] = '\0';
         memcpy(g_user, g_line, sizeof g_user);
         g_len = 0u;
         g_stage = S_LOGIN_PASS;
         o_str("\r\nPassword: ");
         return;
      }
      g_line[g_len] = '\0';
      g_len = 0u;
      if (g_fails != 0u && (Pi1MHz_now_us - g_fail_us) >= FAIL_DECAY_US)
         g_fails = 0u;                           /* long enough since the last wrong one */
      if (credentials_ok()) {
         g_fails = 0u;
         g_stage = S_LIVE;
         g_att_known = false;
         o_str("\r\n");
         send_banners();                         /* only now: nothing of the Beeb's before a login */
         connect_start();
      } else {
         g_fail_us = Pi1MHz_now_us;
         g_hold = true;                          /* every wrong one costs FAIL_DELAY_US */
         g_hold_until = Pi1MHz_now_us + FAIL_DELAY_US;
         if (++g_fails % 3u == 0u) {             /* every third, on whichever connections: locked out */
            o_str("\r\nLogin incorrect\r\n");
            g_close_after = true;
            g_locked = true;
            g_lockout_us = Pi1MHz_now_us;
         } else {
            o_str("\r\nLogin incorrect\r\n\r\nlogin: ");
            g_stage = S_LOGIN_USER;
         }
      }
      return;
   }
   if (k == 127u) {
      if (g_len != 0u) {
         g_len--;
         if (g_stage == S_LOGIN_USER)
            o_put("\b \b", 3u);
      }
      return;
   }
   if (k >= 32u && k < 127u && g_len < LOGIN_MAX) {
      g_line[g_len++] = (char)k;
      if (g_stage == S_LOGIN_USER)
         o_put(&k, 1u);                         /* the password is not echoed */
   }
}

/* One key from the client: a key for the Beeb, a login character, or the ESCAPE
   request.  ESCAPE goes straight to the redirect instead of through the key
   ring - the Beeb may be busy in a loop with its keyboard buffer full of
   type-ahead, which is when it is wanted - and, as the Beeb's own ESCAPE does,
   it discards what was typed ahead. */
static uint32_t keys_waiting(void)
{
   uint32_t taken = serial_redirect_key_gen() + serial_redirect_key_other_gen();
   uint32_t w = serial_redirect_rx_count() - taken - g_keys_base;
   return w > 0x80000000u ? 0u : w;
}

static void keys_emptied(void)                   /* the Beeb's buffer emptied (ESCAPE, a reset) */
{
   g_scan_keys = 0u;                             /* a scan key in it went too */
   g_keys_base = serial_redirect_rx_count() - serial_redirect_key_gen() - serial_redirect_key_other_gen();
}

static void deliver_key(uint8_t k)
{
   if (g_lit_next) {                             /* the byte after TND_KEY_LIT: any value, as it is */
      g_lit_next = false;
      if (g_stage == S_LIVE && serial_redirect_lit_ok()) {
         k_push(LIT_KEY);
         k_push(k);
      }
      return;
   }
   if (k == TND_KEY_LIT) {
      g_lit_next = true;
      return;
   }
   if (g_stage != S_LIVE) {
      if (k != TND_KEY_ESCAPE && k != TND_KEY_BREAK && k != TND_KEY_REFRESH && k != TND_KEY_SHIFT)
         login_key(k);
   } else if (k == TND_KEY_BREAK) {
      g_khead = g_ktail;                           /* what was typed ahead goes, as at a BREAK */
      g_ckq_head = g_ckq_tail;
      g_break_asked = true;                        /* the next hook is the start of a new boot */
      serial_redirect_break();
   } else if (k == TND_KEY_REFRESH) {
      if (serial_redirect_scan_ok())
         k_push(SCAN_KEY);                            /* the ROM keeps it from the program and sends the screen */
   } else if (k == TND_KEY_SHIFT) {
      if (serial_redirect_scan_ok())
         serial_redirect_shift();                     /* a trip, not a key: nothing queued can hold it up */
   } else if (k == TND_KEY_ESCAPE) {
      g_khead = g_ktail;
      g_ckq_head = g_ckq_tail;                      /* the escape flushes the Beeb's buffer too */
      serial_redirect_escape();
      keys_emptied();
      g_silent_wait = true;                      /* and a screen the ESCAPE changed without printing is read */
      g_silent_us = Pi1MHz_now_us;
      g_silent_tx = g_out.drawn;
      if (g_copy && g_ansi)
         tnd_out_copykey(&g_out, 27u);
   } else {
      k_push(k);
      if (g_copy && g_ansi && ((k >= 139u && k <= 143u) || k == 13u)) {
         if (serial_redirect_keys_told()) {         /* moved when the Beeb takes it (see g_ckq) */
            if (g_ckq_tail - g_ckq_head < CKQ_SIZE)
               g_ckq[g_ckq_tail++ % CKQ_SIZE] = k;
         } else {
            tnd_out_copykey(&g_out, k);            /* cursor keys, COPY, RETURN: the copy cursor */
         }
      }
   }
}

static void client_byte(uint8_t c, uint32_t now_us)
{
   uint8_t keys[TND_IN_KEYS], rep[TND_IN_REPLY];
   size_t  rl;
   size_t  n = tnd_in_byte(&g_in, c, now_us, keys, rep, &rl);

   if (rl != 0u)
      o_put(rep, rl);
   for (size_t i = 0u; i < n; i++)
      deliver_key(keys[i]);
}

/* The boot banners (tnd_banner_byte): read for the first few seconds after a start, and kept for
   the greeting of the next session.  The screen model never sees a VDU 21 block (VDU 21 silences
   it). */
static void banner_byte(uint8_t c)
{
   if (tnd_banner_byte(&g_ban, c, (int32_t)(Pi1MHz_now_us - g_banner_until) < 0))
      g_banner_until = Pi1MHz_now_us;              /* the language's prompt: the start-up is over */
}

/* With nobody connected, or before the login, the Beeb's output is only followed. */
static void beeb_follow(uint8_t c)
{
   banner_byte(c);
   if (g_ansi) {
      uint8_t d[TND_OUT_MAX];
      (void)tnd_out_byte(&g_out, c, d);
      tnd_out_touch(&g_out, Pi1MHz_now_us);
   }
}

static void beeb_byte(uint8_t c)
{
   banner_byte(c);
   if (g_ansi) {
      uint8_t d[TND_OUT_MAX];
      size_t  n = tnd_out_byte(&g_out, c, d);
      tnd_out_touch(&g_out, Pi1MHz_now_us);
      if (n != 0u)
         o_put(d, n);
   } else if (c == 0xFFu) {                     /* IAC must be doubled */
      static const uint8_t ff[2] = { 0xFF, 0xFF };
      o_put(ff, 2u);
   } else {
      o_put(&c, 1u);
   }
}

/* ---- lwIP callbacks ------------------------------------------------------- */

/* Turn waiting client data into keys while the ring has room, and tell lwIP how
   much has been read.  Called when data arrives and from the poll. */
static void pend_consume(void)
{
   uint32_t now = Pi1MHz_now_us;
   u32_t    n = 0u;

   while (g_pq != NULL && g_pcb != NULL) {
      const uint8_t *d = (const uint8_t *)g_pq->payload;
      while (g_pqoff < g_pq->len) {
         if (KRING_SIZE - (g_ktail - g_khead) < TND_IN_KEYS)
            goto full;                          /* a byte releases at most TND_IN_KEYS keys */
         client_byte(d[g_pqoff++], now);
         n++;
      }
      g_pq = g_pq->next;
      g_pqoff = 0u;
   }
   pend_free();                                 /* all of it read */
   g_rx_blocked = false;
   if (n != 0u && g_pcb != NULL)
      tcp_recved(g_pcb, (u16_t)(n > 0xFFFFu ? 0xFFFFu : n));
   if (n != 0u)
      g_prog_us = now;
   return;

full:
   if (n != 0u) {
      tcp_recved(g_pcb, (u16_t)(n > 0xFFFFu ? 0xFFFFu : n));
      g_prog_us = now;
      g_rx_blocked = false;
   }
   if (!g_rx_blocked) {
      g_rx_blocked = true;
      g_prog_us = now;
   }
}

static err_t td_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
   (void)arg;

   if (pcb != g_pcb) {                          /* not (or no longer) the session */
      if (p != NULL)
         pbuf_free(p);
      return ERR_OK;
   }
   if (err != ERR_OK) {
      if (p != NULL)
         pbuf_free(p);
      g_why = "recv error";
      g_why_err = (int)err;
      (void)session_close(true);
      return ERR_ABRT;
   }
   if (p == NULL) {                             /* the client closed */
      g_why = "client closed";
      return session_close(false) ? ERR_ABRT : ERR_OK;
   }

   /* The data is ours now.  It is read into the key ring as fast as the Beeb
      empties it and the window stays closed (tcp_recved) for what has not been
      read, which is what holds the client back.  (Refusing the packet with
      ERR_MEM instead does not work: lwIP can hand over several queued segments
      as one chain, and one longer than half the ring would never fit.) */
   if (g_pend == NULL) {
      g_pend = g_pq = p;
      g_pqoff = 0u;
   } else {
      pbuf_cat(g_pend, p);
      if (g_pq == NULL)
         g_pq = p;
   }
   pend_consume();
   return ERR_OK;
}

/* The client acknowledged some of what was sent: it is there. */
static err_t td_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
   (void)arg;
   (void)len;
   if (pcb == g_pcb)
      g_prog_us = Pi1MHz_now_us;
   return ERR_OK;
}

static void td_err(void *arg, err_t err)
{
   (void)arg;
   g_why = "lwIP error";
   g_why_err = (int)err;
   g_pcb = NULL;                                /* lwIP has already freed the pcb */
   session_reset();
}

static err_t td_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
   (void)arg;

   if (err != ERR_OK || newpcb == NULL)
      return ERR_VAL;

   if (g_locked && (Pi1MHz_now_us - g_lockout_us) >= LOCKOUT_US)
      g_locked = false;
   /* The one session is a dead one, and this is the same machine calling again
      (it gave up on the old connection, as a client that crashed or was killed
      does): the old one goes.  Only when it has really stopped moving - a session
      that is merely quiet or slow is not replaced. */
   if (g_pcb != NULL && !g_locked && ip_addr_cmp(&g_pcb->remote_ip, &newpcb->remote_ip)
       && tnd_stalled(session_pending(), Pi1MHz_now_us, g_prog_us, STALE_US)) {
      g_replaced++;
      g_why = "replaced";
      (void)session_close(true);
   }
   /* A session still at the login prompt holds nothing yet: a new connection takes its place, so
      nobody can keep the owner out by connecting and waiting. */
   if (g_pcb != NULL && !g_locked && g_stage != S_LIVE) {
      g_replaced++;
      g_why = "replaced at login";
      (void)session_close(true);
   }
   if (g_pcb != NULL || g_locked || (!login_open() && !login_configured())) {
      static const char busy[]   = "Pi1MHz: the BBC is in use by another session.\r\n";
      static const char locked[] = "Pi1MHz: too many wrong passwords, try later.\r\n";
      static const char unset[]  = "Pi1MHz: telnet has no login set up (telnetd_user and telnetd_password).\r\n";
      const char *m = g_locked ? locked : g_pcb != NULL ? busy : unset;
      g_refused++;
      (void)tcp_write(newpcb, m, (u16_t)strlen(m), TCP_WRITE_FLAG_COPY);
      (void)tcp_output(newpcb);
      if (tcp_close(newpcb) != ERR_OK) {
         tcp_abort(newpcb);
         return ERR_ABRT;
      }
      return ERR_OK;
   }

   g_pcb = newpcb;
   g_sessions++;
   session_reset();
   g_start_us = g_tx_us = g_prog_us = Pi1MHz_now_us;
   snprintf(g_peer, sizeof g_peer, "%s", ip4addr_ntoa(ip_2_ip4(&newpcb->remote_ip)));
   tcp_nagle_disable(newpcb);                   /* keys and echoes must not wait */
   tcp_arg(newpcb, NULL);
   tcp_recv(newpcb, td_recv);
   tcp_sent(newpcb, td_sent);
   tcp_err(newpcb, td_err);

   tnd_in_reset(&g_in);
   g_ckq_head = g_ckq_tail;                     /* a new terminal: no copy keys waiting */
   g_kgen_seen = serial_redirect_key_gen();
   g_win_gen = 0u;
   g_win_first = false;
   tnd_out_session(&g_out);                     /* the model has been following the Beeb all along */
   tnd_out_tek(&g_out, false);                  /* until this terminal says it is xterm */
   g_out.tek_on = false;
   /* The ROM is asked for the Beeb's screen (below).  A model that knows nothing is drawn from it; one
      that does is drawn first, with its colours, and the read is only compared with it: a program can
      have changed the screen where the Pi could not see (output with the RS423 copy off, writes to
      screen memory), and then the terminal is drawn again from the Beeb's characters. */
   g_scan_pending = true;
   g_scan_merge = g_out.sync && !tnd_out_blank(&g_out);
   g_repaint = false;                           /* drawn in place once connected (connect_done) */
   g_repaint_row = 0u;
   g_connecting = false;
   {
      uint8_t g[TND_GREETING_LEN];
      o_put(g, tnd_greeting(g));
   }
   if (g_ansi)
      o_str("\x1b[0m");
   if (!login_open()) {                         /* the banners come after the login */
      g_stage = S_LOGIN_USER;
      o_str("\r\nPi1MHz telnet\r\n\r\nlogin: ");
   } else {
      send_banners();
      connect_start();
   }
   return ERR_OK;
}

/* ---- the main loop's half --------------------------------------------------- */

static void pump(void)
{
   if (g_pcb == NULL)
      return;
   if (g_opos < g_olen && (g_olen - g_opos >= OUT_BATCH || g_close_after
                           || (uint32_t)(Pi1MHz_now_us - g_obuf_us) >= OUT_HOLD_US)) {
      size_t want = g_olen - g_opos;
      size_t room = tcp_sndbuf(g_pcb);
      if (room == 0u)
         g_room0++;
      if (want > room)
         want = room;
      if (want != 0u) {
         bool  was_idle = g_pcb->snd_queuelen == 0u;
         err_t e = tcp_write(g_pcb, g_obuf + g_opos, (u16_t)want, TCP_WRITE_FLAG_COPY);
         if (e == ERR_OK) {
            g_opos += want;
            g_tx_us = Pi1MHz_now_us;
            if (was_idle)
               g_prog_us = g_tx_us;              /* the wait for the first ACK starts now */
            (void)tcp_output(g_pcb);
         } else if (e == ERR_MEM) {             /* ERR_MEM: the heap is busy, try again */
            g_wmem++;
         } else {
            g_why = "write error";
            g_why_err = (int)e;
            (void)session_close(true);
            return;
         }
      }
      if (g_opos == g_olen)
         g_opos = g_olen = 0u;
   }
   if (g_close_after && g_opos == g_olen) {
      g_why = "login";
      (void)session_close(false);
   }
}

void telnetd_poll(uint32_t now_us, bool beeb_attached)
{
   static uint32_t mode_seen;
   uint8_t c;

   /* A reset starts everything again: the screen is new, so the model is, and the next few
      seconds hold the boot banners.  It is seen on the Beeb's reset line, or told by the
      auto-start ROM (a software BREAK never touches the line), or asked for by F12, or the
      hook finds the cursor in the corner of the screen. */
   static uint32_t reset_seen;
   bool            hooked = serial_redirect_mode_gen() != mode_seen;
   static uint32_t last_hook_us;
   /* A hook that comes with F12 or well after the last one (the ROM and then HELPERS
      hook within seconds of each other in one boot, a disc being slow) follows a reset
      the Pi never saw on the reset line, such as CALL !-4: the Beeb has a new screen. */
   bool            hw_reset = serial_redirect_reset_gen() != reset_seen;
   bool            reset = hw_reset
                           || (hooked && (g_break_asked || (Pi1MHz_now_us - last_hook_us) > 30000000u));
   if (hooked || hw_reset)
      last_hook_us = Pi1MHz_now_us;
   if (hooked && !reset) {
      uint8_t rc, rr;
      serial_redirect_beeb_cursor(&rc, &rr);
      reset = (rc == 0u && rr == 0u);
   }
   if (hooked || reset) {
      uint8_t col, row;
      mode_seen = serial_redirect_mode_gen();
      reset_seen = serial_redirect_reset_gen();
      keys_emptied();                           /* a reset empties the Beeb's buffer */
      if (hooked)
         g_break_asked = false;
      if (reset || serial_redirect_beeb_mode() != g_out.mode)
         tnd_out_assume_mode(&g_out, serial_redirect_beeb_mode());
      if (g_out.mode != g_mc_mode)
         mode_changed('h');
      serial_redirect_beeb_cursor(&col, &row);
      tnd_out_at(&g_out, col, row);
      if (reset) {
         g_out.coloured = false;               /* a reset puts the colours and the cursor back */
         g_out.curoff = false;
         if (g_pcb != NULL && g_stage == S_LIVE && g_ansi) {
            /* Somebody is connected: their terminal is refreshed too, so what the Beeb prints
               from here (the new banners) lands where the model says. */
            char cup[40];
            snprintf(cup, sizeof cup, "\x1b[0m\x1b[?25h\x1b[2J\x1b[%u;%uH", (unsigned)row + 1u, (unsigned)col + 1u);
            o_str(cup);                        /* cleared, and the cursor where the Beeb's is */
            g_out.origin_ok = true;
            g_repaint = false;
         }
         tnd_banner_reset(&g_ban);             /* the banners of the last start are no longer true ... */
         g_banner_until = Pi1MHz_now_us + 4000000u;   /* ... and the next four seconds hold the new ones */
      }
   }
   bool    live = g_pcb != NULL && g_stage == S_LIVE && !g_close_after;

   /* What the Beeb printed.  With a client to show it to it is translated, and
      the rest waits in the Beeb's buffer while the output buffer is full;
      with none (or before login) it is read and dropped. */
   if (beeb_attached && !((g_repaint || g_connecting) && g_stage == S_LIVE)) {   /* held while connecting */
      for (unsigned i = 0u; i < BEEB_PER_POLL; i++) {
         if (live && o_room() < TND_OUT_MAX + 2u)
            break;
         if (serial_redirect_read(&c, 1u) == 0u)
            break;
         g_mc_ring[g_mc_n++ % MC_RING] = c;
         if (live)
            beeb_byte(c);
         else
            beeb_follow(c);
         if (g_out.mode != g_mc_mode)
            mode_changed('v');
      }
   }

   if (g_pcb == NULL)
      return;
   tnd_out_tek(&g_out, g_tek && g_ansi && g_in.xterm);   /* the terminal answered CSI > q as xterm */

   g_in.lit_ok = serial_redirect_lit_ok();       /* bytes above 127 can go (ROM build 13) */
   g_in.tele = g_out.tele;                       /* MODE 7: its glyphs come back as its codes */

   /* Every key taken (ROM build 13): when the count last moved, and a key that drew no character
      (WordWise-Plus moves the cursor with VDU 31 and writes the screen memory itself). */
   {
      uint32_t taken = serial_redirect_key_gen() + serial_redirect_key_other_gen();
      if (taken != g_taken_seen) {
         g_taken_seen = taken;
         g_taken_us = now_us;
      }
      uint32_t other = serial_redirect_key_other_gen();
      if (other != g_other_seen) {
         uint32_t d = other - g_other_seen;
         g_other_seen = other;
         if (d <= g_scan_keys) {                     /* the Pi's own scan keys: the read draws or agrees */
            g_scan_keys -= d;
         } else {
            g_scan_keys = 0u;
            g_silent_wait = true;
            g_silent_us = now_us;
            g_silent_tx = g_out.drawn;
         }
      }
      if (g_silent_wait && (now_us - g_silent_us) >= SILENT_KEY_US
          && (serial_redirect_pending() || g_khead != g_ktail || keys_waiting() != 0u)) {
         g_silent_us = now_us;                      /* still busy: its output not all read (the client can
                                                       be holding it back), or more keys to take - wait on */
      } else if (g_silent_wait && (now_us - g_silent_us) >= SILENT_KEY_US) {
         g_silent_wait = false;
         if (g_out.drawn == g_silent_tx && serial_redirect_lit_ok() && g_ansi && g_pcb != NULL
             && g_stage == S_LIVE && !g_scan_pending && (now_us - g_silent_scan_us) >= SILENT_SCAN_US) {
            g_silent_scan_us = now_us;
            g_scan_pending = true;                 /* the screen read, merged: only what differs is drawn */
            g_scan_merge = g_out.sync;
         }
      }
   }

   /* The keys the Beeb has taken (ROM build 11): each moves the copy cursor, in order. */
   g_out.cc_exact = serial_redirect_keys_told();
   {
      uint32_t gen = serial_redirect_key_gen();
      while (g_kgen_seen != gen) {
         g_kgen_seen++;
         if (g_ckq_head != g_ckq_tail)
            tnd_out_copykey(&g_out, g_ckq[g_ckq_head++ % CKQ_SIZE]);
      }
   }

   if (g_stage != S_LIVE && !g_close_after && (now_us - g_start_us) >= LOGIN_US) {
      o_str("\r\nLogin timed out\r\n");          /* once: the session closes when it has gone */
      g_close_after = true;
   }

   /* lwIP's own retransmission gives up on a silent client only after minutes.
      A session with something waiting on the client that has not moved for
      ABORT_US is dropped, so a client that died mid-output cannot hold the one
      session slot. */
   if (tnd_stalled(session_pending(), now_us, g_prog_us, ABORT_US)) {
      g_drop_sndq = g_pcb->snd_queuelen;
      g_drop_sndbuf = tcp_sndbuf(g_pcb);
      g_drop_wnd = g_pcb->snd_wnd;
      g_drop_obuf = (unsigned long)(g_olen - g_opos);
      g_drop_rxblk = g_rx_blocked;
      g_dropped++;
      g_why = "stalled";
      (void)session_close(true);
      return;
   }

   /* A client that has vanished (a pulled cable, a crashed machine) would keep
      the only session for ever, and lwIP's own keepalive is not compiled in.
      An idle session is sent a telnet NOP (IAC NOP, which every client ignores)
      now and then: a client that is gone does not acknowledge it, and lwIP
      aborts the connection after its retransmissions. */
   if (g_olen == g_opos && (now_us - g_tx_us) >= KEEPALIVE_US) {
      static const uint8_t nop[2] = { 255u, 241u };
      o_put(nop, 2u);
   }

   {
      uint8_t k[TND_IN_KEYS];
      if (tnd_in_tick(&g_in, now_us, k) != 0u)
         deliver_key(k[0]);
   }

   if (g_pend != NULL)
      pend_consume();                           /* client data waiting for room in the ring */

   /* The client's window size: the first one before the screen is drawn, a change after it. */
   if (g_pcb != NULL && g_stage == S_LIVE && g_in.win_gen != g_win_gen && o_room() >= TND_COPY_MAX + 120u) {
      g_win_gen = g_in.win_gen;
      if (!g_win_first) {
         g_win_first = true;                     /* the first: the connecting phase looks at it */
      } else if (g_ansi && !g_repaint && !g_connecting) {
         uint8_t rb[TND_COPY_MAX];
         o_put(rb, tnd_out_redraw(&g_out, rb));   /* resized: drawn again by position */
      }
   }
   bool naws_wait = !g_win_first && !g_in.naws_no && (now_us - g_start_us) < NAWS_WAIT_US;

   /* Connecting: a dot now and then; done once the minimum is over and the client has said its
      window size (or not, in time) - then a too-small window is told so for a while, and then
      the terminal is cleared and the Beeb's screen drawn in place. */
   if (g_connecting && g_stage == S_LIVE && g_pcb != NULL && o_room() >= TND_COPY_MAX + 160u) {
      if (g_warn_until == 0u && (now_us - g_dot_us) >= CONNECT_DOT_US) {
         g_dot_us = now_us;
         o_str(" .");
      }
      bool ready = (now_us - g_conn_us) >= CONNECT_MIN_US && !naws_wait;
      if (ready && !g_warned) {
         g_warned = true;
         o_str("\r\n");
         if (g_ansi && g_win_first && g_in.win_rows != 0u && g_in.win_cols != 0u
             && (g_in.win_rows < g_out.rows || g_in.win_cols < g_out.cols)) {
            char w[160];
            /* A client that reports a resize has the screen drawn again by itself; Windows telnet
               (terminal type ANSI) reports its size only when it connects, so it needs F11. */
            bool no_naws_change = g_in.ttype_got && strncmp(g_in.ttype, "ANSI", 4) == 0;
            snprintf(w, sizeof w, "\r\nThis window is %u x %u, smaller than the Beeb's MODE %u (%u x %u):\r\n"
                     "%s\r\n",
                     (unsigned)g_in.win_cols, (unsigned)g_in.win_rows, (unsigned)g_out.mode,
                     (unsigned)g_out.cols, (unsigned)g_out.rows,
                     no_naws_change ? "make it bigger, then press F11." : "make it bigger and the screen is drawn again.");
            o_str(w);
            g_warn_until = now_us + CONNECT_WARN_US;
            if (g_warn_until == 0u)
               g_warn_until = 1u;
         }
      } else if (ready && (g_warn_until == 0u || (int32_t)(now_us - g_warn_until) >= 0
                           || (g_in.win_rows >= g_out.rows && g_in.win_cols >= g_out.cols))) {   /* made big enough */
         g_connecting = false;
         if (g_ansi && g_tek && g_in.ttype_got
             && (strncmp(g_in.ttype, "XTERM", 5) == 0 || strncmp(g_in.ttype, "xterm", 5) == 0))
            o_str("\x1b[>q");                   /* XTVERSION: a real xterm can draw the Beeb's graphics */
         if (g_ansi) {
            uint8_t rb[TND_COPY_MAX];
            size_t  rn = tnd_out_redraw(&g_out, rb); /* cleared, drawn row by row from the model ... */
            if (rn != 0u) {
               o_put(rb, rn);
            } else {                             /* ... or, knowing nothing, cleared for the screen read */
               o_str("\x1b[0m\x1b[2J\x1b[H");
               g_out.origin_ok = true;
            }
            uint8_t cb[16];
            o_put(cb, tnd_out_colours(&g_out, cb));
            if (beeb_attached) {                 /* the screen says it all: no "attached" line over it */
               g_att_known = true;
               g_att = true;
            }
         } else {
            o_str("\r\n");
         }
      }
   }

   if (g_repaint && !naws_wait && g_stage == S_LIVE && g_pcb != NULL && o_room() >= TND_REPAINT_MAX + 2u) {
      uint8_t rb[TND_REPAINT_MAX];
      size_t  rn = tnd_out_repaint(&g_out, g_repaint_row, rb);
      if (rn != 0u)
         o_put(rb, rn);
      if (++g_repaint_row > g_out.rows) {
         g_repaint = false;
         tnd_out_origin_lost(&g_out);            /* the terminal's rows are under the greeting: not the Beeb's */
         if (beeb_attached) {                    /* the screen says it all: no "attached" line over it */
            g_att_known = true;
            g_att = true;
         }
      }
   }

   /* A terminal that has just come is shown what is on the Beeb's screen: the ROM reads it and
      sends it (silently), and the terminal is cleared and drawn from that.  Once the greeting
      and the model's own picture are out. */
   /* The same after a ROM filing system operation, which printed to the Beeb's screen only
      (see serial_redirect_unseen_gen).  The ROM reports it on every ROM it pages, so the
      scan is asked for once the reports have stopped for UNSEEN_QUIET_US; it runs when the
      Beeb next reads the keyboard, after the operation. */
   {
      static uint32_t unseen_seen;
      static uint32_t unseen_us;
      uint32_t        gen = serial_redirect_unseen_gen();
      if (gen != unseen_seen) {
         unseen_seen = gen;
         unseen_us = now_us;
         g_unseen = true;
      } else if (g_unseen && (now_us - unseen_us) >= UNSEEN_QUIET_US) {
         g_unseen = false;
         g_scan_pending = true;
      }
   }
   if (g_out.rescan) {                           /* a compared read was cut short: an ordinary one */
      g_out.rescan = false;
      g_scan_pending = true;
   }
   if (g_scan_pending && g_stage == S_LIVE && !g_repaint && !g_connecting && !naws_wait && g_pcb != NULL && beeb_attached
       && serial_redirect_scan_ok() && g_khead == g_ktail) {
      g_scan_pending = false;
      if (g_scan_merge) {
         g_scan_merge = false;
         tnd_out_scan_merge(&g_out);
      }
      k_push(SCAN_KEY);
   }

   /* A narrow text window that scrolled or was cleared is drawn again from the model,
      a row at a time, once the Beeb has nothing more to print for now. */
   if (g_out.dirty && g_stage == S_LIVE && g_pcb != NULL && !g_repaint && !serial_redirect_pending()
       && o_room() >= TND_REPAINT_MAX + 2u) {
      uint8_t db[TND_REPAINT_MAX];
      size_t  dn = tnd_out_dirty_row(&g_out, db);
      if (dn != 0u)
         o_put(db, dn);
   }

   /* The copy cursor's mark, once the Beeb has been quiet for a moment and has
      the keys. */
   if (g_pcb != NULL && g_stage == S_LIVE && g_copy && g_ansi && g_khead == g_ktail
       && o_room() >= TND_COPY_MAX + 2u) {
      uint8_t cb[TND_COPY_MAX];
      size_t  cn = tnd_out_copyflush(&g_out, now_us, cb);
      if (cn != 0u)
         o_put(cb, cn);
   }

   /* The keys, as far as the Beeb's keyboard buffer has room (the redirect's
      own FIFO, and behind it the stub's retry when the MOS buffer is full). */
   if (beeb_attached && g_stage == S_LIVE) {
      while (g_khead != g_ktail) {
         size_t room = serial_redirect_room();
         uint32_t idx = g_khead & KRING_MASK;
         size_t chunk = g_ktail - g_khead;
         size_t n;
         if (room == 0u)
            break;
         if (chunk > KRING_SIZE - idx)
            chunk = KRING_SIZE - idx;
         if (chunk > room)
            chunk = room;
         n = serial_redirect_write(&g_kring[idx], chunk);
         g_khead += (uint32_t)n;
         if (n < chunk)
            break;
      }
   }

   /* Only a Beeb that has gone is said: a line printed when it comes back (it used to say
      "[BBC attached - press RETURN]") would push the terminal's rows one away from the Beeb's
      just after a reset had put them in step, and the copy cursor's mark then landed a row off. */
   if (g_stage == S_LIVE && !g_repaint && !g_connecting && (!g_att_known || beeb_attached != g_att)) {
      if (!beeb_attached) {
         o_str("[BBC not attached: on the BBC type X%=20:CALL &FC88]\r\n");
         tnd_out_origin_lost(&g_out);           /* that line moved the rows: not the Beeb's now */
      }
      g_att_known = true;
      g_att = beeb_attached;
   }

   pump();
}

/* ---- setup and status ------------------------------------------------------- */

/* TelnetD is off until telnetd_port is set, and Telnetd_addr=-1 turns it off whatever else is set,
   as <name>_addr=-1 does for every other device (it has no FRED address of its own). */
static long telnetd_port_cfg(void)
{
   const char *v = config_get("telnetd_port");
   uint8_t     unused = 0u;
   long        port = (v != NULL) ? strtol(v, NULL, 10) : 0;
   if (config_emulator_override("Telnetd", &unused) < 0)
      return 0;
   return (port > 0 && port <= 65535) ? port : 0;
}

bool telnetd_enabled(void)
{
   return telnetd_port_cfg() != 0;
}

void telnetd_init(void)
{
   long port = telnetd_port_cfg();
   struct tcp_pcb *l;

   if (g_listener != NULL) {
      tcp_arg(g_listener, NULL);
      tcp_accept(g_listener, NULL);
      tcp_close(g_listener);
      g_listener = NULL;
   }
   g_port = 0u;
   if (port <= 0 || port > 65535)
      return;
   if (serial_redirect_address() == 0u)      /* Serial_addr=-1: nothing to attach to, and no poll */
      return;

   g_ansi = cfg_flag("telnetd_ansi", true);
   g_utf8 = cfg_flag("telnetd_utf8", true);
   g_copy = cfg_flag("telnetd_copycursor", true);
   g_tek = cfg_flag("telnetd_tek", true);
   g_debug = cfg_flag("telnetd_debug", false);
   tnd_out_reset(&g_out, g_utf8);
   {
      const char *m = config_get("telnetd_mode");
      long mode = (m != NULL && m[0] != '\0') ? strtol(m, NULL, 10) : 7;
      g_mode = (uint8_t)((mode >= 0 && mode <= 7) ? mode : 7);
      tnd_out_assume_mode(&g_out, g_mode);
      g_mc_mode = g_out.mode;                   /* the MODE changes count from here */
   }

   l = tcp_new_ip_type(IPADDR_TYPE_V4);
   if (l == NULL)
      return;
   if (tcp_bind(l, IP_ADDR_ANY, (u16_t)port) != ERR_OK) {
      tcp_close(l);
      return;
   }
   g_listener = tcp_listen_with_backlog(l, 2u);
   if (g_listener == NULL) {
      tcp_close(l);
      return;
   }
   tcp_arg(g_listener, NULL);
   tcp_accept(g_listener, td_accept);
   g_port = (uint16_t)port;
}

void telnetd_status(char *buf, size_t len)
{
   size_t used;

   if (g_port == 0u) {
      snprintf(buf, len, "off");
      return;
   }
   /* The diagnostics (telnetd_debug=1 in Pi1MHz.cfg): hooks and resets seen, the model's MODE changes
      with what led up to the last, the keys waiting and when one was last taken, the TCP state. */
   if (g_debug) {
      used = (size_t)snprintf(buf, len, "hooks %lu, resets %lu; ",
                              (unsigned long)serial_redirect_mode_gen(), (unsigned long)serial_redirect_reset_gen());
      if (used >= len)
         return;
      buf += used;
      len -= used;
      if (g_mc_count != 0u) {
         char hex[MC_RING * 3u + 1u];
         for (unsigned i = 0u; i < MC_RING; i++)
            snprintf(hex + 3u * i, 4u, "%02x ", g_mc_last[i]);
         used = (size_t)snprintf(buf, len, "MODE changes %lu, last %u to %u by %s %lu s ago after %s; ",
                                 (unsigned long)g_mc_count, (unsigned)g_mc_from, (unsigned)g_mc_mode,
                                 g_mc_how == 'v' ? "VDU 22" : "hook", (unsigned long)((Pi1MHz_now_us - g_mc_us) / 1000000u), hex);
         if (used >= len)
            return;
         buf += used;
         len -= used;
      }
   }
   if (serial_redirect_lit_ok()) {              /* ROM build 13: the keys waiting, and a Beeb not taking them */
      uint32_t w = keys_waiting();
      bool     hung = w != 0u && (Pi1MHz_now_us - g_taken_us) >= KEYS_HUNG_US;
      used = 0u;
      if (g_debug)
         used = (size_t)snprintf(buf, len, "keys waiting %lu, last taken %lu s ago%s; ", (unsigned long)w,
                                 (unsigned long)((Pi1MHz_now_us - g_taken_us) / 1000000u), hung ? " - BBC NOT TAKING KEYS" : "");
      else if (hung)                            /* otherwise only when it matters */
         used = (size_t)snprintf(buf, len, "BBC NOT TAKING KEYS (%lu waiting); ", (unsigned long)w);
      if (used >= len)
         return;
      buf += used;
      len -= used;
   }
   if (g_pcb != NULL)
      used = (size_t)snprintf(buf, len, "port %u, %s from %s, %lu sessions, %lu refused, %lu replaced, %lu dropped, last end: %s",
                              (unsigned int)g_port, g_stage == S_LIVE ? "live" : "login", g_peer,
                              (unsigned long)g_sessions, (unsigned long)g_refused,
                              (unsigned long)g_replaced, (unsigned long)g_dropped, g_why);
   else
      used = (size_t)snprintf(buf, len, "port %u, listening, %lu sessions, %lu refused, %lu replaced, %lu dropped, last end: %s",
                              (unsigned int)g_port, (unsigned long)g_sessions, (unsigned long)g_refused,
                              (unsigned long)g_replaced, (unsigned long)g_dropped, g_why);
   if (g_debug && used < len) {
      buf += used;
      len -= used;
      if (g_pcb != NULL)
         snprintf(buf, len, " (%d); ring %lu, rx %s, rcvwnd %u, tcp state %d, sndq %u", g_why_err,
                  (unsigned long)(g_ktail - g_khead), g_rx_blocked ? "refused" : "ok",
                  (unsigned int)g_pcb->rcv_wnd, (int)g_pcb->state, (unsigned int)g_pcb->snd_queuelen);
      else
         snprintf(buf, len, " (%d)", g_why_err);
   }
}

bool telnetd_debug(void)
{
   return g_debug;
}

/* The TCP side, for /status (telnetd_debug=1): tcp_write refused for memory, output waiting with no send
   room, and the session's state when one was last dropped as stalled. */
void telnetd_tcp_status(char *buf, size_t len)
{
   snprintf(buf, len, "wmem %lu, room0 %lu; last drop: sndq %u sndbuf %u wnd %u obuf %lu rxblk %d; now: obuf %lu sndq %u sndbuf %u",
            (unsigned long)g_wmem, (unsigned long)g_room0, g_drop_sndq, g_drop_sndbuf, g_drop_wnd,
            g_drop_obuf, g_drop_rxblk, (unsigned long)(g_olen - g_opos),
            g_pcb != NULL ? (unsigned int)g_pcb->snd_queuelen : 0u,
            g_pcb != NULL ? (unsigned int)tcp_sndbuf(g_pcb) : 0u);
}
