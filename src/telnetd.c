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
  unless telnetd_login=0 opens the daemon to the whole network.  The rules
  (tnd_login_*, telnetd_proto.h): wrong passwords counted per address, every third
  locking that address out for longer each time, every one holding all login
  prompts for a moment.

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
#include "lwip/priv/tcp_priv.h"   /* tcp_rexmit_rto: the loss probe (tcp_probe) */

#include "Pi1MHz.h"
#include "config.h"
#include "lib/textparse.h"
#include "wifi/lwip_util.h"
#include "serial_redirect.h"
#include "telnetd.h"
#include "telnetd_proto.h"
#include "net_telnet.h"           /* telnet_escape */
#include "wifi/wifi.h"

#define KRING_SIZE   4096u               /* power of two */
#define KRING_MASK   (KRING_SIZE - 1u)
#define OBUF_SIZE    4096u
/* Output is sent in batches: once OUT_BATCH bytes are waiting or the oldest has waited OUT_HOLD_US.
   A segment per main-loop pass, a few bytes each, is a Wi-Fi frame each, and the frames, not the
   bytes, are what the link runs out of (a stream from the Beeb reached the client at ~350 bytes/s). */
#define OUT_BATCH    1024u
#define OUT_HOLD_US  8000u
#define OUT_HOLD_BUSY_US 50000u          /* ... and the longest wait for a whole segment while much is in flight */
#define HOOK_SAME_BOOT_US 30000000u     /* hooks within this of each other are one boot (the ROM, then HELPERS) */
#define LOGIN_MAX    64u                 /* as the web interface's user name and password may be */
#define LOGIN_US     30000000u           /* a session must be logged in by then */
#define FIN_QUIET_US 1000000u            /* client closed: the Beeb's answer to the last key, this long */
#define FIN_STUCK_US 10000000u           /* client closed: nothing moved this long, the rest is dropped */
#define BEEB_PER_POLL 256u               /* Beeb bytes translated per main-loop pass */
#define KEEPALIVE_US 30000000u           /* a telnet NOP this often when idle, see telnetd_poll */
#define STALE_US     10000000u           /* a session that has not moved for this long, with
                                            something waiting on the client, may be replaced
                                            by a new connection from the same address */
#define ABORT_US     30000000u           /* ...and is dropped outright after this long */

/* The session's stage.  CONNECTING and LIVE are logged in (see logged_in); CLOSING is a login that
   failed for good (the third wrong password, the time limit): the session closes once its last
   words have gone. */
enum { S_LOGIN_USER, S_LOGIN_PASS, S_CONNECTING, S_LIVE, S_CLOSING };

static struct tcp_pcb *g_listener;
static struct tcp_pcb *g_pcb;            /* the one session */
static uint16_t        g_port;
static bool            g_ansi = true;          /* this session: ANSI translation (telnetd_ansi, and not teletype mode) */
static bool            g_ansi_cfg = true;      /* telnetd_ansi */
static struct tcp_pcb *g_raw_listener;         /* telnetd_raw_port: sessions there are teletype mode from the start */
static uint16_t        g_raw_port;
static bool            g_sess_rawport;         /* this session came on the raw port: no telnet at all */
static bool            g_tek = true;          /* telnetd_tek: graphics to an xterm's Tek window */
static bool            g_debug;               /* telnetd_debug=1: the diagnostics on the status page */
static bool            g_utf8 = true;
static bool            g_copy = true;         /* show the copy cursor */

/* With the ROM (serial_redirect_beeb->rom) the Beeb tells each cursor key, COPY or RETURN a program takes from its keyboard
   buffer (serial_redirect_beeb->key_gen): the ones sent wait here and move the copy cursor only when taken,
   in order - so a key that only waits in the buffer (a listing in paged mode waiting for SHIFT)
   moves nothing, whatever is on the screen. */
#define CKQ_SIZE 32u
static uint8_t  g_ckq[CKQ_SIZE];
static uint32_t g_ckq_head, g_ckq_tail;
static uint32_t g_kgen_seen;                  /* the reports already used */

/* The ROM tells every byte a program takes (serial_redirect_beeb->key_other_gen), so the Pi knows the keys
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
/* A read of the Beeb's screen to ask for: none, an ordinary one (the terminal drawn from it), or one
   compared with the model, which has been drawn already (only what differs is drawn). */
enum { SCAN_NONE, SCAN_FULL, SCAN_MERGE };
static uint8_t         g_scan;
static bool            g_unseen;              /* the ROM filing system printed where RS423 cannot see: a scan, once it stops */
#define UNSEEN_QUIET_US 100000u
static bool            g_break_asked;         /* F12 was pressed: the next hook follows a BREAK */
static tnd_banner_t    g_ban;                     /* what the Beeb printed just after the helper hooked in */
static uint32_t        g_banner_until;            /* banners are read until then */

static uint8_t   g_stage;
static uint32_t  g_start_us;

static bool logged_in(void)
{
   return g_stage == S_CONNECTING || g_stage == S_LIVE;
}

/* The client's window size (NAWS, asked for in the greeting).  The screen is not drawn until the
   first size has come, or NAWS_WAIT_US has gone by (a client that says nothing): a window smaller
   than the Beeb's mode is told so before the screen, as one line.  A later size (a resize) has
   the terminal drawn again from the model, since a console that shrinks moves its rows. */
#define NAWS_WAIT_US 500000u
static uint8_t  g_win_gen;                      /* the last size acted on (tnd_in_t's win_gen) */
static uint8_t  g_mode_gen;                     /* the client's last line or character mode acted on (tnd_in_t's mode_gen) */
static bool     g_win_first;                    /* the first size has been acted on */

/* A new session (once logged in) is "connecting" for a moment: under the boot banners the line
   "Pi1MHz telnet - connecting" gets a dot every CONNECT_DOT_US, for at least CONNECT_MIN_US and
   until the client's window size and terminal type are in; the Beeb's output waits meanwhile.
   Then the terminal is cleared and the Beeb's screen drawn in place, row by row, so its rows are
   the Beeb's from the start (nothing jumps when the Beeb next scrolls or clears). */
#define CONNECT_DOT_US   300000u
#define CONNECT_MIN_US  1000000u
#define CONNECT_WARN_US 3000000u                 /* how long a too-small window's warning stays */
static uint32_t g_conn_us;                       /* when it started */
static uint32_t g_dot_us;                        /* the last dot */
static uint32_t g_warn_until;                    /* a warning is being shown until then */
static bool     g_warned;
static uint32_t  g_tx_us;                /* the last time bytes went to the client */
static char      g_line[LOGIN_MAX + 1u];
static char      g_user[LOGIN_MAX + 1u];
static unsigned  g_len;
static tnd_login_t g_login;              /* wrong passwords per address, lockouts, the hold: kept across sessions */
static uint32_t  g_peer_addr;            /* the session's address, for those */
static uint32_t  g_login_input_us;       /* when the session at the login prompt last had a key */
static bool      g_login_cr;             /* ... and that key was CR (a LF after it is the same RETURN) */
static uint8_t   g_hold[2u * LOGIN_MAX + 2u];   /* login keys typed during the hold, taken after it */
static unsigned  g_hold_n;
static bool      g_fin;                  /* the client closed its side: the session ends once its keys are in */
static uint32_t  g_fin_us;               /* ... and when the keys or the output last moved */
static bool      g_bind_failed;          /* telnetd_port is set but could not be listened on */
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
static bool      g_esc_cut;              /* an ESCAPE or BREAK is waiting behind the backlog: the keys before it go */
static bool      g_esc_wait;             /* an ESCAPE key is being decided on the Beeb (serial_redirect_escape_key):
                                            the keys behind it wait in the ring */
static uint32_t  g_esc_wait_us;
static uint32_t  g_esc_pos;              /* ... where in the ring it came (g_ktail then): a character goes there */
static uint8_t   g_esc_more;             /* ESCAPE keys that came while one was being decided */
#define ESC_WAIT_US  1500000u            /* no answer in this long: carry on (the Pi gives up at 0.4 s) */
static uint32_t  g_recv_us;              /* when client data last came */
/* Client data taken from lwIP but not yet turned into keys, copied out of its pbufs as it comes.
   Holding the pbufs themselves held lwIP's receive pool: the window is 64 KB, but a client sending
   small segments (a stream to TERM, a fast paste) pins a pool pbuf for each, and the 96 ran out -
   the driver could then receive nothing, not even the ACKs for our output, which retransmitted
   until lwIP gave up, and keys typed meanwhile never came in.  The window is still opened only for
   what has been read (tcp_recved), and it is smaller than this buffer, so the client cannot overrun it. */
#define INBUF_SIZE 65536u                /* power of two, more than TCP_WND */
static uint8_t   g_inbuf[INBUF_SIZE];
static uint32_t  g_in_head, g_in_tail;   /* read, write */
static uint32_t  g_in_drops;             /* bytes that did not fit (should never be; telnetd_debug) */

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

/* All of it or none (PR review): a piece cut short by a full buffer could end in the middle of an
   escape sequence and leave the terminal in a wrong state.  False if it did not fit. */
static bool o_put(const void *p, size_t n)
{
   if (n > o_room())
      return false;
   if (g_olen == g_opos && n != 0u)
      g_obuf_us = Pi1MHz_now_us;
   o_compact();
   memcpy(g_obuf + g_olen, p, n);
   g_olen += n;
   return true;
}

static bool o_str(const char *s)
{
   return o_put(s, strlen(s));
}

/* The Pi's name in the greeting: its network hostname (wifi_hostname), as the web pages show it, so
   several Pi1MHz on one network can be told apart; "Pi1MHz" when none is set. */
static const char *td_name(void)
{
   const wifi_config_t *c = wifi_get_config();
   return (c != NULL && c->hostname[0] != '\0') ? c->hostname : "Pi1MHz";
}

/* The byte the Pi puts in the Beeb's keyboard buffer to ask for the screen: the auto-start
   ROM takes it out of REMV before any program sees it. */
#define SCAN_KEY 0xFFu


static uint32_t g_scan_keys;                       /* scan keys queued and not yet told taken: not silent keys */

/* A byte for the Beeb's keyboard buffer.  literal: the byte after LIT_KEY, a character whatever its
   value (&FF there is a typed y-diaeresis, not the Pi's own SCAN_KEY). */
static void k_put(uint8_t k, bool literal)
{
   if (literal || k != SCAN_KEY)
      g_banner_until = Pi1MHz_now_us;              /* somebody is typing: the start-up is over */
   else
      g_scan_keys++;
   if (g_ktail - g_khead < KRING_SIZE) {
      g_kring[g_ktail & KRING_MASK] = k;
      g_ktail++;
   }
}

static void k_push(uint8_t k)
{
   k_put(k, false);
}

/* ---- the session -------------------------------------------------------- */

/* Telnet has its own user name and password (telnetd_user, telnetd_password), never the web
   interface's: telnet sends them in the clear.  Without them a session is refused, unless
   telnetd_login=0 says the daemon is to be open to the network. */
static bool login_open(void)
{
   return !config_get_flag("telnetd_login", true);
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
   g_in_head = g_in_tail;
}

static void session_reset(void)
{
   g_login_cr = false;
   g_hold_n = 0u;
   g_fin = false;
   pend_free();
   g_khead = g_ktail = 0u;
   g_olen = g_opos = 0u;
   g_len = 0u;
   g_att_known = false;
   g_rx_blocked = false;
   g_stage = S_LIVE;
   /* and nothing of the last session's keys carries over (PR review): a byte after LIT_KEY still
      to come, a key being watched for a silent screen, a scan key counted in the ring just emptied */
   g_lit_next = false;
   g_esc_cut = false;
   g_esc_wait = false;
   g_esc_more = 0u;
   g_silent_wait = false;
   g_scan_keys = 0u;
   g_scan = SCAN_NONE;
   memset(g_user, 0, sizeof g_user);             /* the login typed, not kept after the session */
   memset(g_line, 0, sizeof g_line);
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
   g_stage = S_CONNECTING;
   g_conn_us = g_dot_us = Pi1MHz_now_us;
   g_warn_until = 0u;
   g_warned = false;
   o_str("\r\n");
   o_str(td_name());
   o_str(" telnet - connecting");
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
   if (tnd_login_held(&g_login)) {               /* after a wrong password, on any connection: kept a while, */
      if (g_hold_n < sizeof g_hold)              /* not dropped (a user name typed straight after "Login */
         g_hold[g_hold_n++] = k;                 /* incorrect" would lose letters and fail again) */
      return;
   }
   g_login_input_us = Pi1MHz_now_us;
   if (k == 10u) {                               /* LF ends the line too (nc on the raw port sends it */
      bool after_cr = g_login_cr;                /* alone); straight after a CR it is the same RETURN */
      g_login_cr = false;
      if (after_cr)
         return;
      k = 13u;
   } else {
      g_login_cr = (k == 13u);
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
      if (credentials_ok()) {
         tnd_login_ok(&g_login, g_peer_addr);
         g_att_known = false;
         o_str("\r\n");
         send_banners();                         /* only now: nothing of the Beeb's before a login */
         connect_start();
      } else {
         if (tnd_login_fail(&g_login, g_peer_addr)) {   /* the third from this address: it is locked out */
            o_str("\r\nLogin incorrect\r\n");
            g_stage = S_CLOSING;
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
   uint32_t taken = serial_redirect_beeb->key_gen + serial_redirect_beeb->key_other_gen;
   uint32_t w = serial_redirect_rx_count() - taken - g_keys_base;
   if (w > 0x80000000u) {
      /* More taken than put in since the buffer was last counted empty: the ESCAPE that set the
         base had not emptied the Beeb's buffer yet (the program empties it when it acknowledges
         the ESCAPE, and can take a key before that).  Counted again from here, or every count
         after would be that many short (T-17's "3 waiting" read 2 after an earlier ESCAPE). */
      g_keys_base = serial_redirect_rx_count() - taken;
      return 0u;
   }
   return w;
}

static void keys_emptied(void)                   /* the Beeb's buffer emptied (ESCAPE, a reset) */
{
   g_scan_keys = 0u;                             /* a scan key in it went too */
   g_keys_base = serial_redirect_rx_count() - serial_redirect_beeb->key_gen - serial_redirect_beeb->key_other_gen;
}

static void deliver_key(uint8_t k)
{
   if (g_lit_next) {                             /* the byte after TND_KEY_LIT: any value, as it is */
      g_lit_next = false;
      if (logged_in() && serial_redirect_beeb->rom) {
         k_push(LIT_KEY);
         k_put(k, true);
      }
      return;
   }
   if (k == TND_KEY_LIT) {
      g_lit_next = true;
      return;
   }
   if (!logged_in()) {
      if (k != TND_KEY_ESCAPE && k != TND_KEY_BREAK && k != TND_KEY_REFRESH && k != TND_KEY_SHIFT
          && k != TND_KEY_SHIFTBREAK && k != TND_KEY_CTRLBREAK)
         login_key(k);
   } else if (k == TND_KEY_BREAK || k == TND_KEY_SHIFTBREAK || k == TND_KEY_CTRLBREAK) {
      g_khead = g_ktail;                           /* what was typed ahead goes, as at a BREAK */
      g_ckq_head = g_ckq_tail;
      g_break_asked = true;                        /* the next hook is the start of a new boot */
      g_esc_wait = false;
      serial_redirect_break(k == TND_KEY_SHIFTBREAK ? SER_BRK_SHIFT : k == TND_KEY_CTRLBREAK ? SER_BRK_CTRL : SER_BRK_PLAIN);
   } else if (k == TND_KEY_REFRESH) {
      if (serial_redirect_beeb->rom)
         k_push(SCAN_KEY);                            /* the ROM keeps it from the program and sends the screen */
   } else if (k == TND_KEY_SHIFT) {
      if (serial_redirect_beeb->rom)
         serial_redirect_shift();                     /* a trip, not a key: nothing queued can hold it up */
   } else if (k == TND_KEY_ESCAPE) {
      /* As the keyboard's ESCAPE key: the Beeb's settings decide (escape_settled) */
      if (!g_esc_wait) {
         serial_redirect_escape_key();
         g_esc_wait = true;
         g_esc_wait_us = Pi1MHz_now_us;
         g_esc_pos = g_ktail;
      } else if (g_esc_more < 255u) {
         g_esc_more++;
      }
   } else {
      k_push(k);
      if (g_copy && g_ansi && ((k >= 139u && k <= 143u) || k == 13u)) {
         if (serial_redirect_beeb->rom) {         /* moved when the Beeb takes it (see g_ckq) */
            if (g_ckq_tail - g_ckq_head < CKQ_SIZE)
               g_ckq[g_ckq_tail++ % CKQ_SIZE] = k;
         } else {
            tnd_out_copykey(&g_out, k);            /* cursor keys, COPY, RETURN: the copy cursor */
         }
      }
   }
}

/* How the Beeb took an ESCAPE key (serial_redirect_escape_key): as ESCAPE the keys behind it go with
   the Beeb's buffer, as at the keyboard; as a character (*FX229, or another escape character) it goes
   into the buffer ahead of the keys typed after it; ignored (*FX200) or an escape event: nothing. */
static void escape_settled(uint8_t r)
{
   g_esc_wait = false;
   if (r == SER_ESC_RAISED) {
      g_khead = g_ktail;
      g_ckq_head = g_ckq_tail;                      /* the escape flushed the Beeb's buffer too */
      keys_emptied();
      g_silent_wait = true;                      /* and a screen the ESCAPE changed without printing is read */
      g_silent_us = Pi1MHz_now_us;
      g_silent_tx = g_out.drawn;
      if (g_copy && g_ansi)
         tnd_out_copykey(&g_out, 27u);
      g_esc_more = 0u;
   } else if (r == SER_ESC_CHAR && g_ktail - g_khead < KRING_SIZE) {
      /* in order: after the keys typed before the ESC (still in the ring: they wait while it is
         decided), before the ones typed after it */
      if (g_esc_pos - g_khead > g_ktail - g_khead)   /* the ring emptied meanwhile */
         g_esc_pos = g_khead;
      for (uint32_t i = g_ktail; i != g_esc_pos; i--)
         g_kring[i & KRING_MASK] = g_kring[(i - 1u) & KRING_MASK];
      g_kring[g_esc_pos & KRING_MASK] = 27u;
      g_ktail++;
   }
   if (r != SER_ESC_RAISED && g_esc_more != 0u) {   /* the next ESCAPE key, where it came */
      g_esc_more--;
      serial_redirect_escape_key();
      g_esc_wait = true;
      g_esc_wait_us = Pi1MHz_now_us;
      g_esc_pos = g_ktail;
   }
}

/* A key from the client.  While an ESCAPE or BREAK is cut through the backlog (pend_consume)
   the keys before it are thrown away; it is delivered itself, which empties the ring and the
   Beeb's buffer. */
static void take_key(uint8_t k)
{
   if (g_esc_cut) {
      if (k != TND_KEY_ESCAPE && k != TND_KEY_BREAK && k != TND_KEY_SHIFTBREAK && k != TND_KEY_CTRLBREAK)
         return;
      g_esc_cut = false;
      g_lit_next = false;
   }
   deliver_key(k);
}

static void client_byte(uint8_t c, uint32_t now_us)
{
   uint8_t keys[TND_IN_KEYS], rep[TND_IN_REPLY];
   size_t  rl;
   size_t  n = tnd_in_byte(&g_in, c, now_us, keys, rep, &rl);

   if (rl != 0u)
      o_put(rep, rl);
   for (size_t i = 0u; i < n; i++)
      take_key(keys[i]);
}

/* Is a lone ESC (the ESCAPE key, not the start of a cursor key's ESC [ or ESC O) waiting in the
   client data not yet read?  ESCAPE is meant to cut through type-ahead, but what is still in TCP
   waits behind a full key ring: a client sending faster than the Beeb takes keys (a paste, a
   stream to TERM) had its ESC held for as long as the backlog, and lost when it left.  The scan
   (tnd_in_ahead_*) skips telnet commands; an ESC that ends the data counts once nothing more has
   come for 300 ms.  The same for BREAK (F12 and its SHIFT and CTRL forms, or IAC BRK) and IAC
   ABORT (ESCAPE), and IAC IP, a line-mode client's ESC: a Beeb stuck in a program that reads no
   keys has a full ring, and they are how it is got back. */
static bool esc_ahead(uint32_t now_us)
{
   tnd_ahead_t a;

   tnd_in_ahead_start(&g_in, &a);
   for (uint32_t i = g_in_head; i != g_in_tail; i++)
      if (tnd_in_ahead_byte(&a, g_inbuf[i & (INBUF_SIZE - 1u)]))
         return true;
   return tnd_in_ahead_esc_end(&a) && (now_us - g_recv_us) > 300000u;
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
/* Diagnostics (telnetd_debug): a printable byte the Beeb printed that sent the client nothing,
   with the model's state, and the bytes followed with nobody shown them while a client is on. */
#define SIL_RING 12u
static struct { uint8_t c, code, sm, fl, x, y; } g_sil[SIL_RING];
static uint32_t g_sil_n, g_follow_on, g_oput_fail;

static void sil_note(uint8_t c)
{
   unsigned i = g_sil_n++ % SIL_RING;
   g_sil[i].c = c;
   g_sil[i].code = g_out.code;
   g_sil[i].sm = g_out.sm_st;
   g_sil[i].fl = (uint8_t)((g_out.disabled ? 1u : 0u) | (g_out.smerge ? 2u : 0u) | (g_out.tele ? 4u : 0u)
                           | (g_out.sync ? 8u : 0u) | (g_out.origin_ok ? 16u : 0u) | (g_out.tek_on ? 32u : 0u)
                           | (g_out.dirty ? 64u : 0u));
   g_sil[i].x = g_out.cx;
   g_sil[i].y = g_out.cy;
}

/* What the Beeb printed since its last line end: its prompt and what has been typed after it, over
   as many rows as it wraps (the Beeb wraps without a line end) - what a teletype is shown of the
   line when it joins, or when the client changes between line and character mode.  DELETE's echo
   takes a character off; a cursor move (copy editing) or a line longer than the buffer makes it
   no longer the line (g_tline_ok false: the model's cursor row is used).  A VDU code's parameters
   are not text. */
#define TLINE_MAX 320u                          /* an input line (255) and its prompt */
static uint8_t  g_tline[TLINE_MAX];
static uint16_t g_tlen;
static bool     g_tline_ok;                     /* nothing seen yet: not known */

static void tline_byte(uint8_t c)
{
   if (g_out.code != 0u)
      return;
   if (c >= 32u && c != 127u && c != 0xFFu) {
      if (g_tlen < TLINE_MAX)
         g_tline[g_tlen++] = c;
      else
         g_tline_ok = false;
   } else if (c == 127u) {                      /* DELETE */
      if (g_tlen != 0u)
         g_tlen--;
      else
         g_tline_ok = false;
   } else if (c == 10u || c == 13u || c == 12u || c == 22u) {
      g_tlen = 0u;                              /* a line end, CLS, MODE: a new line */
      g_tline_ok = true;
   } else if (c == 8u || c == 9u || c == 11u || c == 30u || c == 31u) {
      g_tline_ok = false;                       /* the cursor moved (copy editing, home, TAB(x,y) - a screen
                                                   read is a stream of them): the model's row is the line */
   }
}

/* A teletype is shown the Beeb's line (usually its prompt, and what has been typed), so it does not
   look dead; no key is sent to the Beeb for it. */
static void o_beeb_line(void)
{
   char cl[TND_COLS_MAX + 1u];

   if (g_tline_ok)
      o_put(g_tline, g_tlen);
   else if (tnd_out_cursor_line(&g_out, cl, sizeof cl) != 0u)
      o_str(cl);
}

static void beeb_follow(uint8_t c)
{
   tline_byte(c);
   if (g_debug && g_pcb != NULL)
      g_follow_on++;
   banner_byte(c);
   if (g_ansi) {
      uint8_t d[TND_OUT_MAX];
      (void)tnd_out_byte(&g_out, c, d);
      tnd_out_touch(&g_out, Pi1MHz_now_us);
   }
}

static void beeb_byte(uint8_t c)
{
   tline_byte(c);
   banner_byte(c);
   if (g_ansi) {
      uint8_t d[TND_OUT_MAX];
      uint8_t pre = 0u;
      if (g_out.parked) {                        /* the terminal's cursor is on the copy cursor: back first */
         uint8_t u[TND_UNPARK_MAX];
         size_t  un = tnd_out_unpark(&g_out, u);
         if (un != 0u)
            o_put(u, un);
      }
      if (g_debug && c >= 32u && c < 127u && g_out.code == 0u && !g_out.disabled && !g_out.smerge)
         pre = 1u;                              /* a character to print, not a parameter or a read */
      size_t  n = tnd_out_byte(&g_out, c, d);
      tnd_out_touch(&g_out, Pi1MHz_now_us);
      if (n != 0u && !o_put(d, n) && g_debug)
         g_oput_fail++;
      if (pre && n == 0u)
         sil_note(c);
   } else {
      if (g_ansi_cfg) {                         /* teletype mode: the model follows quietly, for a later terminal */
         uint8_t d[TND_OUT_MAX];
         (void)tnd_out_byte(&g_out, c, d);
         tnd_out_touch(&g_out, Pi1MHz_now_us);
      }
      if (g_sess_rawport) {                     /* the raw port: no telnet there, so no escaping */
         o_put(&c, 1u);
      } else {                                  /* IAC must be doubled */
         uint8_t esc[2];
         size_t  used;
         o_put(esc, telnet_escape(&c, 1u, esc, sizeof esc, &used));
      }
   }
}

/* ---- lwIP callbacks ------------------------------------------------------- */

/* Turn waiting client data into keys while the ring has room, and tell lwIP how
   much has been read.  Called when data arrives and from the poll. */
static void pend_consume(void)
{
   uint32_t now = Pi1MHz_now_us;
   u32_t    n = 0u;

again:
   while (g_in_head != g_in_tail && g_pcb != NULL) {
      if (!g_esc_cut && KRING_SIZE - (g_ktail - g_khead) < TND_IN_KEYS)
         goto full;                             /* a byte releases at most TND_IN_KEYS keys */
      client_byte(g_inbuf[g_in_head++ & (INBUF_SIZE - 1u)], now);
      n++;
   }
   g_rx_blocked = false;
   if (n != 0u && g_pcb != NULL)
      tcp_recved(g_pcb, (u16_t)(n > 0xFFFFu ? 0xFFFFu : n));
   if (n != 0u)
      g_prog_us = now;
   return;

full:
   if (logged_in() && esc_ahead(now)) {   /* ESCAPE or BREAK behind the backlog: cut through to it */
      g_esc_cut = true;
      goto again;
   }
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
   if (p == NULL) {                             /* the client closed its side ... */
      g_why = "client closed";
      if (!logged_in() || (g_in_head == g_in_tail && g_khead == g_ktail))
         return session_close(false) ? ERR_ABRT : ERR_OK;
      g_fin = true;                             /* ... after sending more than the Beeb has taken yet */
      g_fin_us = Pi1MHz_now_us;                 /* (cat prog | nc): it is typed in, then the session ends */
      return ERR_OK;
   }

   /* The data is ours now: copied out (see g_inbuf) and the pbufs given back at once.  It is read
      into the key ring as fast as the Beeb empties it and the window stays closed (tcp_recved) for
      what has not been read, which is what holds the client back.  (Refusing the packet with
      ERR_MEM instead does not work: lwIP can hand over several queued segments as one chain.) */
   g_recv_us = Pi1MHz_now_us;
   if (p->tot_len <= INBUF_SIZE - (g_in_tail - g_in_head))
      g_in_tail = byte_ring_put_pbuf(g_inbuf, INBUF_SIZE - 1u, g_in_tail, p);
   else
      g_in_drops += p->tot_len;
   pbuf_free(p);
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
   bool rawport = (arg != NULL);                /* the raw port's listener carries a marker */

   if (err != ERR_OK || newpcb == NULL)
      return ERR_VAL;

   uint32_t addr = ip4_addr_get_u32(ip_2_ip4(&newpcb->remote_ip));
   bool     locked = tnd_login_locked(&g_login, addr);
   /* The one session is a dead one, and this is the same machine calling again
      (it gave up on the old connection, as a client that crashed or was killed
      does): the old one goes.  Only when it has really stopped moving - a session
      that is merely quiet or slow is not replaced. */
   if (g_pcb != NULL && !locked && ip_addr_cmp(&g_pcb->remote_ip, &newpcb->remote_ip)
       && tnd_stalled(session_pending(), Pi1MHz_now_us, g_prog_us, STALE_US)) {
      g_replaced++;
      g_why = "replaced";
      (void)session_close(true);
   }
   /* A session still at the login prompt holds nothing yet: a new connection from the same address
      takes its place, or from another once nothing has been typed there for a while, so nobody can
      keep the owner out by connecting and waiting, nor by reconnecting over and over. */
   if (g_pcb != NULL && !locked && !logged_in()
       && tnd_login_may_replace(g_peer_addr, addr, Pi1MHz_now_us - g_login_input_us)) {
      g_replaced++;
      g_why = "replaced at login";
      (void)session_close(true);
   }
   if (g_pcb != NULL || locked || (!login_open() && !login_configured())) {
      static const char busy[]   = "Pi1MHz: the BBC is in use by another session.\r\n";
      static const char locked_msg[] = "Pi1MHz: too many wrong passwords from here, try later.\r\n";
      static const char unset[]  = "Pi1MHz: telnet has no login set up (telnetd_user and telnetd_password).\r\n";
      const char *m = locked ? locked_msg : g_pcb != NULL ? busy : unset;
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
   serial_redirect_drop_keys(LIT_KEY);          /* nothing the last client typed ahead is typed into this one */
   g_start_us = g_tx_us = g_prog_us = Pi1MHz_now_us;
   snprintf(g_peer, sizeof g_peer, "%s", ip4addr_ntoa(ip_2_ip4(&newpcb->remote_ip)));
   g_peer_addr = addr;
   g_login_input_us = Pi1MHz_now_us;
   tcp_nagle_disable(newpcb);                   /* keys and echoes must not wait */
   tcp_arg(newpcb, NULL);
   tcp_recv(newpcb, td_recv);
   tcp_sent(newpcb, td_sent);
   tcp_err(newpcb, td_err);

   tnd_in_reset(&g_in);
   g_sess_rawport = rawport;
   g_in.raw = rawport;                          /* teletype mode from the start on the raw port ... */
   g_in.notelnet = rawport;                     /* ... and no telnet at all there */
   g_ansi = g_ansi_cfg && !rawport;
   g_ckq_head = g_ckq_tail;                     /* a new terminal: no copy keys waiting */
   g_kgen_seen = serial_redirect_beeb->key_gen;
   g_win_gen = 0u;
   g_mode_gen = 0u;
   g_win_first = false;
   tnd_out_session(&g_out);                     /* the model has been following the Beeb all along */
   tnd_out_tek(&g_out, false);                  /* until this terminal says it is xterm */
   /* The ROM is asked for the Beeb's screen (below).  A model that knows nothing is drawn from it; one
      that does is drawn first, with its colours, and the read is only compared with it: a program can
      have changed the screen where the Pi could not see (output with the RS423 copy off, writes to
      screen memory), and then the terminal is drawn again from the Beeb's characters. */
   g_scan = g_out.sync && !tnd_out_blank(&g_out) ? SCAN_MERGE : SCAN_FULL;
   if (!rawport) {                              /* the raw port: no negotiation, nothing but text */
      uint8_t g[TND_GREETING_LEN];
      o_put(g, tnd_greeting(g));
   }
   if (g_ansi)
      o_str("\x1b[0m");
   if (!login_open()) {                         /* the banners come after the login */
      g_stage = S_LOGIN_USER;
      o_str("\r\n");
      o_str(td_name());
      o_str(" telnet\r\n\r\nlogin: ");
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
   /* While much is in flight, whole segments: lwIP queues at most TCP_SND_QUEUELEN segments
      whatever their size, and a Beeb printing fast gave one of ~80 bytes every OUT_HOLD_US, so
      with ACKs slow (WiFi, a busy client) the queue filled long before the send buffer, every
      write after was ERR_MEM, and the output crawled at a few hundred bytes a second while the
      Beeb sat in OSWRCH, deaf even to ESCAPE.  With little in flight it is as before, for echoes. */
   size_t   have = g_olen - g_opos;
   uint32_t held = (uint32_t)(Pi1MHz_now_us - g_obuf_us);
   bool     busy = g_pcb->snd_queuelen >= TCP_SND_QUEUELEN / 4u;
   bool     go = busy ? (have >= TCP_MSS || have >= OBUF_SIZE / 2u || held >= OUT_HOLD_BUSY_US)
                      : (have >= OUT_BATCH || held >= OUT_HOLD_US);
   if (g_opos < g_olen && (go || g_stage == S_CLOSING)) {
      bool  was_idle = g_pcb->snd_queuelen == 0u;
      u16_t sent = 0u;
      err_t e = lwip_tcp_write_some(g_pcb, g_obuf + g_opos, g_olen - g_opos, &sent);
      if (e == ERR_OK && sent != 0u) {
         g_opos += sent;
         g_tx_us = Pi1MHz_now_us;
         if (was_idle)
            g_prog_us = g_tx_us;                 /* the wait for the first ACK starts now */
         (void)tcp_output(g_pcb);
      } else if (e == ERR_OK) {                  /* the send buffer is full */
         g_room0++;
      } else if (e == ERR_MEM) {                 /* ERR_MEM: the heap is busy, try again */
         g_wmem++;
      } else {
         g_why = "write error";
         g_why_err = (int)e;
         (void)session_close(true);
         return;
      }
      if (g_opos == g_olen)
         g_opos = g_olen = 0u;
   }
   if (g_stage == S_CLOSING && g_opos == g_olen) {
      g_why = "login";
      (void)session_close(false);
   }
}

/* Diagnostics (telnetd_debug): retransmissions on the session, and the longest time sent data
   waited for its ACK.  A lost segment on WiFi is resent only when lwIP's timer runs out. */
static uint32_t g_rtx_events, g_rtx_max, g_unack_max_ms, g_unack_since_us;
static bool     g_unack_on;

static void tcp_watch(uint32_t now_us)
{
   static uint8_t last_nrtx;
   if (!g_debug || g_pcb == NULL)
      return;
   if (g_pcb->nrtx > last_nrtx)
      g_rtx_events++;
   if (g_pcb->nrtx > g_rtx_max)
      g_rtx_max = g_pcb->nrtx;
   last_nrtx = g_pcb->nrtx;
   if (g_pcb->unacked != NULL) {
      if (!g_unack_on) {
         g_unack_on = true;
         g_unack_since_us = now_us;
      } else {
         uint32_t ms = (now_us - g_unack_since_us) / 1000u;
         if (ms > g_unack_max_ms)
            g_unack_max_ms = ms;
      }
   } else {
      g_unack_on = false;
   }
}

/* The loss probe.  On WiFi a segment is lost now and then; lwIP resends it only when its
   retransmission timer runs out, in 500 ms ticks from its round-trip estimate and doubled at
   once, so an interactive session (too little traffic for a fast retransmit) froze for 2 to 10
   seconds with everything after the lost segment held by the client's TCP.  So once sent data
   has waited PROBE_FIRST_MS for its ACK (more than a delayed ACK takes), what is unacknowledged
   is sent again, and again after twice as long while the same data waits, up to PROBE_MAX_MS.
   A resend that was not needed costs the client a duplicate it throws away. */
#define PROBE_FIRST_MS 400u
#define PROBE_MAX_MS   3200u
static uint32_t g_probes;                  /* resends made (telnetd_debug) */

static void tcp_probe(uint32_t now_us)
{
   static bool     on;
   static u32_t    ack;
   static uint32_t since_us, wait_ms;

   if (g_pcb == NULL || g_pcb->unacked == NULL || g_pcb->state != ESTABLISHED) {
      on = false;
      return;
   }
   if (!on || g_pcb->lastack != ack) {          /* new data waiting, or an ACK came: start again */
      on = true;
      ack = g_pcb->lastack;
      since_us = now_us;
      wait_ms = PROBE_FIRST_MS;
      return;
   }
   if ((now_us - since_us) / 1000u < wait_ms)
      return;
   tcp_rexmit_rto(g_pcb);                       /* the unacknowledged segments, sent again */
   g_pcb->rtime = 0;                            /* lwIP's own timer starts again from here */
   g_probes++;
   since_us = now_us;
   if (wait_ms < PROBE_MAX_MS)
      wait_ms *= 2u;
}

static void telnetd_poll(uint32_t now_us, bool beeb_attached)
{
   tcp_watch(now_us);
   tcp_probe(now_us);
   static uint32_t mode_seen;
   uint8_t c;

   /* A reset starts everything again: the screen is new, so the model is, and the next few
      seconds hold the boot banners.  It is seen on the Beeb's reset line, or told by the
      auto-start ROM (a software BREAK never touches the line), or asked for by F12, or the
      hook finds the cursor in the corner of the screen. */
   static uint32_t reset_seen;
   bool            hooked = serial_redirect_beeb->mode_gen != mode_seen;
   static uint32_t hook_recent_us, hook_last_poll_us;   /* counted down: no wrap of the 32-bit clock */
   /* A hook that comes with F12 or well after the last one (the ROM and then HELPERS
      hook within seconds of each other in one boot, a disc being slow) follows a reset
      the Pi never saw on the reset line, such as CALL !-4: the Beeb has a new screen. */
   bool            hw_reset = serial_redirect_beeb->reset_gen != reset_seen;
   {
      uint32_t dt = now_us - hook_last_poll_us;
      hook_last_poll_us = now_us;
      hook_recent_us = hook_recent_us > dt ? hook_recent_us - dt : 0u;
   }
   {                                             /* the login's timers: lockouts, failures forgotten, the hold */
      static uint32_t login_tick_us;             /* counted in elapsed time, so ticked every 10 ms, not each pass */
      if ((uint32_t)(now_us - login_tick_us) >= 10000u) {
         login_tick_us = now_us;
         tnd_login_tick(&g_login, now_us);
      }
   }
   bool            reset = hw_reset
                           || (hooked && (g_break_asked || hook_recent_us == 0u));
   if (hooked || hw_reset)
      hook_recent_us = HOOK_SAME_BOOT_US;
   if (hooked && !reset) {
      uint8_t rc, rr;
      rc = serial_redirect_beeb->col;
      rr = serial_redirect_beeb->row;
      reset = (rc == 0u && rr == 0u);
   }
   if (hooked || reset) {
      uint8_t col, row;
      mode_seen = serial_redirect_beeb->mode_gen;
      reset_seen = serial_redirect_beeb->reset_gen;
      keys_emptied();                           /* a reset empties the Beeb's buffer */
      if (hooked)
         g_break_asked = false;
      if (reset || serial_redirect_beeb->mode != g_out.mode)
         tnd_out_assume_mode(&g_out, serial_redirect_beeb->mode);
      if (g_debug && g_out.mode != g_mc_mode)
         mode_changed('h');
      col = serial_redirect_beeb->col;
      row = serial_redirect_beeb->row;
      tnd_out_at(&g_out, col, row);
      if (reset) {
         tnd_out_beeb_reset(&g_out);
         if (g_pcb != NULL && logged_in() && g_ansi) {
            /* Somebody is connected: their terminal is refreshed too, so what the Beeb prints
               from here (the new banners) lands where the model says. */
            char cup[40];
            snprintf(cup, sizeof cup, "\x1b[0m\x1b[?25h\x1b[2J\x1b[%u;%uH", (unsigned)row + 1u, (unsigned)col + 1u);
            o_str(cup);                        /* cleared, and the cursor where the Beeb's is */
            g_out.origin_ok = true;
         }
         tnd_banner_reset(&g_ban);             /* the banners of the last start are no longer true ... */
         g_banner_until = Pi1MHz_now_us + 4000000u;   /* ... and the next four seconds hold the new ones */
      }
   }
   bool    live = g_pcb != NULL && logged_in();

   /* Teletype mode: the raw port, or a client that asked for an 8-bit path (telnet BINARY, either way).
      Then the Beeb's bytes go out as they are and the client's go in as they are: no translation, no
      screen reads, no copy cursor.  Back to a terminal (BINARY off): its screen is read and drawn. */
   if (g_pcb != NULL) {
      bool raw = g_sess_rawport || g_in.bin_in || g_in.bin_out;
      if (raw != g_in.raw) {
         g_in.raw = raw;
         if (!raw && g_ansi_cfg && logged_in()) {
            g_scan = SCAN_FULL;
         }
      }
      g_ansi = g_ansi_cfg && !raw;
   }

   /* What the Beeb printed.  With a client to show it to it is translated, and
      the rest waits in the Beeb's buffer while the output buffer is full;
      with none (or before login) it is read and dropped. */
   if (beeb_attached && g_stage != S_CONNECTING) {   /* held while connecting */
      for (unsigned i = 0u; i < BEEB_PER_POLL; i++) {
         if (live && o_room() < TND_OUT_MAX + 2u)
            break;
         if (serial_redirect_read(&c, 1u) == 0u)
            break;
         if (g_debug)
            g_mc_ring[g_mc_n++ % MC_RING] = c;
         if (live)
            beeb_byte(c);
         else
            beeb_follow(c);
         if (g_debug && g_out.mode != g_mc_mode)
            mode_changed('v');
      }
   }

   if (g_pcb == NULL)
      return;
   tnd_out_tek(&g_out, g_tek && g_ansi && g_in.xterm);   /* the terminal answered CSI > q as xterm */

   g_in.lit_ok = serial_redirect_beeb->rom;       /* bytes above 127 can go */
   g_in.tele = g_out.tele;                       /* MODE 7: its glyphs come back as its codes */

   /* Every key taken : when the count last moved, and a key that drew no character
      (WordWise-Plus moves the cursor with VDU 31 and writes the screen memory itself). */
   {
      uint32_t taken = serial_redirect_beeb->key_gen + serial_redirect_beeb->key_other_gen;
      if (taken != g_taken_seen) {
         g_taken_seen = taken;
         g_taken_us = now_us;
      }
      uint32_t other = serial_redirect_beeb->key_other_gen;
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
         if (g_out.drawn == g_silent_tx && serial_redirect_beeb->rom && g_ansi && g_pcb != NULL
             && logged_in() && g_scan == SCAN_NONE && (now_us - g_silent_scan_us) >= SILENT_SCAN_US) {
            g_silent_scan_us = now_us;
            g_scan = g_out.sync ? SCAN_MERGE : SCAN_FULL;   /* the screen read, merged: only what differs is drawn */
         }
      }
   }

   /* The keys the Beeb has taken : each moves the copy cursor, in order. */
   g_out.cc_exact = serial_redirect_beeb->rom;
   {
      uint32_t gen = serial_redirect_beeb->key_gen;
      while (g_kgen_seen != gen) {
         g_kgen_seen++;
         if (g_ckq_head != g_ckq_tail)
            tnd_out_copykey(&g_out, g_ckq[g_ckq_head++ % CKQ_SIZE]);
      }
   }

   if ((g_stage == S_LOGIN_USER || g_stage == S_LOGIN_PASS) && (now_us - g_start_us) >= LOGIN_US) {
      o_str("\r\nLogin timed out\r\n");          /* once: the session closes when it has gone */
      g_stage = S_CLOSING;
      (void)tnd_login_fail(&g_login, g_peer_addr);   /* a failure too: holding the prompt by typing now and */
   }                                              /* then, reconnecting each time, ends in a lockout */

   if (g_hold_n != 0u && !tnd_login_held(&g_login) && (g_stage == S_LOGIN_USER || g_stage == S_LOGIN_PASS)) {
      uint8_t  k[sizeof g_hold];
      unsigned n = g_hold_n;
      memcpy(k, g_hold, n);                      /* what was typed during the hold, in order (a wrong */
      g_hold_n = 0u;                             /* password among it starts another hold: the rest waits) */
      for (unsigned i = 0u; i < n; i++)
         login_key(k[i]);
   }

   /* The client closed its side with keys still to go in: once they are in and the output has
      gone, or nothing has moved for FIN_STUCK_US, the session ends. */
   if (g_fin) {
      static uint32_t left;
      uint32_t        now_left = (g_in_tail - g_in_head) + (g_ktail - g_khead) + (uint32_t)(g_olen - g_opos);
      if (now_left != left) {
         left = now_left;
         g_fin_us = now_us;
      }
      if ((now_left == 0u && (now_us - g_fin_us) >= FIN_QUIET_US) || (now_us - g_fin_us) >= FIN_STUCK_US) {
         (void)session_close(false);
         return;
      }
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
      aborts the connection after its retransmissions.  Not on the raw port: no
      telnet there, so the NOP would arrive as two characters of text. */
   if (!g_sess_rawport && g_olen == g_opos && (now_us - g_tx_us) >= KEEPALIVE_US) {
      static const uint8_t nop[2] = { 255u, 241u };
      o_put(nop, 2u);
   }

   {
      uint8_t k[TND_IN_KEYS];
      size_t  n = tnd_in_tick(&g_in, now_us, k);   /* Alt-P with nothing after it is two keys: ESCAPE and P */
      for (size_t i = 0u; i < n; i++)
         take_key(k[i]);
   }

   if (g_in_head != g_in_tail)
      pend_consume();                           /* client data waiting for room in the ring */

   /* The client's window size: the first one before the screen is drawn, a change after it. */
   if (g_pcb != NULL && logged_in() && g_in.win_gen != g_win_gen && o_room() >= TND_COPY_MAX + 120u) {
      g_win_gen = g_in.win_gen;
      if (!g_win_first) {
         g_win_first = true;                     /* the first: the connecting phase looks at it */
      } else if (g_ansi && g_stage == S_LIVE) {
         uint8_t rb[TND_COPY_MAX];
         o_put(rb, tnd_out_redraw(&g_out, rb));   /* resized: drawn again by position */
      }
   }
   bool naws_wait = !g_win_first && !g_in.naws_no && (now_us - g_start_us) < NAWS_WAIT_US;

   /* The client changed between line and character mode (Linux telnet's "mode line"): its own
      "telnet>" prompt is where the Beeb's line was, and nothing more comes until the Beeb prints.
      Shown again: an ANSI terminal drawn again by position, a teletype the line (its prompt and
      what was typed of it before the change). */
   if (g_pcb != NULL && logged_in() && g_in.mode_gen != g_mode_gen && o_room() >= TND_COPY_MAX + TLINE_MAX + 16u) {
      g_mode_gen = g_in.mode_gen;
      if (g_stage == S_LIVE) {
         if (g_ansi) {
            uint8_t rb[TND_COPY_MAX];
            size_t  rn = tnd_out_redraw(&g_out, rb);
            if (rn != 0u) {
               o_put(rb, rn);
               o_put(rb, tnd_out_colours(&g_out, rb));
            }
         } else {
            o_str("\r");
            o_beeb_line();
         }
      }
   }

   /* Connecting: a dot now and then; done once the minimum is over and the client has said its
      window size (or not, in time) - then a too-small window is told so for a while, and then
      the terminal is cleared and the Beeb's screen drawn in place. */
   if (g_stage == S_CONNECTING && g_pcb != NULL && o_room() >= TND_COPY_MAX + 160u) {
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
         g_stage = S_LIVE;
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
            o_str("\r\n");                       /* teletype mode: the line the Beeb is on */
            o_beeb_line();
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
      uint32_t        gen = serial_redirect_beeb->unseen_gen;
      if (gen != unseen_seen) {
         unseen_seen = gen;
         unseen_us = now_us;
         g_unseen = true;
      } else if (g_unseen && (now_us - unseen_us) >= UNSEEN_QUIET_US) {
         g_unseen = false;
         if (g_scan == SCAN_NONE)
            g_scan = SCAN_FULL;
      }
   }
   if (g_out.rescan) {                           /* a compared read was cut short: an ordinary one */
      g_out.rescan = false;
      if (g_scan == SCAN_NONE)
         g_scan = SCAN_FULL;
   }
   if (g_scan != SCAN_NONE && g_ansi && g_stage == S_LIVE && !naws_wait && g_pcb != NULL && beeb_attached
       && serial_redirect_beeb->rom && g_khead == g_ktail) {
      if (g_scan == SCAN_MERGE)
         tnd_out_scan_merge(&g_out);
      else
         tnd_out_scan_full(&g_out);
      g_scan = SCAN_NONE;
      k_push(SCAN_KEY);
   }

   /* A narrow text window that scrolled or was cleared is drawn again from the model,
      a row at a time, once the Beeb has nothing more to print for now. */
   if (g_out.dirty && logged_in() && g_pcb != NULL && !serial_redirect_pending()
       && o_room() >= TND_REPAINT_MAX + 2u) {
      uint8_t db[TND_REPAINT_MAX];
      size_t  dn = tnd_out_dirty_row(&g_out, db);
      if (dn != 0u)
         o_put(db, dn);
   }

   /* The copy cursor's mark, once the Beeb has been quiet for a moment and has
      the keys. */
   if (g_pcb != NULL && logged_in() && g_copy && g_ansi && g_khead == g_ktail
       && o_room() >= TND_COPY_MAX + 2u) {
      uint8_t cb[TND_COPY_MAX];
      size_t  cn = tnd_out_copyflush(&g_out, now_us, cb);
      if (cn != 0u)
         o_put(cb, cn);
   }

   /* The keys, as far as the Beeb's keyboard buffer has room (the redirect's
      own FIFO, and behind it the stub's retry when the MOS buffer is full). */
   if (g_esc_wait) {
      uint8_t r = serial_redirect_escape_result();
      if (r != 0u)
         escape_settled(r);
      else if ((now_us - g_esc_wait_us) > ESC_WAIT_US)
         escape_settled(SER_ESC_OFF);
   }
   if (beeb_attached && logged_in() && !g_esc_wait) {
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
   if (g_stage == S_LIVE && (!g_att_known || beeb_attached != g_att)) {
      if (!beeb_attached) {
         o_str("[BBC not attached: on the BBC type X%=20:CALL &FC88]\r\n");
         tnd_out_origin_lost(&g_out);           /* that line moved the rows: not the Beeb's now */
      }
      g_att_known = true;
      g_att = beeb_attached;
   }

   pump();
}

static void telnetd_poll_cb(void)
{
   telnetd_poll(Pi1MHz_now_us, serial_redirect_telnet_attached());
}

void telnetd_attach(void)
{
   Pi1MHz_Register_Poll(telnetd_poll_cb, "telnetd");
}

/* ---- setup and status ------------------------------------------------------- */

/* TelnetD is off until telnetd_port is set, and Telnetd_addr=-1 turns it off whatever else is set,
   as <name>_addr=-1 does for every other device (it has no FRED address of its own). */
static long telnetd_port_cfg(void)
{
   uint16_t port = 0u;
   uint8_t  unused = 0u;
   if (config_emulator_override("Telnetd", &unused) < 0)
      return 0;
   return text_parse_port_str(config_get("telnetd_port"), &port) ? (long)port : 0;
}

/* Before WiFi is up (the ROM asks at every reset) the setting is the answer; once a listen has
   failed, it is off, so the ROM does not hook the Beeb's output to a daemon nobody can reach. */
bool telnetd_enabled(void)
{
   return telnetd_port_cfg() != 0 && !g_bind_failed;
}

static void telnetd_listen_failed(void)
{
   g_bind_failed = true;
   serial_redirect_telnetd_refresh();
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
   if (g_raw_listener != NULL) {
      tcp_arg(g_raw_listener, NULL);
      tcp_accept(g_raw_listener, NULL);
      tcp_close(g_raw_listener);
      g_raw_listener = NULL;
   }
   g_raw_port = 0u;
   g_port = 0u;
   if (port <= 0 || port > 65535)
      return;
   if (serial_redirect_address() == 0u)      /* Serial_addr=-1: nothing to attach to, and no poll */
      return;

   g_ansi_cfg = config_get_flag("telnetd_ansi", true);
   g_ansi = g_ansi_cfg;
   g_utf8 = config_get_flag("telnetd_utf8", true);
   g_copy = config_get_flag("telnetd_copycursor", true);
   g_tek = config_get_flag("telnetd_tek", true);
   g_debug = config_get_flag("telnetd_debug", false);
   tnd_out_reset(&g_out, g_utf8);
   {
      const char *m = config_get("telnetd_mode");
      long mode = (m != NULL && m[0] != '\0') ? strtol(m, NULL, 10) : 7;
      tnd_out_assume_mode(&g_out, (uint8_t)((mode >= 0 && mode <= 7) ? mode : 7));
      g_mc_mode = g_out.mode;                   /* the MODE changes count from here */
   }

   l = tcp_new_ip_type(IPADDR_TYPE_V4);
   if (l == NULL) {
      telnetd_listen_failed();
      return;
   }
   if (tcp_bind(l, IP_ADDR_ANY, (u16_t)port) != ERR_OK) {
      tcp_close(l);
      telnetd_listen_failed();
      return;
   }
   g_listener = tcp_listen_with_backlog(l, 2u);
   if (g_listener == NULL) {
      tcp_close(l);
      telnetd_listen_failed();
      return;
   }
   if (g_bind_failed) {                         /* listening after all (a later init) */
      g_bind_failed = false;
      serial_redirect_telnetd_refresh();
   }
   tcp_arg(g_listener, NULL);
   tcp_accept(g_listener, td_accept);
   g_port = (uint16_t)port;

   {                                            /* telnetd_raw_port: teletype mode, no telnet (nc, PuTTY "Raw") */
      uint16_t rp = 0u;
      if (text_parse_port_str(config_get("telnetd_raw_port"), &rp) && (long)rp != port) {
         struct tcp_pcb *rl = tcp_new_ip_type(IPADDR_TYPE_V4);
         if (rl != NULL && tcp_bind(rl, IP_ADDR_ANY, (u16_t)rp) == ERR_OK) {
            g_raw_listener = tcp_listen_with_backlog(rl, 2u);
            if (g_raw_listener != NULL) {
               tcp_arg(g_raw_listener, &g_raw_port);   /* the marker td_accept looks for */
               tcp_accept(g_raw_listener, td_accept);
               g_raw_port = (uint16_t)rp;
            } else {
               tcp_close(rl);
            }
         } else if (rl != NULL) {
            tcp_close(rl);
         }
      }
   }
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
                              (unsigned long)serial_redirect_beeb->mode_gen, (unsigned long)serial_redirect_beeb->reset_gen);
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
   if (g_debug) {                               /* printable bytes that sent nothing (see sil_note) */
      used = (size_t)snprintf(buf, len, "silent %lu, followed %lu, put fails %lu:", (unsigned long)g_sil_n,
                              (unsigned long)g_follow_on, (unsigned long)g_oput_fail);
      for (unsigned k = 0u; k < SIL_RING && k < g_sil_n && used < len; k++) {
         unsigned i = (g_sil_n - 1u - k) % SIL_RING;
         used += (size_t)snprintf(buf + used, len - used, " %02x/%02x/%u/%02x@%u,%u", g_sil[i].c, g_sil[i].code,
                                  g_sil[i].sm, g_sil[i].fl, g_sil[i].x, g_sil[i].y);
      }
      if (used < len)
         used += (size_t)snprintf(buf + used, len - used, "; ");
      if (used >= len)
         return;
      buf += used;
      len -= used;
   }
   if (serial_redirect_beeb->rom) {              /* the ROM: the keys waiting, and a Beeb not taking them */
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
      used = (size_t)snprintf(buf, len, "port %u, %s%s from %s, %lu sessions, %lu refused, %lu replaced, %lu dropped, last end: %s",
                              (unsigned int)g_port, logged_in() ? "live" : "login", g_in.raw ? " (teletype)" : "", g_peer,
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
         snprintf(buf, len, " (%d); ring %lu, rx %s, rcvwnd %u, tcp state %d, sndq %u; rtx %lu (most %lu), rto %d ticks,"
                  " unacked longest %lu ms, probes %lu", g_why_err,
                  (unsigned long)(g_ktail - g_khead), g_rx_blocked ? "refused" : "ok",
                  (unsigned int)g_pcb->rcv_wnd, (int)g_pcb->state, (unsigned int)g_pcb->snd_queuelen,
                  (unsigned long)g_rtx_events, (unsigned long)g_rtx_max, (int)g_pcb->rto, (unsigned long)g_unack_max_ms,
                  (unsigned long)g_probes);
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
   snprintf(buf, len, "wmem %lu, room0 %lu; last drop: sndq %u sndbuf %u wnd %u obuf %lu rxblk %d; now: obuf %lu sndq %u sndbuf %u; in %lu, in drops %lu",
            (unsigned long)g_wmem, (unsigned long)g_room0, g_drop_sndq, g_drop_sndbuf, g_drop_wnd,
            g_drop_obuf, g_drop_rxblk, (unsigned long)(g_olen - g_opos),
            g_pcb != NULL ? (unsigned int)g_pcb->snd_queuelen : 0u,
            g_pcb != NULL ? (unsigned int)tcp_sndbuf(g_pcb) : 0u, (unsigned long)(g_in_tail - g_in_head),
            (unsigned long)g_in_drops);
}
