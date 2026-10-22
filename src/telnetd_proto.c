/*
  telnetd_proto.c - the telnet daemon's two byte translators (see the header)

  Client -> Beeb.  A telnet client sends keys as bytes, with IAC command
  sequences mixed in and the terminal's own escape sequences for the cursor and
  function keys.  What the Beeb's keyboard buffer wants is the byte the key
  would have produced there:

     CR NUL, CR LF, lone LF   -> 13
     BS, DEL                  -> 127   (the Beeb's DELETE)
     cursor left/right/down/up-> 140/141/142/143 in the keyboard buffer (what the
                                keyboard scan stores: 136-139 + 4; *FX4 and the
                                copy-cursor editing work from these)
     End                      -> 139   (COPY)     Delete -> 127
     F1-F9, F10               -> 129-137, 128 (f1-f9, f0; the MOS expands *KEY
                                definitions, as for the real keys)
     a lone ESC, Ctrl-C       -> TND_KEY_ESCAPE (the Beeb's ESCAPE condition; a
                                lone ESC is only that after TND_ESC_US with
                                nothing behind it)
     the pound sign           -> 96    (UTF-8 C2 A3, or Latin-1 A3)
     other bytes 0x80 and up  -> dropped
  Telnet negotiation is answered once per option, so a peer cannot start a loop:
  WILL ECHO/SGA and DO ECHO/SGA are ours; anything else is refused.  DONT ECHO
  or DONT SGA (a client's line mode) is answered WONT, and DO again WILL; DO
  TIMING-MARK, which a client sends after an IP and flushes until answered, is
  answered WONT every time.  WILL LINEMODE is taken (see tnd_in_t's lm).

  Beeb -> terminal.  The Beeb's RS423 output (*FX3,1) is the VDU byte stream:
  text, plus control codes some of which take parameter bytes.  A terminal gets
  ANSI for the ones it can show, and the parameter bytes of all the others are
  swallowed.  Teletext colour codes (Mode 7) become colour changes.
*/
#include <string.h>
#include "net_telnet.h"
#include "telnetd_proto.h"

/* ---- client -> Beeb ---------------------------------------------------- */

enum { I_NORMAL, I_CR, I_IAC, I_OPT, I_SB, I_SBIAC, I_ESC, I_CSI, I_SS3, I_C2, I_DCS0, I_DCS, I_DCSESC };

/* The end of a telnet command: back to the escape sequence it came inside, if any. */
#define IAC_DONE(s) ((s)->state = (s)->after, (s)->after = I_NORMAL)

/* The answer to WILL LINEMODE: DO, the client to edit and trap signals (MODE EDIT|TRAPSIG), and its
   special characters (SLC, function / level / value; level 2 is a value, &40 flush its input):
   interrupt (3) ESC, abort (7) Ctrl-C, forward (17) "~". */
static const uint8_t lm_on[] = {
   TN_IAC, TN_DO, TND_OPT_LINEMODE,
   TN_IAC, TN_SB, TND_OPT_LINEMODE, 1u, 3u, TN_IAC, TN_SE,
   TN_IAC, TN_SB, TND_OPT_LINEMODE, 3u, 3u, 0x42u, 27u, 7u, 0x42u, 3u, 17u, 2u, '~', TN_IAC, TN_SE
};

#define ESC_STUCK_US  500000u    /* an escape sequence that never finished */
#define SB_STUCK_US   2000000u   /* a subnegotiation that never finished (IAC SB with no IAC SE) */

/* Options the client agreed to: if it takes one back later, that is acknowledged (RFC 854). */
#define AGREED_ECHO   1u         /* DO ECHO: we echo (the Beeb does) */
#define AGREED_SGA_DO 2u         /* DO SGA: we suppress go-ahead */
#define AGREED_SGA_WILL 4u       /* WILL SGA: the client does */

static bool seen(const uint8_t *map, uint8_t o)
{
   return (((unsigned)map[o >> 3] >> (o & 7u)) & 1u) != 0u;
}

static void mark(uint8_t *map, uint8_t o)
{
   map[o >> 3] = (uint8_t)(map[o >> 3] | (1u << (o & 7u)));
}

void tnd_in_reset(tnd_in_t *s)
{
   memset(s, 0, sizeof *s);
}

size_t tnd_greeting(uint8_t *out)
{
   static const uint8_t g[TND_GREETING_LEN] = {
      TN_IAC, TN_WILL, TN_OPT_ECHO,
      TN_IAC, TN_WILL, TN_OPT_SGA,
      TN_IAC, TN_DO,   TN_OPT_SGA,
      TN_IAC, TN_DO,   TND_OPT_NAWS,
      TN_IAC, TN_DO,   TND_OPT_TTYPE
   };
   memcpy(out, g, sizeof g);
   return sizeof g;
}

/* A character above 127 from the terminal, as the Beeb's byte: the inverse of what the daemon
   prints.  The pound sign is the Beeb's 96 (as before); in MODE 7 teletext's glyphs are their
   codes (an up arrow is &5E); otherwise U+0080-U+00FF is the byte of that value, which goes to the
   Beeb behind TND_KEY_LIT when its ROM can read it as it is (build 13), and is dropped when not. */
static size_t char_key(const tnd_in_t *s, uint32_t cp, uint8_t *keys)
{
   if (cp == 0xA3u) {
      keys[0] = 96u;
      return 1u;
   }
   if (s->tele) {
      uint8_t b = 0u;
      switch (cp) {
      case 0x2190u: b = 0x5Bu; break;  case 0xBDu:   b = 0x5Cu; break;
      case 0x2192u: b = 0x5Du; break;  case 0x2191u: b = 0x5Eu; break;
      case 0x2015u: b = 0x60u; break;  case 0xBCu:   b = 0x7Bu; break;
      case 0x2016u: b = 0x7Cu; break;  case 0xBEu:   b = 0x7Du; break;
      case 0xF7u:   b = 0x7Eu; break;  case 0x25A0u: b = 0x7Fu; break;
      default: break;
      }
      if (b != 0u) {
         keys[0] = b;
         return 1u;
      }
   }
   if (cp >= 0x80u && cp <= 0xFFu && s->lit_ok) {
      keys[0] = TND_KEY_LIT;
      keys[1] = (uint8_t)cp;
      return 2u;
   }
   return 0u;
}

/* A byte that is not part of a command or an escape sequence. */
static size_t plain_byte(tnd_in_t *s, uint8_t c, uint8_t *keys)
{
   if (s->state == I_CR) {
      s->state = I_NORMAL;
      if (c == 0u || c == '\n')                 /* the second half of CR NUL / CR LF */
         return 0u;
   }

   if (s->u8_need != 0u) {                      /* inside a UTF-8 character */
      if ((c & 0xC0u) == 0x80u) {
         s->u8_cp = (s->u8_cp << 6) | (c & 0x3Fu);
         s->u8_lead = 0u;
         if (--s->u8_need != 0u)
            return 0u;
         return char_key(s, s->u8_cp, keys);
      }
      s->u8_need = 0u;                          /* broken: this byte read afresh */
      if (s->u8_lead != 0u) {                   /* a lone lead byte was a Latin-1 client's character (&E9) */
         size_t n = char_key(s, s->u8_lead, keys);
         s->u8_lead = 0u;
         return n + plain_byte(s, c, keys + n);
      }
   }

   if (c == TN_IAC) {
      s->state = I_IAC;
      return 0u;
   }
   if (c == '\r') {
      s->state = I_CR;
      keys[0] = 13u;
      return 1u;
   }
   if (c == '\n') {
      keys[0] = 13u;
      return 1u;
   }
   if (c == 0u)
      return 0u;
   if (c == 3u) {                               /* Ctrl-C: what a terminal user means by ESCAPE */
      keys[0] = TND_KEY_ESCAPE;
      return 1u;
   }
   if (c == 12u) {                              /* Ctrl-L: what a terminal user means by "draw it again" */
      keys[0] = TND_KEY_REFRESH;
      return 1u;
   }
   if (c == 8u || c == 127u) {
      keys[0] = 127u;
      return 1u;
   }
   if (c >= 0xC2u && c <= 0xF4u) {              /* the start of a UTF-8 character */
      s->u8_cp = c & (c >= 0xF0u ? 0x07u : c >= 0xE0u ? 0x0Fu : 0x1Fu);
      s->u8_need = (uint8_t)(c >= 0xF0u ? 3u : c >= 0xE0u ? 2u : 1u);
      s->u8_lead = c;
      return 0u;
   }
   if (c >= 0x80u)                              /* not UTF-8: a Latin-1 client's byte */
      return char_key(s, c, keys);
   keys[0] = c;
   return 1u;
}

/* The Beeb key for a function key's CSI number (ESC [ n ~), 0 for none. */
static uint8_t fkey_csi(unsigned n)
{
   switch (n) {
   case 11: return 0x81u;  case 12: return 0x82u;  case 13: return 0x83u;
   case 14: return 0x84u;  case 15: return 0x85u;  case 17: return 0x86u;
   case 18: return 0x87u;  case 19: return 0x88u;  case 20: return 0x89u;
   case 21: return 0x80u;
   case 6:  return TND_KEY_SHIFT;                /* Page Down: SHIFT, for paged mode */
   case 23: return TND_KEY_REFRESH;              /* F11 */
   case 24: return TND_KEY_BREAK;                /* F12 */
   default: return 0u;
   }
}

/* What the keyboard scan puts in the MOS keyboard buffer for the cursor keys
   and COPY: the codes GET returns under *FX4,1 (135-139) plus 4.  The MOS takes
   the 4 off again, or, with *FX4,0, treats them as copy-cursor editing - so
   these, and not 135-139, are what to insert (measured on a BBC B: inserting 139
   read back as 135, COPY). */
static uint8_t arrow(uint8_t f)
{
   switch (f) {
   case 'A': return 143u;                       /* up    */
   case 'B': return 142u;                       /* down  */
   case 'C': return 141u;                       /* right */
   case 'D': return 140u;                       /* left  */
   case 'F': return 139u;                       /* COPY  */
   default:  return 0u;
   }
}

/* The final byte of ESC [ params f. */
static uint8_t csi_key(const tnd_in_t *s, uint8_t f)
{
   if (f == '~' && s->param[0] == 24u && s->nparam >= 1u) {   /* F12 with SHIFT (2), CTRL (5) or both (6) */
      if (s->param[1] == 2u)
         return TND_KEY_SHIFTBREAK;
      if (s->param[1] == 5u || s->param[1] == 6u)
         return TND_KEY_CTRLBREAK;
   }
   if (f == '~') {
      switch (s->param[0]) {
      case 3: return 127u;
      case 4: case 8: return 139u;                /* End: COPY */
      default: return fkey_csi(s->param[0]);
      }
   }
   return arrow(f);
}

/* The terminal's name, in its answer to XTVERSION (CSI > q): "|XTerm(379)" from xterm, which has a
   Tektronix 4014 mode for the Beeb's graphics. */
static void tnd_in_dcs_done(tnd_in_t *s)
{
   static const char xt[] = "|XTerm(";
   if (s->ndcs >= sizeof xt - 1u && memcmp(s->dcs, xt, sizeof xt - 1u) == 0)
      s->xterm = true;
}

size_t tnd_in_byte(tnd_in_t *s, uint8_t c, uint32_t now_us,
                   uint8_t keys[TND_IN_KEYS], uint8_t rep[TND_IN_REPLY],
                   size_t *replen)
{
   *replen = 0u;

   if (s->raw && (s->notelnet || s->state == I_NORMAL || s->state == I_CR) && !(c == TN_IAC && !s->notelnet)) {
      bool after_cr = (s->state == I_CR);
      s->state = I_NORMAL;
      if (after_cr && !s->bin_in && !s->notelnet && (c == 0u || c == '\n'))
         return 0u;                             /* telnet's CR NUL / CR LF: the CR was the key */
      if (c == 13u && !s->bin_in && !s->notelnet)
         s->state = I_CR;
      if (c == 27u) {
         keys[0] = TND_KEY_ESCAPE;              /* the Beeb's own ESCAPE key */
         return 1u;
      }
      if (c < 128u && c != 0u) {
         keys[0] = c;
         return 1u;
      }
      if (!s->lit_ok)
         return 0u;
      keys[0] = TND_KEY_LIT;                    /* above 127, and NUL (0 is TND_KEY_ESCAPE): as it is, behind the marker */
      keys[1] = c;
      return 2u;
   }

   /* A telnet command inside an escape sequence (the IP that is a line-mode client's ESC, then its DO
      TIMING-MARK, before the rest of F12 or Page Down) is read, and the sequence goes on after it. */
   if (c == TN_IAC && !s->notelnet && s->state >= I_ESC) {
      s->after = s->state;                      /* a telnet command inside an escape sequence: read it, */
      s->state = I_IAC;                         /* then carry on with the sequence (telnet is the layer below) */
      return 0u;
   }

   switch (s->state) {
   case I_IAC:
      switch (c) {
      case TN_IAC:                              /* a literal 0xFF: not a Beeb key ... */
         s->state = s->after;
         if (s->after != I_NORMAL) {             /* (inside an escape sequence: dropped) */
            s->after = I_NORMAL;
            break;
         }
         if (s->raw && s->lit_ok) {             /* ... except in teletype mode: the byte 255 */
            keys[0] = TND_KEY_LIT;
            keys[1] = 0xFFu;
            return 2u;
         }
         break;
      case TN_WILL: case TN_WONT: case TN_DO: case TN_DONT:
         s->cmd = c;
         s->state = I_OPT;
         break;
      case TN_SB:
         s->state = I_SB;
         s->nsb = 0u;
         s->sb_us = now_us;
         break;
      case TN_IP: {                             /* Interrupt Process: a line-mode client's ESC (or Ctrl-C) */
         uint8_t r[TND_IN_REPLY];               /* read as the ESC byte: alone it is ESCAPE, IP [ 2 4 ~ is F12 */
         size_t  rl;
         s->after = I_NORMAL;                   /* (one inside an escape sequence starts afresh) */
         IAC_DONE(s);
         return tnd_in_byte(s, 27u, now_us, keys, r, &rl);
      }
      case TN_ABORT:                            /* a line-mode client's Ctrl-C */
         IAC_DONE(s);
         keys[0] = TND_KEY_ESCAPE;
         return 1u;
      case TN_BRK:                              /* "send brk": the BREAK key */
         IAC_DONE(s);
         keys[0] = TND_KEY_BREAK;
         return 1u;
      default:                                  /* NOP, AYT, GA, ... */
         IAC_DONE(s);
         break;
      }
      return 0u;

   case I_OPT:
      IAC_DONE(s);
      if (c == TND_OPT_LINEMODE && s->cmd == TN_WILL && !s->lm_offered) {
         s->lm_offered = true;                 /* offered as the client connects: character mode, as before */
         rep[0] = TN_IAC; rep[1] = TN_DONT; rep[2] = c;
         *replen = 3u;
         return 0u;
      }
      if (c == TND_OPT_LINEMODE && (s->cmd == TN_WILL || s->cmd == TN_WONT)) {
         bool on = (s->cmd == TN_WILL);
         if (on != s->lm) {
            s->lm = on;
            if (on) {
               memcpy(rep, lm_on, sizeof lm_on);
               *replen = sizeof lm_on;
            } else {
               rep[0] = TN_IAC; rep[1] = TN_DONT; rep[2] = c;
               *replen = 3u;
            }
         }
         return 0u;
      }
      if (c == TND_OPT_BINARY) {                /* teletype mode asked for (or ended): an 8-bit path */
         bool on = (s->cmd == TN_DO || s->cmd == TN_WILL);
         bool *f = (s->cmd == TN_DO || s->cmd == TN_DONT) ? &s->bin_out : &s->bin_in;
         if (*f != on) {
            *f = on;
            rep[0] = TN_IAC;
            rep[1] = (s->cmd == TN_DO) ? TN_WILL : (s->cmd == TN_DONT) ? TN_WONT : (s->cmd == TN_WILL) ? TN_DO : TN_DONT;
            rep[2] = TND_OPT_BINARY;
            *replen = 3u;
         }
         return 0u;
      }
      if ((s->cmd == TN_DO || s->cmd == TN_DONT) && (c == TN_OPT_ECHO || c == TN_OPT_SGA)) {   /* ours */
         uint8_t bit = (c == TN_OPT_ECHO) ? AGREED_ECHO : AGREED_SGA_DO;
         bool *off = (c == TN_OPT_ECHO) ? &s->echo_off : &s->sga_off;
         if (s->cmd == TN_DONT && !*off) {     /* a client's line mode (or our offer refused) */
            *off = true;
            s->mode_gen++;
            if ((s->agreed & bit) != 0u) {     /* an agreed option taken back: say so (a refusal of */
               s->agreed = (uint8_t)(s->agreed & ~bit);   /* the offer itself needs no answer) */
               rep[0] = TN_IAC; rep[1] = TN_WONT; rep[2] = c;
               *replen = 3u;
            }
         } else if (s->cmd == TN_DO && *off) { /* ... and its character mode again: a change, answered */
            *off = false;
            s->mode_gen++;
            s->agreed |= bit;
            rep[0] = TN_IAC; rep[1] = TN_WILL; rep[2] = c;
            *replen = 3u;
         } else if (s->cmd == TN_DO) {         /* an agreement is not */
            s->agreed |= bit;
         }
         return 0u;
      }
      if (s->cmd == TN_DO && c == TND_OPT_TM) {      /* every time: the client flushes until it is answered */
         rep[0] = TN_IAC; rep[1] = TN_WONT; rep[2] = c;
         *replen = 3u;
      } else if (s->cmd == TN_DO) {
         if (!seen(s->seen_do, c)) {
            mark(s->seen_do, c);
            rep[0] = TN_IAC; rep[1] = TN_WONT; rep[2] = c;
            *replen = 3u;
         }
      } else if (s->cmd == TN_WONT) {
         if (c == TND_OPT_NAWS)
            s->naws_no = true;
         if (c == TN_OPT_SGA && (s->agreed & AGREED_SGA_WILL) != 0u) {
            s->agreed = (uint8_t)(s->agreed & ~AGREED_SGA_WILL);
            rep[0] = TN_IAC; rep[1] = TN_DONT; rep[2] = c;
            *replen = 3u;
         }
      } else if (s->cmd == TN_WILL && c == TND_OPT_TTYPE) {
         if (!seen(s->seen_will, c)) {           /* asked for in the greeting: what is it? (SEND) */
            static const uint8_t send[6] = { TN_IAC, TN_SB, TND_OPT_TTYPE, 1u, TN_IAC, TN_SE };
            mark(s->seen_will, c);
            memcpy(rep, send, sizeof send);
            *replen = sizeof send;
         }
      } else if (s->cmd == TN_WILL) {
         if (c == TN_OPT_SGA)
            s->agreed |= AGREED_SGA_WILL;
         else if (c != TND_OPT_NAWS && !seen(s->seen_will, c)) {
            mark(s->seen_will, c);
            rep[0] = TN_IAC; rep[1] = TN_DONT; rep[2] = c;
            *replen = 3u;
         }
      }
      return 0u;

   case I_SB:
      if (c == TN_IAC)
         s->state = I_SBIAC;
      else if (s->nsb < sizeof s->sb)
         s->sb[s->nsb++] = c;
      return 0u;

   case I_SBIAC:
      if (c == TN_SE) {
         IAC_DONE(s);
         if (s->nsb == 5u && s->sb[0] == TND_OPT_NAWS) {   /* NAWS: width and height, high byte first */
            s->win_cols = (uint16_t)((s->sb[1] << 8) | s->sb[2]);
            s->win_rows = (uint16_t)((s->sb[3] << 8) | s->sb[4]);
            s->win_gen++;
         } else if (s->nsb >= 2u && s->sb[0] == TND_OPT_TTYPE && s->sb[1] == 0u) {   /* TTYPE IS name */
            unsigned n = s->nsb - 2u;
            if (n > sizeof s->ttype - 1u)
               n = sizeof s->ttype - 1u;
            memcpy(s->ttype, &s->sb[2], n);
            s->ttype[n] = '\0';
            s->ttype_got = true;
         }
         return 0u;
      }
      s->state = I_SB;
      if (c == TN_IAC && s->nsb < sizeof s->sb)  /* IAC IAC: a 255 in the data */
         s->sb[s->nsb++] = c;
      return 0u;

   case I_ESC:
      if (c == '[') {
         s->state = I_CSI;
         s->nparam = 0u;
         s->param[0] = s->param[1] = s->param[2] = 0u;
         return 0u;
      }
      if (c == 'O') {
         s->state = I_SS3;
         return 0u;
      }
      if (c == 'P') {                           /* a DCS: the terminal answering CSI > q, or Alt-P */
         s->state = I_DCS0;
         return 0u;
      }
      /* Not a sequence: the ESC was the ESCAPE key after all, and this byte is a key. */
      keys[0] = TND_KEY_ESCAPE;
      if (c == 27u) {                           /* ... or the start of the next one */
         s->esc_us = now_us;
         return 1u;
      }
      s->state = I_NORMAL;
      return 1u + plain_byte(s, c, keys + 1);

   case I_CSI:
      if (c >= '0' && c <= '9') {
         uint16_t v = (uint16_t)(s->param[s->nparam] * 10u + (unsigned)(c - '0'));
         s->param[s->nparam] = v > 999u ? 999u : v;
         return 0u;
      }
      if (c == ';') {
         if (s->nparam < 2u)
            s->nparam++;
         return 0u;
      }
      if (c >= 0x40u && c <= 0x7Eu) {           /* the final byte */
         uint8_t k = csi_key(s, c);
         s->state = I_NORMAL;
         if (k != 0u) {
            keys[0] = k;
            return 1u;
         }
      }
      return 0u;                                /* a parameter or intermediate byte */

   case I_DCS0:                                 /* only a reply to CSI > q is taken as one: DCS > | ... */
      if (c == '>') {
         s->state = I_DCS;
         s->ndcs = 0u;
         return 0u;
      }
      s->state = I_NORMAL;                      /* ESC P was Alt-P: the ESCAPE key, a P, and this */
      keys[0] = TND_KEY_ESCAPE;
      {
         size_t k = 1u + plain_byte(s, 'P', keys + 1);
         if (c == 27u) {                         /* ... or the start of the next sequence */
            s->state = I_ESC;
            s->esc_us = now_us;
         } else if (k < TND_IN_KEYS) {
            k += plain_byte(s, c, keys + k);
         }
         return k;
      }

   case I_DCS:                                  /* up to the string terminator (ESC \, or BEL) */
      if (c == 27u) {
         s->state = I_DCSESC;
      } else if (c == 7u) {
         s->state = I_NORMAL;
         tnd_in_dcs_done(s);
      } else if (s->ndcs < sizeof s->dcs) {
         s->dcs[s->ndcs++] = c;
      }
      return 0u;

   case I_DCSESC:
      s->state = I_NORMAL;
      tnd_in_dcs_done(s);
      return 0u;

   case I_SS3:
      s->state = I_NORMAL;
      if (c >= 'P' && c <= 'S') {               /* F1-F4 */
         keys[0] = (uint8_t)(0x81u + (unsigned)c - (unsigned)'P');
         return 1u;
      }
      {
         uint8_t k = arrow(c);
         if (k != 0u) {
            keys[0] = k;
            return 1u;
         }
      }
      return 0u;

   default:
      if (c == 27u) {
         size_t n = 0u;
         if (s->u8_need != 0u && s->u8_lead != 0u)   /* a lone lead byte before it was a Latin-1 character: */
            n = char_key(s, s->u8_lead, keys);       /* given now, so nothing is left over for the escape paths */
         s->u8_need = 0u;
         s->u8_lead = 0u;
         s->state = I_ESC;
         s->esc_us = now_us;
         return n;
      }
      {
         size_t n = plain_byte(s, c, keys);
         if (s->u8_lead != 0u)
            s->u8_us = now_us;
         return n;
      }
   }
}

size_t tnd_in_tick(tnd_in_t *s, uint32_t now_us, uint8_t keys[TND_IN_KEYS])
{
   if (s->state == I_ESC && (now_us - s->esc_us) >= TND_ESC_US) {
      s->state = I_NORMAL;
      keys[0] = TND_KEY_ESCAPE;
      return 1u;
   }
   if ((s->state == I_CSI || s->state == I_SS3 || s->state == I_DCS || s->state == I_DCSESC)
       && (now_us - s->esc_us) >= ESC_STUCK_US)
      s->state = I_NORMAL;
   if (s->state == I_DCS0 && (now_us - s->esc_us) >= TND_ESC_US) {   /* Alt-P with nothing after it */
      s->state = I_NORMAL;
      keys[0] = TND_KEY_ESCAPE;
      return 1u + plain_byte(s, 'P', keys + 1);
   }
   if ((s->state == I_SB || s->state == I_SBIAC) && (now_us - s->sb_us) >= SB_STUCK_US) {
      s->state = I_NORMAL;                      /* IAC SB that never ended: the keys after it are keys again */
      s->after = I_NORMAL;
   }
   if (s->state == I_NORMAL && s->u8_need != 0u && s->u8_lead != 0u
       && (now_us - s->u8_us) >= TND_ESC_US) {  /* a lone lead byte nothing followed: a Latin-1 character */
      size_t n = char_key(s, s->u8_lead, keys);
      s->u8_need = 0u;
      s->u8_lead = 0u;
      return n;
   }
   return 0u;
}

/* The look ahead's states: a byte stream's place, as tnd_in_byte's, without its data. */
enum { A_NORMAL, A_IAC, A_OPT, A_SB, A_SBIAC, A_ESC, A_CSI, A_SS3, A_DCS0, A_DCS, A_DCSESC,
       A_CSI0, A_CSI2, A_CSI24, A_CSI24M };   /* ESC [, ESC [ 2, ESC [ 2 4, ESC [ 2 4 ; m: F12 is ESC [ 2 4 ~ */

static uint8_t ahead_state(uint8_t st)
{
   switch (st) {
   case I_IAC:    return A_IAC;
   case I_OPT:    return A_OPT;
   case I_SB:     return A_SB;
   case I_SBIAC:  return A_SBIAC;
   case I_ESC:    return A_ESC;
   case I_CSI:    return A_CSI;
   case I_SS3:    return A_SS3;
   case I_DCS0:   return A_DCS0;
   case I_DCS:    return A_DCS;
   case I_DCSESC: return A_DCSESC;
   default:       return A_NORMAL;
   }
}

void tnd_in_ahead_start(const tnd_in_t *s, tnd_ahead_t *a)
{
   a->st = ahead_state(s->state);
   a->ret = ahead_state(s->after);
   a->raw = s->raw;
   a->notelnet = s->notelnet;
}

bool tnd_in_ahead_byte(tnd_ahead_t *a, uint8_t c)
{
   if (c == TN_IAC && !a->notelnet && a->st != A_IAC && a->st != A_OPT && a->st != A_SB && a->st != A_SBIAC) {
      if (a->st != A_NORMAL)
         a->ret = a->st;
      a->st = A_IAC;
      return false;
   }
   switch (a->st) {
   case A_IAC:
      if (c == TN_BRK || c == TN_ABORT)         /* BREAK, ESCAPE */
         return true;
      if (c == TN_IP) {                         /* a line-mode client's ESC: the ESCAPE key, or F12's start */
         if (a->raw)
            return true;
         a->st = A_ESC;
         a->ret = A_NORMAL;
      } else if (c == TN_SB)
         a->st = A_SB;
      else if (c >= TN_WILL && c <= TN_DONT)
         a->st = A_OPT;
      else {
         a->st = a->ret;
         a->ret = A_NORMAL;
      }
      return false;
   case A_OPT:
      a->st = a->ret;
      a->ret = A_NORMAL;
      return false;
   case A_SB:
      if (c == TN_IAC)
         a->st = A_SBIAC;
      return false;
   case A_SBIAC:
      if (c == TN_SE) {
         a->st = a->ret;
         a->ret = A_NORMAL;
      } else {
         a->st = A_SB;
      }
      return false;
   case A_ESC:
      if (c == '[') a->st = A_CSI0;
      else if (c == 'O') a->st = A_SS3;
      else if (c == 'P') a->st = A_DCS0;
      else return true;                         /* not a sequence: the ESC was the key */
      return false;
   case A_CSI0:                                 /* F12 (and with SHIFT or CTRL) is BREAK */
   case A_CSI2:
   case A_CSI24:
   case A_CSI24M:
      if ((a->st == A_CSI24 || a->st == A_CSI24M) && c == '~')
         return true;
      if (a->st == A_CSI0 && c == '2')
         a->st = A_CSI2;
      else if (a->st == A_CSI2 && c == '4')
         a->st = A_CSI24;
      else if (a->st == A_CSI24 && c == ';')
         a->st = A_CSI24M;
      else if (a->st != A_CSI24M || c < '0' || c > '9')
         a->st = (c >= 0x40u && c <= 0x7Eu) ? A_NORMAL : A_CSI;
      return false;
   case A_CSI:
      if (c >= 0x40u && c <= 0x7Eu)
         a->st = A_NORMAL;
      return false;
   case A_SS3:
      a->st = A_NORMAL;
      return false;
   case A_DCS0:
      a->st = (c == '>') ? A_DCS : (c == 27u) ? A_ESC : A_NORMAL;
      return false;
   case A_DCS:
      if (c == 27u) a->st = A_DCSESC;
      else if (c == 7u) a->st = A_NORMAL;
      return false;
   case A_DCSESC:
      a->st = A_NORMAL;
      return false;
   default:
      if (c == 27u) {
         if (a->raw)                            /* teletype mode: ESC is the ESCAPE key, always */
            return true;
         a->st = A_ESC;
      }
      return false;
   }
}

bool tnd_in_ahead_esc_end(const tnd_ahead_t *a)
{
   return a->st == A_ESC;
}

/* ---- Beeb -> terminal ---------------------------------------------------- */

/* How many parameter bytes follow each VDU code (the Beeb User Guide's VDU
   table). */
static uint8_t vdu_params(uint8_t c)
{
   switch (c) {
   case 1:  return 1u;
   case 17: return 1u;
   case 18: return 2u;
   case 19: return 5u;
   case 22: return 1u;
   case 23: return 9u;
   case 24: return 8u;
   case 25: return 5u;
   case 28: return 4u;
   case 29: return 4u;
   case 31: return 2u;
   default: return 0u;
   }
}

#define AT_KNOWN 0x80u       /* the model has seen what is in this cell */
#define TTX_SGR_PLAIN 0x07u  /* MODE 7, the terminal's colours: white on black, steady, its own (ESC [0m) */
#define AT_DEF   0x40u       /* no colour was set: the terminal's own */

/* ---- the text screen model ---------------------------------------------------
   The terminal tracks its own cursor, which is enough while the Beeb prints
   along a line.  It is not enough once the Beeb's screen is a different size
   (the Beeb wraps at 40 columns in MODE 7, the terminal at 80 or more), scrolls
   inside its text window, or has the copy cursor, which the MOS moves without
   telling the RS423 stream.  So the model follows every VDU code, and the
   translator sends an absolute cursor position or a scroll wherever the
   terminal would otherwise drift.  Until a CLS, MODE, TAB or similar fixes the
   Beeb's cursor position the model does not know where it is, and the output is
   what it was before the model existed. */

/* What each logical colour shows as after a MODE: the Beeb's default palette. */
static void scr_default_palette(tnd_out_t *s)
{
   static const uint8_t four[4] = { 0u, 1u, 3u, 7u };     /* black, red, yellow, white */

   switch (s->mode) {
   case 0: case 3: case 4: case 6:              /* two colours */
      s->ncol = 2u;
      s->pal[0] = 0u; s->pal[1] = 7u;
      break;
   case 1: case 5:
      s->ncol = 4u;
      memcpy(s->pal, four, sizeof four);
      break;
   case 2:                                      /* sixteen, the upper eight flashing: shown steady */
      s->ncol = 16u;
      for (unsigned i = 0u; i < 16u; i++)
         s->pal[i] = (uint8_t)(i & 7u);
      break;
   default:                                     /* teletext: the colours are the colours */
      s->ncol = 8u;
      for (unsigned i = 0u; i < 16u; i++)
         s->pal[i] = (uint8_t)(i & 7u);
      break;
   }
}

/* The terminal's colour for a logical one. */
static uint8_t phys(const tnd_out_t *s, uint8_t logical)
{
   return (uint8_t)(s->pal[logical & 15u] & 7u);
}

static void tnd_out_set_mode(tnd_out_t *s, uint8_t mode)
{
   static const uint8_t cols[8] = { 80u, 40u, 20u, 80u, 40u, 20u, 40u, 40u };
   static const uint8_t rows[8] = { 32u, 32u, 32u, 25u, 32u, 32u, 25u, 25u };

   mode &= 7u;                                  /* 128-135 are the shadow modes: the same sizes */
   s->mode = mode;
   s->cols = cols[mode];
   s->rows = rows[mode];
   s->tele = (mode == 7u);
   s->wl = 0u; s->wt = 0u;
   s->wr = (uint8_t)(s->cols - 1u);
   s->wb = (uint8_t)(s->rows - 1u);
   s->cx = s->cy = 0u;
   scr_default_palette(s);
   s->fg = (uint8_t)(s->ncol - 1u); s->bg = 0u;  /* the MOS's: the highest logical colour on 0 */
   s->cc_on = s->hl_on = false;
   s->npend = 0u;
   s->dirty = false;                             /* a new screen: nothing left to draw again */
   memset(s->ch, ' ', sizeof s->ch);
   memset(s->at, AT_KNOWN | AT_DEF, sizeof s->at);
   memset(s->lattr, 0, sizeof s->lattr);
   s->tsgr = TTX_SGR_PLAIN;
}

void tnd_out_assume_mode(tnd_out_t *s, uint8_t mode)
{
   tnd_out_set_mode(s, mode);
   s->code = 0u;                                 /* a reset ends a VDU sequence half collected ... */
   s->got = 0u;
   s->disabled = false;                          /* ... and a VDU 21, as the MOS's does */
   memset(s->at, AT_DEF, sizeof s->at);          /* nothing on the screen is known yet */
   s->sync = false;
   s->paged = false;                             /* a reset ends paged mode, as the MOS's does */
}

void tnd_out_at(tnd_out_t *s, uint8_t x, uint8_t y)
{
   if (x <= s->wr && y <= s->wb) {
      s->cx = x;
      s->cy = y;
      s->sync = true;
      s->colok = true;
   }
}

/* ---- the boot banners (see telnetd_proto.h) ------------------------------- */

void tnd_banner_reset(tnd_banner_t *b)
{
   memset(b, 0, sizeof *b);
}

static void banner_end_line(tnd_banner_t *b, bool block)
{
   if (b->llen != 0u && (block || (b->line[0] != ' ' && b->line[0] != '>'))
       && b->len + b->llen + 1u <= sizeof b->text) {
      memcpy(&b->text[b->len], b->line, b->llen);
      b->len += b->llen;
      b->text[b->len++] = '\n';
      b->lines++;
   }
   b->llen = 0u;
}

bool tnd_banner_byte(tnd_banner_t *b, uint8_t c, bool open)
{
   if (b->skip != 0u) {                            /* a parameter: not text, not a code */
      b->skip--;
      return false;
   }
   if (c < 32u && vdu_params(c) != 0u) {
      b->skip = vdu_params(c);
      return false;
   }
   if (c == 21u) {
      banner_end_line(b, false);
      b->blk = true;
      return false;
   }
   if (c == 6u) {
      banner_end_line(b, true);
      b->blk = false;
      return false;
   }
   if (!open)                                      /* the banners are read only just after a start: */
      return false;                                /* a later VDU 21 block is a program's, not a banner */
   if (!b->blk && b->llen == 0u && c == '>')
      return true;                                 /* the language's prompt: the start-up is over */
   if (c == 10u || c == 13u) {
      if (c == 10u)
         banner_end_line(b, b->blk);
   } else if (c >= 32u && c < 127u && c != 96u) {
      if (b->llen < sizeof b->line)
         b->line[b->llen++] = (char)c;
   }
   return false;
}

void tnd_out_reset(tnd_out_t *s, bool utf8)
{
   memset(s, 0, sizeof *s);
   s->utf8 = utf8;
   tnd_out_assume_mode(s, 7u);
   s->origin_ok = true;
}

void tnd_out_touch(tnd_out_t *s, uint32_t now_us)
{
   s->last_us = now_us;
}

void tnd_out_beeb_reset(tnd_out_t *s)
{
   s->coloured = false;                          /* a reset puts the colours and the cursor back */
   s->curoff = false;
   s->smerge = false;                            /* ... and ends a screen read half sent */
}

void tnd_out_session(tnd_out_t *s)
{
   s->tek_on = false;                            /* a new terminal is not in Tektronix mode */
   s->tsgr = 0xFFu;                              /* a new terminal: its colours are not known */
   s->hl_on = false;
   s->cc_on = false;
   s->npend = 0u;                                /* (the Beeb's own colours, `coloured`, carry on) */
}

static size_t put_str(uint8_t *dst, const char *str);

static size_t put_str(uint8_t *dst, const char *str)
{
   size_t n = 0u;
   while (str[n] != '\0') {
      dst[n] = (uint8_t)str[n];
      n++;
   }
   return n;
}

/* ESC [ n m for a single-digit n. */
static size_t sgr(uint8_t *dst, char kind, uint8_t colour)
{
   dst[0] = 27u; dst[1] = '['; dst[2] = (uint8_t)kind;
   dst[3] = (uint8_t)('0' + (colour & 7u)); dst[4] = 'm';
   return 5u;
}

static size_t put_num(uint8_t *dst, unsigned v)
{
   size_t n = 0u;
   if (v >= 100u)
      dst[n++] = (uint8_t)('0' + v / 100u);
   if (v >= 10u)
      dst[n++] = (uint8_t)('0' + (v / 10u) % 10u);
   dst[n++] = (uint8_t)('0' + v % 10u);
   return n;
}

/* ESC [ row ; col H for the zero-based cell x, y. */
/* The cursor to column x (from 0) of the row it is on: CR, then forward.  Not CHA (ESC [ n G):
   Windows' telnet client misreads it (the cursor goes to the next line, and what follows is
   garbled), while CR and cursor forward work in every VT100-style terminal. */
static size_t put_col(uint8_t *dst, unsigned x)
{
   size_t n = 0u;
   dst[n++] = 13u;
   if (x != 0u) {
      dst[n++] = 27u;
      dst[n++] = '[';
      n += put_num(dst + n, x);
      dst[n++] = 'C';
   }
   return n;
}

static size_t put_pos(uint8_t *dst, unsigned x, unsigned y)
{
   size_t n = 0u;
   dst[n++] = 27u;
   dst[n++] = '[';
   n += put_num(dst + n, y + 1u);
   dst[n++] = ';';
   n += put_num(dst + n, x + 1u);
   dst[n++] = 'H';
   return n;
}

/* An absolute cursor move.  If the terminal's rows are not the Beeb's yet (a terminal
   that was drawn under its greeting), it is cleared first, once, and they are. */
static size_t abs_pos(tnd_out_t *s, uint8_t *dst, unsigned x, unsigned y)
{
   size_t n = 0u;

   if (!s->origin_ok) {
      n += put_str(dst, "\x1b[2J");
      s->origin_ok = true;
   }
   n += put_pos(dst + n, x, y);
   return n;
}

static uint8_t cur_attr(const tnd_out_t *s)
{
   return s->coloured ? (uint8_t)((s->fg & 7u) | ((s->bg & 7u) << 3)) : AT_DEF;
}

/* The terminal's colours for a cell's attribute. */
static size_t put_attr(const tnd_out_t *s, uint8_t *dst, uint8_t at)
{
   size_t n = put_str(dst, "\x1b[0m");
   if ((at & AT_DEF) == 0u) {
      n += sgr(dst + n, '3', phys(s, at & 7u));
      if (phys(s, (uint8_t)((at >> 3) & 7u)) != 0u)
         n += sgr(dst + n, '4', phys(s, (uint8_t)((at >> 3) & 7u)));
   }
   return n;
}

/* A stored cell character as the terminal wants it: 96 is the Beeb's pound. */
static size_t put_glyph(const tnd_out_t *s, uint8_t *dst, uint8_t ch)
{
   if (ch == 96u) {
      if (s->utf8) {
         dst[0] = 0xC2u; dst[1] = 0xA3u;
         return 2u;
      }
      dst[0] = 0xA3u;
      return 1u;
   }
   dst[0] = ch;
   return 1u;
}

/* ---- teletext (MODE 7) ------------------------------------------------------
   The SAA5050 draws a row from left to right, and what a cell shows depends on
   the control codes to its left: each row starts white on black, alphanumerics,
   single height, and a control code changes that for the cells after it (colours,
   graphics, flash, double height, release) or from its own cell (new and black
   background, steady, normal height, conceal, contiguous and separated, hold).  A
   control code's own cell is a space, or under hold graphics the last mosaic.

   So once the model knows where the cursor is, each MODE 7 cell holds the byte
   the screen holds (the MOS swaps #, _ and the pound as it stores them) and is
   drawn from the state of the cells to its left: a character is one cell, a
   control code (or a cell that held one) redraws the rest of its row.  Mosaics
   are Unicode's sextants (the block U+1FB00, with the half and full blocks that
   Unicode already had); the characters the SAA5050 has in place of ASCII are
   their Unicode ones; double height is the terminal's double height line
   (DECDHL), the first such row of a pair the top half, the next the bottom, as
   the Beeb pairs them.  A terminal that does not do UTF-8 gets Latin-1 or a near
   ASCII character. */

typedef struct {
   uint8_t fg, bg, held;
   bool    gfx, sep, hold, flash, conceal, dh;
} ttx_t;

static void ttx_init(ttx_t *t)
{
   memset(t, 0, sizeof *t);
   t->fg = 7u;
   t->held = ' ';
}

/* The byte MODE 7's screen holds for one the Beeb printed. */
static uint8_t ttx_store(uint8_t c)
{
   if (c >= 0xA0u)
      return (uint8_t)(c & 0x7Fu);              /* the chip ignores bit 7 from &A0 */
   switch (c) {
   case 35u: return 0x5Fu;                      /* # */
   case 95u: return 0x60u;                      /* _ (a long dash on the screen) */
   case 96u: return 0x23u;                      /* the pound */
   default:  return c;
   }
}

static uint32_t ttx_mosaic(uint8_t b)
{
   unsigned v = (b & 0x1Fu) | ((b & 0x40u) >> 1);   /* bits 0-5: the six cells, top left first */
   if (v == 0u)  return ' ';
   if (v == 63u) return 0x2588u;                /* full block */
   if (v == 21u) return 0x258Cu;                /* left half */
   if (v == 42u) return 0x2590u;                /* right half */
   return 0x1FB00u + (v - 1u) - (v > 21u ? 1u : 0u) - (v > 42u ? 1u : 0u);
}

static uint32_t ttx_alpha(uint8_t b)
{
   switch (b) {
   case 0x23u: return 0xA3u;                    /* pound */
   case 0x5Bu: return 0x2190u;                  /* left arrow */
   case 0x5Cu: return 0xBDu;                    /* one half */
   case 0x5Du: return 0x2192u;                  /* right arrow */
   case 0x5Eu: return 0x2191u;                  /* up arrow */
   case 0x5Fu: return '#';
   case 0x60u: return 0x2015u;                  /* long dash */
   case 0x7Bu: return 0xBCu;                    /* one quarter */
   case 0x7Cu: return 0x2016u;                  /* double bar */
   case 0x7Du: return 0xBEu;                    /* three quarters */
   case 0x7Eu: return 0xF7u;                    /* divide */
   case 0x7Fu: return 0x25A0u;                  /* block */
   default:    return (b >= 0x20u && b < 0x7Fu) ? b : ' ';
   }
}

/* One cell: what it shows (in *cp) with which colours, and the state after it. */
static void ttx_step(ttx_t *t, uint8_t b, uint32_t *cp, uint8_t *fg, uint8_t *bg, bool *flash)
{
   if (b >= 0x80u && b < 0xA0u) {
      switch (b) {                              /* set at: from this cell */
      case 0x89u: t->flash = false; break;
      case 0x8Cu: if (t->dh) { t->dh = false; t->held = ' '; } break;
      case 0x98u: t->conceal = true; break;
      case 0x99u: t->sep = false; break;
      case 0x9Au: t->sep = true; break;
      case 0x9Cu: t->bg = 0u; break;
      case 0x9Du: t->bg = t->fg; break;
      case 0x9Eu: t->hold = true; break;
      default: break;
      }
      *cp = (t->hold && t->gfx) ? ttx_mosaic(t->held) : ' ';
      *fg = t->fg; *bg = t->bg; *flash = t->flash;
      if (b >= 0x81u && b <= 0x87u) {           /* set after: from the next cell */
         t->fg = (uint8_t)(b & 7u);
         if (t->gfx) t->held = ' ';
         t->gfx = false;
         t->conceal = false;
      } else if (b >= 0x91u && b <= 0x97u) {
         t->fg = (uint8_t)(b & 7u);
         if (!t->gfx) t->held = ' ';
         t->gfx = true;
         t->conceal = false;
      } else if (b == 0x88u) {
         t->flash = true;
      } else if (b == 0x8Du) {
         if (!t->dh) t->held = ' ';
         t->dh = true;
      } else if (b == 0x9Fu) {
         t->hold = false;
      }
      return;
   }
   if (t->gfx && (b & 0x20u) != 0u) {
      *cp = ttx_mosaic(b);
      t->held = b;
   } else {
      *cp = ttx_alpha(b);                       /* in graphics, &40-&5F blast through as capitals */
   }
   if (t->conceal)
      *cp = ' ';
   *fg = t->fg; *bg = t->bg; *flash = t->flash;
}

/* The state at the start of cell x of a row. */
static void ttx_state(const tnd_out_t *s, unsigned y, unsigned x, ttx_t *t)
{
   uint32_t cp; uint8_t fg, bg; bool fl;
   ttx_init(t);
   for (unsigned i = 0u; i < x; i++)
      ttx_step(t, s->ch[y][i], &cp, &fg, &bg, &fl);
}

static size_t ttx_put_cp(const tnd_out_t *s, uint8_t *dst, uint32_t cp)
{
   if (cp < 0x80u) {
      dst[0] = (uint8_t)cp;
      return 1u;
   }
   if (!s->utf8) {
      if (cp <= 0xFFu) {
         dst[0] = (uint8_t)cp;
         return 1u;
      }
      switch (cp) {
      case 0x2190u: dst[0] = '<'; break;
      case 0x2192u: dst[0] = '>'; break;
      case 0x2191u: dst[0] = '^'; break;
      case 0x2015u: dst[0] = '-'; break;
      case 0x2016u: dst[0] = '|'; break;
      default:      dst[0] = '#'; break;      /* the block and the mosaics */
      }
      return 1u;
   }
   if (cp < 0x800u) {
      dst[0] = (uint8_t)(0xC0u | (cp >> 6));
      dst[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
      return 2u;
   }
   if (cp < 0x10000u) {
      dst[0] = (uint8_t)(0xE0u | (cp >> 12));
      dst[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
      dst[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
      return 3u;
   }
   dst[0] = (uint8_t)(0xF0u | (cp >> 18));
   dst[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
   dst[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
   dst[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
   return 4u;
}

/* The terminal's colours for a cell, if they are not what it has. */
static size_t ttx_sgr(uint8_t *tsgr, uint8_t *dst, uint8_t fg, uint8_t bg, bool flash)
{
   uint8_t key = (uint8_t)((fg & 7u) | ((bg & 7u) << 3) | (flash ? 0x40u : 0u));
   size_t  n = 0u;

   if (key == *tsgr)
      return 0u;
   *tsgr = key;
   if (fg == 7u && bg == 0u && !flash)
      return put_str(dst, "\x1b[0m");
   n += put_str(dst, "\x1b[0;3");
   dst[n++] = (uint8_t)('0' + (fg & 7u));
   if (bg != 0u) {
      n += put_str(dst + n, ";4");
      dst[n++] = (uint8_t)('0' + (bg & 7u));
   }
   if (flash)
      n += put_str(dst + n, ";5");
   dst[n++] = 'm';
   return n;
}

/* Cells x0 to x1 of row y, from the terminal's cursor on, *tsgr the colours the terminal
   has: at most room bytes.  Returns the bytes written; *done is the last cell drawn (x0 - 1
   if none fitted). */
static size_t ttx_cells(const tnd_out_t *s, uint8_t *tsgr, uint8_t *dst, unsigned y, unsigned x0, unsigned x1,
                        size_t room, unsigned *done)
{
   ttx_t    t;
   size_t   n = 0u;
   uint32_t cp; uint8_t fg, bg; bool fl;

   ttx_state(s, y, x0, &t);
   *done = x0 - 1u;
   for (unsigned x = x0; x <= x1; x++) {
      uint8_t buf[24];
      size_t  k;
      ttx_step(&t, s->ch[y][x], &cp, &fg, &bg, &fl);
      uint8_t save = *tsgr;
      k = ttx_sgr(tsgr, buf, fg, bg, fl);
      k += ttx_put_cp(s, buf + k, cp);
      if (n + k > room) {
         *tsgr = save;
         break;
      }
      memcpy(dst + n, buf, k);
      n += k;
      *done = x;
   }
   return n;
}

/* The last cell of row y that shows something (a character, or a background colour), or -1. */
static int ttx_row_end(const tnd_out_t *s, unsigned y)
{
   ttx_t t; uint32_t cp; uint8_t fg, bg; bool fl;
   int   end = -1;
   ttx_init(&t);
   for (unsigned x = 0u; x < s->cols; x++) {
      ttx_step(&t, s->ch[y][x], &cp, &fg, &bg, &fl);
      if (cp != ' ' || bg != 0u)
         end = (int)x;
   }
   return end;
}

/* Is any cell of row y a double height one?  And does it use hold graphics? */
static bool ttx_row_dh(const tnd_out_t *s, unsigned y)
{
   ttx_t t; uint32_t cp; uint8_t fg, bg; bool fl;
   ttx_init(&t);
   for (unsigned x = 0u; x < s->cols; x++) {
      ttx_step(&t, s->ch[y][x], &cp, &fg, &bg, &fl);
      if (t.dh)
         return true;
   }
   return false;
}

static bool ttx_row_hold(const tnd_out_t *s, unsigned y)
{
   for (unsigned x = 0u; x < s->cols; x++)
      if (s->ch[y][x] == 0x9Eu)
         return true;
   return false;
}

static size_t ttx_put_lattr(uint8_t *dst, uint8_t a)
{
   return put_str(dst, a == 3u ? "\x1b#3" : a == 4u ? "\x1b#4" : "\x1b#5");
}

/* Bring the terminal's line attributes into line with the model's rows, the cursor put back
   after.  Needs absolute positions.  A double height row is the top half, unless the row
   above is a top half, when it is the bottom. */
static size_t ttx_sync_lattr(tnd_out_t *s, uint8_t *dst, size_t room)
{
   size_t n = 0u;
   bool   moved = false;
   uint8_t prev = 0u;

   if (!s->tele || !s->sync || !s->origin_ok)
      return 0u;
   for (unsigned y = 0u; y < s->rows; y++) {
      uint8_t a = ttx_row_dh(s, y) ? (prev == 3u ? 4u : 3u) : 0u;
      prev = a;
      if (a == s->lattr[y])
         continue;
      if (n + 16u > room)
         break;
      n += put_pos(dst + n, 0u, y);
      n += ttx_put_lattr(dst + n, a);
      s->lattr[y] = a;
      moved = true;
   }
   if (moved)
      n += put_pos(dst + n, s->cx, s->cy);
   return n;
}

/* Back to single width and height on every row the terminal has a line attribute on
   (before the screen is cleared: a terminal may keep them through a clear). */
static size_t ttx_clear_lattr(tnd_out_t *s, uint8_t *dst)
{
   size_t n = 0u;
   for (unsigned y = 0u; y < TND_ROWS_MAX; y++)
      if (s->lattr[y] != 0u) {
         n += put_pos(dst + n, 0u, y);
         n += put_str(dst + n, "\x1b#5");
         s->lattr[y] = 0u;
      }
   return n;
}

/* MODE 7: the terminal's own colours again, if a cell left it with others, before anything that
   fills with the background colour (a clear, a scroll). */
static size_t ttx_plain(tnd_out_t *s, uint8_t *dst)
{
   if (!s->tele || s->tsgr == TTX_SGR_PLAIN)
      return 0u;
   s->tsgr = TTX_SGR_PLAIN;
   return put_str(dst, "\x1b[0m");
}

/* Put the terminal's colours back to what the Beeb has set, and its cursor to the Beeb's. */
static size_t put_finish(const tnd_out_t *s, uint8_t *dst)
{
   size_t n = put_pos(dst, s->cx, s->cy);
   n += put_str(dst + n, "\x1b[0m");
   if (s->coloured) {
      n += sgr(dst + n, '3', phys(s, s->fg));
      if (phys(s, s->bg) != 0u)
         n += sgr(dst + n, '4', phys(s, s->bg));
   }
   return n;
}

/* The terminal's colours set to the Beeb's: for a new terminal that is not being repainted. */
size_t tnd_out_colours(const tnd_out_t *s, uint8_t dst[16])
{
   size_t n = put_str(dst, "\x1b[0m");
   if (s->coloured) {
      n += sgr(dst + n, '3', phys(s, s->fg));
      if (phys(s, s->bg) != 0u)
         n += sgr(dst + n, '4', phys(s, s->bg));
   }
   return n;
}

static void scr_clear(tnd_out_t *s, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
   for (unsigned y = y0; y <= y1; y++)
      for (unsigned x = x0; x <= x1; x++) {
         s->ch[y][x] = ' ';
         s->at[y][x] = AT_KNOWN | AT_DEF;
      }
}

/* Teletext starts every row white: a colour set on the row above does not carry
   over.  Called when the cursor goes to another row. */
static size_t row_changed(tnd_out_t *s, uint8_t *dst)
{
   if (s->tele && s->coloured) {
      s->coloured = false;
      s->fg = 7u;
      return put_str(dst, "\x1b[0m");
   }
   return 0u;
}

static bool full_width(const tnd_out_t *s)
{
   return s->wl == 0u && s->wr == s->cols - 1u;
}

/* A rectangle the terminal now shows wrong, to be drawn again from the model. */
static void mark_dirty(tnd_out_t *s, unsigned l, unsigned t, unsigned r, unsigned b)
{
   if (!s->dirty) {
      s->dl = (uint8_t)l; s->dt = (uint8_t)t; s->dr = (uint8_t)r; s->db = (uint8_t)b;
      s->dirty = true;
      return;
   }
   if (l < s->dl) s->dl = (uint8_t)l;
   if (t < s->dt) s->dt = (uint8_t)t;
   if (r > s->dr) s->dr = (uint8_t)r;
   if (b > s->db) s->db = (uint8_t)b;
}

/* The text window scrolls up one line.  The terminal is told to as well when the
   window is as wide as the screen; a narrower one is drawn again (tnd_out_dirty_row). */
static size_t scr_scroll_up(tnd_out_t *s, uint8_t *dst)
{
   size_t n = 0u;

   for (unsigned y = s->wt; y < s->wb; y++)
      for (unsigned x = s->wl; x <= s->wr; x++) {
         s->ch[y][x] = s->ch[y + 1u][x];
         s->at[y][x] = s->at[y + 1u][x];
      }
   scr_clear(s, s->wl, s->wb, s->wr, s->wb);
   if (full_width(s)) {                          /* the terminal's lines move with their attributes */
      for (unsigned y = s->wt; y < s->wb; y++)
         s->lattr[y] = s->lattr[y + 1u];
      s->lattr[s->wb] = 0u;
   }
   if (s->hl_on) {
      if (s->hly > s->wt) s->hly--; else s->hl_on = false;
   }
   if (s->cc_on) {
      if (s->ccy > s->wt) s->ccy--; else s->cc_on = false;
   }
   if (s->sync && s->origin_ok && full_width(s)) {
      n += ttx_plain(s, dst + n);
      dst[n++] = 27u; dst[n++] = '[';
      n += put_num(dst + n, (unsigned)s->wt + 1u);
      dst[n++] = ';';
      n += put_num(dst + n, (unsigned)s->wb + 1u);
      dst[n++] = 'r';
      /* Index on the region's last row, not Scroll Up (ESC [ 1 S): Windows' telnet client ignores
         SU and garbles what follows it (the region reset came out as "[r").  The reset homes the
         cursor, as before. */
      n += put_pos(dst + n, 0u, s->wb);
      n += put_str(dst + n, "\x1b" "D\x1b[r");
      n += ttx_sync_lattr(s, dst + n, 200u);     /* a scroll can change which half a double row is */
   } else if (s->sync && s->origin_ok) {
      mark_dirty(s, s->wl, s->wt, s->wr, s->wb);
   }
   return n;
}

static size_t scr_scroll_down(tnd_out_t *s, uint8_t *dst)
{
   size_t n = 0u;

   for (unsigned y = s->wb; y > s->wt; y--)
      for (unsigned x = s->wl; x <= s->wr; x++) {
         s->ch[y][x] = s->ch[y - 1u][x];
         s->at[y][x] = s->at[y - 1u][x];
      }
   scr_clear(s, s->wl, s->wt, s->wr, s->wt);
   if (full_width(s)) {
      for (unsigned y = s->wb; y > s->wt; y--)
         s->lattr[y] = s->lattr[y - 1u];
      s->lattr[s->wt] = 0u;
   }
   if (s->hl_on) {
      if (s->hly < s->wb) s->hly++; else s->hl_on = false;
   }
   if (s->cc_on) {
      if (s->ccy < s->wb) s->ccy++; else s->cc_on = false;
   }
   if (s->sync && s->origin_ok && full_width(s)) {
      n += ttx_plain(s, dst + n);
      dst[n++] = 27u; dst[n++] = '[';
      n += put_num(dst + n, (unsigned)s->wt + 1u);
      dst[n++] = ';';
      n += put_num(dst + n, (unsigned)s->wb + 1u);
      dst[n++] = 'r';
      /* Reverse Index on the region's first row, not Scroll Down (ESC [ 1 T): see above. */
      n += put_pos(dst + n, 0u, s->wt);
      n += put_str(dst + n, "\x1bM\x1b[r");
      n += ttx_sync_lattr(s, dst + n, 200u);     /* a scroll can change which half a double row is */
   } else if (s->sync && s->origin_ok) {
      mark_dirty(s, s->wl, s->wt, s->wr, s->wb);
   }
   return n;
}

/* After a CR the terminal is at column 0: a text window's left edge may be further in. */
static size_t put_left(const tnd_out_t *s, uint8_t *dst)
{
   size_t n = 0u;
   if (s->wl != 0u) {
      dst[n++] = 27u; dst[n++] = '[';
      n += put_num(dst + n, s->wl);
      dst[n++] = 'C';
   }
   return n;
}

/* The cursor moves to the start of the next line, scrolling at the bottom. */
static size_t scr_newline(tnd_out_t *s, uint8_t *dst)
{
   size_t n = 0u;

   s->cx = s->wl;
   if (s->cy >= s->wb)
      n += scr_scroll_up(s, dst + n);
   else
      s->cy++;
   if (s->sync && s->origin_ok) {
      n += row_changed(s, dst + n);
      n += put_pos(dst + n, s->cx, s->cy);
   } else if (s->colok) {                        /* the column is known: a CR LF is the Beeb's wrap */
      dst[n++] = 13u;
      dst[n++] = 10u;
      n += put_left(s, dst + n);
      n += row_changed(s, dst + n);
   }
   return n;
}

/* A character the Beeb prints in MODE 7, the cursor known: stored as the screen holds it and
   drawn - the one cell, or for a control code (or over one, or in a row with hold graphics) the
   rest of the row - and the cursor moved on.  What does not fit is drawn later (the dirty rows). */
static size_t ttx_glyph(tnd_out_t *s, uint8_t c, uint8_t *dst)
{
   uint8_t  b = ttx_store(c);
   uint8_t  old = s->ch[s->cy][s->cx];
   bool     ctl = (b >= 0x80u && b < 0xA0u) || (old >= 0x80u && old < 0xA0u);
   unsigned end = s->cx, done;
   int      before = (ctl || ttx_row_hold(s, s->cy)) ? ttx_row_end(s, s->cy) : -1;
   size_t   n;

   s->ch[s->cy][s->cx] = b;
   s->drawn++;
   s->at[s->cy][s->cx] = AT_KNOWN | AT_DEF;
   if (ctl || (old != b && ttx_row_hold(s, s->cy))) {   /* the rest of the row, as far as it shows anything */
      int after = ttx_row_end(s, s->cy);
      int last = before > after ? before : after;
      if (last > (int)end)
         end = (unsigned)last;
   }
   n = 0u;
   if (b == 0x8Du || old == 0x8Du)              /* first: it leaves the cursor on this cell */
      n += ttx_sync_lattr(s, dst, 200u);
   n += ttx_cells(s, &s->tsgr, dst + n, s->cy, s->cx, end, TND_OUT_MAX - 160u - n, &done);
   if (done < end && s->origin_ok)
      mark_dirty(s, done + 1u, s->cy, end, s->cy);
   if (end > s->cx && s->cx < s->wr)            /* back to the cell after this one */
      n += put_col(dst + n, (unsigned)s->cx + 1u);
   if (s->cx < s->wr)
      s->cx++;
   else
      n += scr_newline(s, dst + n);
   return n;
}

/* ---- graphics: xterm's Tektronix 4014 mode -----------------------------------------
   MOVE, DRAW and PLOT (VDU 25) cannot be text, but xterm still has the storage tube
   terminal it grew out of: CSI ? 38 h puts it in Tektronix mode (its Tek window), GS
   starts a vector at the first point and draws to each one after, ESC FF clears the
   page, and ESC ETX goes back to the text screen.  The Beeb's 1280 x 1024 is the Tek's
   1024 x 780.  A storage tube cannot rub out, so drawing in the background colour or
   inverting (PLOT 2, 3, 6, 7) only moves.  Lines, points, triangles (outlined) and
   rectangles (outlined, the Master's PLOT 96) are drawn; the rest moves. */

void tnd_out_tek(tnd_out_t *s, bool on)
{
   s->tek = on;
}

static size_t tek_enter(tnd_out_t *s, uint8_t *dst)
{
   if (s->tek_on)
      return 0u;
   s->tek_on = true;
   return put_str(dst, "\x1b[?38h");
}

static size_t tek_leave(tnd_out_t *s, uint8_t *dst)
{
   if (!s->tek_on)
      return 0u;
   s->tek_on = false;
   s->tsgr = 0xFFu;
   return put_str(dst, "\x1f\x1b\x03");         /* US first: xterm drops the vectors not yet drawn otherwise */
}

static size_t tek_xy(uint8_t *dst, int x, int y)
{
   int tx = x * 1024 / 1280, ty = y * 780 / 1024;
   if (tx < 0) tx = 0;
   if (tx > 1023) tx = 1023;
   if (ty < 0) ty = 0;
   if (ty > 779) ty = 779;
   dst[0] = (uint8_t)(0x20 | ((ty >> 5) & 0x1F));
   dst[1] = (uint8_t)(0x60 | (ty & 0x1F));
   dst[2] = (uint8_t)(0x20 | ((tx >> 5) & 0x1F));
   dst[3] = (uint8_t)(0x40 | (tx & 0x1F));
   return 4u;
}

/* A polyline through n points, as one GS vector. */
static size_t tek_lines(tnd_out_t *s, uint8_t *dst, const int16_t *xy, unsigned n)
{
   size_t k = tek_enter(s, dst);
   dst[k++] = 0x1Du;
   for (unsigned i = 0u; i < n; i++)
      k += tek_xy(dst + k, xy[2u * i], xy[2u * i + 1u]);
   return k;
}

static size_t tek_plot(tnd_out_t *s, uint8_t *dst)
{
   uint8_t  k = s->p[0];
   int16_t  x = (int16_t)(s->p[1] | (s->p[2] << 8)), y = (int16_t)(s->p[3] | (s->p[4] << 8));
   unsigned type = k & 0xF8u, how = k & 3u;
   size_t   n = 0u;

   if ((k & 4u) != 0u) {                        /* absolute, from the graphics origin */
      x = (int16_t)(x + s->ox);
      y = (int16_t)(y + s->oy);
   } else {                                     /* relative to the graphics cursor */
      x = (int16_t)(x + s->gx);
      y = (int16_t)(y + s->gy);
   }
   if (s->tek && how == 1u) {                   /* drawn in the foreground: the only kind a tube shows */
      if (type < 64u) {                         /* the lines (solid, dotted, dashed, with or without the end) */
         int16_t v[4] = { s->gx, s->gy, x, y };
         if (type == 16u || type == 24u) {      /* PLOT 16-31: dotted, in the tube's dotted style */
            n = tek_enter(s, dst);
            n += put_str(dst + n, "\x1b" "a\x1d");
            n += tek_xy(dst + n, v[0], v[1]);
            n += tek_xy(dst + n, v[2], v[3]);
            n += put_str(dst + n, "\x1b`");
         } else {
            n = tek_lines(s, dst, v, 2u);
         }
      } else if (type == 64u) {                 /* a point */
         int16_t v[4] = { x, y, x, y };
         n = tek_lines(s, dst, v, 2u);
      } else if (type == 80u) {                 /* a triangle, outlined */
         int16_t v[8] = { s->g1x, s->g1y, s->gx, s->gy, x, y, s->g1x, s->g1y };
         n = tek_lines(s, dst, v, 4u);
      } else if (type == 96u) {                 /* a rectangle (Master), outlined */
         int16_t v[10] = { s->gx, s->gy, x, s->gy, x, y, s->gx, y, s->gx, s->gy };
         n = tek_lines(s, dst, v, 5u);
      }
   }
   s->g2x = s->g1x; s->g2y = s->g1y;
   s->g1x = s->gx;  s->g1y = s->gy;
   s->gx = x;       s->gy = y;
   return n;
}

static size_t tek_clear(tnd_out_t *s, uint8_t *dst)
{
   size_t n;
   if (!s->tek)
      return 0u;
   n = tek_enter(s, dst);
   n += put_str(dst + n, "\x1b\x0c");
   return n;
}

static size_t finish_code(tnd_out_t *s, uint8_t dst[TND_OUT_MAX])
{
   uint8_t code = s->code;
   size_t  n = 0u;

   s->code = 0u;
   switch (code) {
   case 17:                                     /* COLOUR: 0-127 text, 128-255 background */
      if (s->tele && s->sync)                    /* MODE 7 has no COLOUR: the control codes do it */
         break;
      if (s->p[0] < 128u) {
         s->fg = (uint8_t)(s->p[0] & (s->ncol - 1u));
         n = sgr(dst, '3', phys(s, s->fg));
      } else {
         s->bg = (uint8_t)((s->p[0] - 128u) & (s->ncol - 1u));
         n = sgr(dst, '4', phys(s, s->bg));
      }
      s->coloured = true;
      s->tsgr = 0xFFu;
      break;
   case 19:                                     /* palette: logical colour p0 shows as physical p1 */
      if (!s->tele) {
         s->pal[s->p[0] & (s->ncol - 1u)] = (uint8_t)(s->p[1] & 7u);
         if (s->coloured) {                     /* the colours already set may have changed */
            n += sgr(dst + n, '3', phys(s, s->fg));
            n += sgr(dst + n, '4', phys(s, s->bg));
         }
      }
      break;
   case 22:                                     /* MODE: the screen is cleared */
      if (s->coloured)
         n += put_str(dst + n, "\x1b[0m");
      else
         n += ttx_plain(s, dst + n);
      n += ttx_clear_lattr(s, dst + n);          /* teletext double height lines back to single */
      if (s->tek) {                              /* the graphics go with the screen */
         n += tek_clear(s, dst + n);
         n += tek_leave(s, dst + n);
      }
      s->gx = s->gy = s->ox = s->oy = 0;
      tnd_out_set_mode(s, s->p[0]);
      s->sync = true;
      s->colok = true;
      s->origin_ok = true;
      s->coloured = false;
      n += put_str(dst + n, "\x1b[2J\x1b[H");
      break;
   case 25:                                     /* PLOT k,x,y */
      n = tek_plot(s, dst);
      break;
   case 29:                                     /* the graphics origin */
      s->ox = (int16_t)(s->p[0] | (s->p[1] << 8));
      s->oy = (int16_t)(s->p[2] | (s->p[3] << 8));
      break;
   case 23:                                     /* VDU 23,1,n: the cursor off (0) or on */
      if (s->p[0] == 1u) {
         s->curoff = (s->p[1] & 1u) == 0u;
         n = put_str(dst, s->curoff ? "\x1b[?25l" : "\x1b[?25h");
      }
      break;
   case 28: {                                   /* text window: left, bottom, right, top */
      unsigned l = s->p[0], b = s->p[1], r = s->p[2], t = s->p[3];
      if (l <= r && t <= b && r < s->cols && b < s->rows) {
         s->wl = (uint8_t)l; s->wb = (uint8_t)b; s->wr = (uint8_t)r; s->wt = (uint8_t)t;
         s->cx = s->wl; s->cy = s->wt;
         s->sync = true;
         s->colok = true;
         n += row_changed(s, dst + n);
         n += abs_pos(s, dst + n, s->cx, s->cy);
      }
      break;
   }
   case 31: {                                   /* TAB(x,y), relative to the text window */
      unsigned x = (unsigned)s->wl + s->p[0], y = (unsigned)s->wt + s->p[1];
      if (x <= s->wr && y <= s->wb) {
         s->cx = (uint8_t)x; s->cy = (uint8_t)y;
         s->sync = true;
         s->colok = true;
         n += row_changed(s, dst + n);
         n += abs_pos(s, dst + n, s->cx, s->cy);
      }
      break;
   }
   default:                                     /* the rest take parameters we ignore */
      break;
   }
   return n;
}

/* A character the Beeb prints, a cell at a time: what the terminal gets, what is
   remembered for the cell, and the colour it sets (teletext control codes). */
static size_t glyph(tnd_out_t *s, uint8_t c, uint8_t *dst, uint8_t *stored, int *newfg)
{
   size_t n;

   *newfg = -1;
   if (c < 127u) {
      *stored = c;
      return put_glyph(s, dst, c);
   }
   /* 128 and up.  Mode 7: 0x81-0x87 and 0x91-0x97 are colour codes, shown as a
      space; the other controls are spaces too (and in the other modes all of them:
      there they are the user-defined characters, with no colour of their own).  From 0xA0 the teletext chip
      ignores bit 7 and shows the character. */
   if (c < 0xA0u) {
      uint8_t colour = (uint8_t)(c & 0x0Fu);   /* 0x81-0x87 alpha, 0x91-0x97 graphics */
      n = 0u;
      if (s->tele && colour >= 1u && colour <= 7u && (c == (0x80u | colour) || c == (0x90u | colour))) {
         n = sgr(dst, '3', colour);
         *newfg = colour;
      }
      dst[n++] = ' ';
      *stored = ' ';
      return n;
   }
   {
      uint8_t ch = (uint8_t)(c & 0x7Fu);
      if (ch == 127u || ch < 32u)
         ch = ' ';
      *stored = ch;
      return put_glyph(s, dst, ch);
   }
}

/* VDU 8: the cursor left; from the window's left edge to the right end of the line above, and
   from its top left corner the window scrolls down first (the MOS's VDU 8 then VDU 11). */
static size_t cursor_back(tnd_out_t *s, uint8_t *dst)
{
   size_t n = 0u;

   if (s->cx > s->wl) {
      s->cx--;
      dst[n++] = 8u;
      return n;
   }
   s->cx = s->wr;
   if (s->cy > s->wt) {
      s->cy--;
      if (s->sync && s->origin_ok)
         n += row_changed(s, dst + n);
   } else {
      n += scr_scroll_down(s, dst + n);
   }
   if (s->sync && s->origin_ok)
      n += put_pos(dst + n, s->cx, s->cy);
   else
      dst[n++] = 8u;
   return n;
}

static size_t out_byte(tnd_out_t *s, uint8_t c, uint8_t *dst)
{
   size_t n = 0u;


   if (s->code != 0u) {                         /* a parameter byte of a VDU code */
      if (s->got < sizeof s->p)
         s->p[s->got] = c;
      s->got++;
      if (s->got >= vdu_params(s->code)) {
         if (s->disabled) {                     /* collected under VDU 21: counted, not acted on */
            s->code = 0u;
            return 0u;
         }
         return finish_code(s, dst);
      }
      return 0u;
   }

   if (s->disabled) {                           /* VDU 21: only VDU 6 gets through, but the MOS */
      if (c == 6u)                              /* still counts the other codes' parameters, so a */
         s->disabled = false;                   /* 6 among them does not end it (as the banner reader) */
      else if (c < 32u && vdu_params(c) != 0u) {
         s->code = c;
         s->got = 0u;
      }
      return 0u;
   }

   if (c < 32u) {
      switch (c) {
      case 6:
         break;
      case 7:  dst[n++] = 7u; break;
      case 14: s->paged = true;  break;          /* paged mode: the MOS waits for SHIFT at each page */
      case 15: s->paged = false; break;
      case 8:                                   /* cursor left; at the edge, the end of the line above */
         n += cursor_back(s, dst);
         break;
      case 9:                                   /* cursor right; at the edge, the start of the next line */
         if (s->cx < s->wr) {
            s->cx++;
            n = put_str(dst, "\x1b[C");
         } else if (s->sync && s->origin_ok) {
            n += scr_newline(s, dst + n);
         } else {
            n = put_str(dst, "\x1b[C");
         }
         break;
      case 10:                                  /* down; at the bottom the window scrolls */
         if (s->cy >= s->wb) {
            n += scr_scroll_up(s, dst + n);
            if (s->sync && s->origin_ok)
               n += put_pos(dst + n, s->cx, s->cy);
            else
               dst[n++] = 10u;
         } else {
            s->cy++;
            dst[n++] = 10u;
            if (s->sync)
               n += row_changed(s, dst + n);
         }
         break;
      case 11:                                  /* up; at the top the window scrolls down */
         if (s->cy <= s->wt) {
            n += scr_scroll_down(s, dst + n);
            if (s->sync && s->origin_ok)
               n += put_pos(dst + n, s->cx, s->cy);
            else
               n += put_str(dst + n, "\x1b[A");
         } else {
            s->cy--;
            n = put_str(dst, "\x1b[A");
            if (s->sync)
               n += row_changed(s, dst + n);
         }
         break;
      case 12:                                  /* CLS: the window is cleared, the cursor to its corner */
         s->hl_on = s->cc_on = false;
         s->npend = 0u;
         s->colok = true;
         if (full_width(s) && s->wt == 0u && s->wb == s->rows - 1u) {
            scr_clear(s, 0u, 0u, (unsigned)s->cols - 1u, (unsigned)s->rows - 1u);
            s->sync = true;
            s->dirty = false;
            s->cx = s->wl; s->cy = s->wt;
            s->origin_ok = true;
            n = ttx_plain(s, dst);
            n += ttx_clear_lattr(s, dst + n);
            n += put_str(dst + n, "\x1b[2J\x1b[H");
         } else if (s->sync && s->origin_ok) {   /* only the window: drawn again, nothing else touched */
            scr_clear(s, s->wl, s->wt, s->wr, s->wb);
            s->cx = s->wl; s->cy = s->wt;
            mark_dirty(s, s->wl, s->wt, s->wr, s->wb);
            n = put_pos(dst, s->cx, s->cy);
         } else {                                /* the terminal's rows are not the Beeb's yet: start them */
            scr_clear(s, s->wl, s->wt, s->wr, s->wb);
            s->cx = s->wl; s->cy = s->wt;
            s->origin_ok = true;
            n = put_str(dst, "\x1b[2J");
            if (s->sync)                         /* what is outside the window comes back */
               mark_dirty(s, 0u, 0u, (unsigned)s->cols - 1u, (unsigned)s->rows - 1u);
            n += put_pos(dst + n, s->cx, s->cy);
         }
         if (s->tele)
            s->coloured = false;
         break;
      case 13:
         s->cx = s->wl;
         s->colok = true;
         if (s->coloured && s->tele) {          /* teletext colour ends with the line; COLOUR does not */
            n = put_str(dst, "\x1b[0m");
            s->coloured = false;
            s->fg = 7u;
            s->bg = 0u;
         }
         dst[n++] = 13u;
         n += put_left(s, dst + n);             /* to the window's left edge */
         break;
      case 20:                                  /* the default colours and palette */
         if (s->tele && s->sync)
            break;
         scr_default_palette(s);
         s->fg = (uint8_t)(s->ncol - 1u);
         s->bg = 0u;
         if (s->coloured) {
            n = put_str(dst, "\x1b[0m");
            s->coloured = false;
            s->tsgr = 0xFFu;
         }
         break;
      case 21: s->disabled = true; break;
      case 26:                                  /* default windows, cursor home */
         s->wl = 0u; s->wt = 0u;
         s->wr = (uint8_t)(s->cols - 1u);
         s->wb = (uint8_t)(s->rows - 1u);
         s->cx = 0u; s->cy = 0u;
         s->sync = true;
         s->colok = true;
         n += row_changed(s, dst + n);
         n += abs_pos(s, dst + n, 0u, 0u);
         break;
      case 30:                                  /* home: the corner of the text window */
         s->cx = s->wl; s->cy = s->wt;
         s->sync = true;
         s->colok = true;
         n += row_changed(s, dst + n);
         n += abs_pos(s, dst + n, s->cx, s->cy);
         break;
      default:
         if (vdu_params(c) != 0u) {
            s->code = c;
            s->got = 0u;
         }
         break;
      }
      return n;
   }

   if (c == 127u && s->tele && s->sync && s->cx > s->wl) {   /* DELETE in MODE 7: the cell drawn again as a space */
      uint8_t  old;
      unsigned end, done;
      int      before;
      s->cx--;
      old = s->ch[s->cy][s->cx];
      before = ttx_row_end(s, s->cy);
      s->ch[s->cy][s->cx] = ' ';
      s->drawn++;
      s->at[s->cy][s->cx] = AT_KNOWN | AT_DEF;
      end = s->cx;
      if (old >= 0x80u && old < 0xA0u && before > (int)end)
         end = (unsigned)before;
      dst[n++] = 8u;
      n += ttx_cells(s, &s->tsgr, dst + n, s->cy, s->cx, end, TND_OUT_MAX - 160u, &done);
      if (done < end && s->origin_ok)
         mark_dirty(s, done + 1u, s->cy, end, s->cy);
      n += put_col(dst + n, s->cx);
      if (old == 0x8Du)
         n += ttx_sync_lattr(s, dst + n, TND_OUT_MAX - 60u - n);
      return n;
   }

   if (c == 127u) {                             /* DELETE: back, blank, back */
      if (s->cx == s->wl) {                     /* from the start of a line: the end of the one above */
         n = cursor_back(s, dst);
         s->ch[s->cy][s->cx] = ' ';
         s->drawn++;
         s->at[s->cy][s->cx] = AT_KNOWN | cur_attr(s);
         if (s->sync && s->origin_ok) {
            dst[n++] = ' ';
            n += put_pos(dst + n, s->cx, s->cy);
            return n;
         }
         dst[n++] = ' ';
         dst[n++] = 8u;
         return n;
      }
      s->cx--;
      s->ch[s->cy][s->cx] = ' ';
      s->drawn++;
      s->at[s->cy][s->cx] = AT_KNOWN | cur_attr(s);
      dst[0] = 8u; dst[1] = ' '; dst[2] = 8u;
      return 3u;
   }

   if (s->tele && s->sync)
      return ttx_glyph(s, c, dst);
   {
      uint8_t stored;
      int     newfg;
      uint8_t at = cur_attr(s);

      n = glyph(s, c, dst, &stored, &newfg);
      s->ch[s->cy][s->cx] = stored;
      s->drawn++;
      s->at[s->cy][s->cx] = AT_KNOWN | at;
      if (newfg >= 0) {
         s->fg = (uint8_t)newfg;
         s->coloured = true;
         s->tsgr = 0xFFu;
      }
      if (s->cx < s->wr)
         s->cx++;
      else                                       /* the last column: on to the next line */
         n += scr_newline(s, dst + n);
      return n;
   }
}

/* Graphics first: anything that is not one of the graphics codes takes the terminal back
   from its Tek window to the text screen, and CLG (VDU 16) clears the Tek page. */
/* ---- a screen read compared with the model (tnd_out_scan_merge) -------------
   The ROM's read is VDU 26, VDU 12, then VDU 31,x,y,char for each cell that is not blank, then
   VDU 28 with the Beeb's window and VDU 31,x,y with its cursor. */

void tnd_out_scan_merge(tnd_out_t *s)
{
   s->smerge = true;
   s->rescan = false;
   s->sm_st = 0u;
   s->sm_n = 0u;
}

/* What the model keeps for a character the read reports (as the printing paths store it). */
static uint8_t merge_char(const tnd_out_t *s, uint8_t c)
{
   if (c == 0u)
      return ' ';
   if (s->tele)                                 /* the read gives the code printed: stored as printing stores it */
      return ttx_store(c);
   if (c < 127u)
      return c;
   if (c < 0xA0u)
      return ' ';
   c &= 0x7Fu;
   return (c == 127u || c < 32u) ? (uint8_t)' ' : c;
}

static uint8_t model_char(const tnd_out_t *s, unsigned x, unsigned y)
{
   uint8_t c = (s->at[y][x] & AT_KNOWN) != 0u ? s->ch[y][x] : (uint8_t)' ';
   return c == 0u ? (uint8_t)' ' : c;
}

/* The read reports a blank where the model has a character on a coloured background: OSBYTE 135
   matches a cell's pixels against the font in the foreground colour, so text on a background (an
   inverse heading, COLOUR 0 on COLOUR 129) reads as nothing.  The model's cell stands. */
static bool merge_unreadable(const tnd_out_t *s, uint8_t ch, uint8_t at, uint8_t want)
{
   return !s->tele && want == ' ' && ch != 0u && ch != ' ' && (at & AT_KNOWN) != 0u
          && (at & AT_DEF) == 0u && ((at >> 3) & 7u) != 0u;
}

static size_t merge_end(tnd_out_t *s, uint8_t *dst)
{
   static uint8_t och[TND_ROWS_MAX][TND_COLS_MAX], oat[TND_ROWS_MAX][TND_COLS_MAX];
   bool   differ = false;
   size_t n;

   for (unsigned y = 0u; y < s->rows && !differ; y++)
      for (unsigned x = 0u; x < s->cols; x++)
         if (model_char(s, x, y) != merge_char(s, s->sh[y][x])
             && !merge_unreadable(s, s->ch[y][x], s->at[y][x], merge_char(s, s->sh[y][x]))) {
            differ = true;
            break;
         }
   if (!differ) {                                  /* the terminal is right already, colours and all, */
      const uint8_t *t = s->sm_tail;              /* but the Beeb's window or cursor may not be */
      if (s->sm_n == 8u && s->sync && t[1] == s->wl && t[2] == s->wb && t[3] == s->wr && t[4] == s->wt
          && (unsigned)s->wl + t[6] == s->cx && (unsigned)s->wt + t[7] == s->cy)
         return 0u;
      n = 0u;
      for (unsigned i = 0u; i < s->sm_n; i++)
         n += out_byte(s, s->sm_tail[i], dst + n);
      return n;
   }
   memcpy(och, s->ch, sizeof och);
   memcpy(oat, s->at, sizeof oat);
   for (unsigned y = 0u; y < s->rows; y++)       /* the cells as they were, for those that agree */
      for (unsigned x = 0u; x < s->cols; x++)
         if ((oat[y][x] & AT_KNOWN) == 0u)
            och[y][x] = ' ';
   n = out_byte(s, 26u, dst);                     /* the model and the terminal cleared, as a read does */
   n += out_byte(s, 12u, dst + n);
   for (unsigned y = 0u; y < s->rows; y++)
      for (unsigned x = 0u; x < s->cols; x++) {
         uint8_t want = merge_char(s, s->sh[y][x]);
         if ((och[y][x] == 0u ? (uint8_t)' ' : och[y][x]) == want || merge_unreadable(s, och[y][x], oat[y][x], want)) {
            s->ch[y][x] = och[y][x];
            s->at[y][x] = oat[y][x];
         } else {
            s->ch[y][x] = want;
            s->at[y][x] = AT_KNOWN | AT_DEF;
         }
      }
   n += ttx_sync_lattr(s, dst + n, 200u);        /* MODE 7: the double-height rows */
   for (unsigned i = 0u; i < s->sm_n; i++)        /* the Beeb's window and cursor */
      n += out_byte(s, s->sm_tail[i], dst + n);
   mark_dirty(s, 0u, 0u, (unsigned)s->cols - 1u, (unsigned)s->rows - 1u);
   return n;
}

static size_t merge_byte(tnd_out_t *s, uint8_t c, uint8_t *dst)
{
   switch (s->sm_st) {
   case 0u: if (c == 26u) { s->sm_st = 1u; return 0u; } break;
   case 1u:
      if (c == 12u) {
         memset(s->sh, ' ', sizeof s->sh);
         s->sm_st = 2u;
         return 0u;
      }
      break;
   case 2u:
      if (c == 31u) { s->sm_st = 3u; return 0u; }
      if (c == 28u) { s->sm_tail[0] = c; s->sm_n = 1u; s->sm_st = 6u; return 0u; }
      break;
   case 3u: if (c < s->cols) { s->sm_x = c; s->sm_st = 4u; return 0u; } break;
   case 4u: if (c < s->rows) { s->sm_y = c; s->sm_st = 5u; return 0u; } break;
   case 5u: s->sh[s->sm_y][s->sm_x] = c; s->sm_st = 2u; return 0u;
   case 6u:
      s->sm_tail[s->sm_n++] = c;
      if (s->sm_n == 5u)
         s->sm_st = 7u;
      return 0u;
   case 7u: if (c == 31u) { s->sm_tail[s->sm_n++] = c; s->sm_st = 8u; return 0u; } break;
   case 8u: s->sm_tail[s->sm_n++] = c; s->sm_st = 9u; return 0u;
   default:
      s->sm_tail[s->sm_n++] = c;
      s->smerge = false;
      return merge_end(s, dst);
   }
   /* Not a screen read after all, or one cut short: as usual from here (a cut-short read asks
      for an ordinary one: the model has not been checked). */
   s->smerge = false;
   if (s->sm_st >= 1u)
      s->rescan = true;
   return tnd_out_byte(s, c, dst);
}

size_t tnd_out_byte(tnd_out_t *s, uint8_t c, uint8_t dst[TND_OUT_MAX])
{
   size_t n = 0u;

   if (s->smerge)
      return merge_byte(s, c, dst);

   if (s->code == 0u && !s->disabled) {
      if (c == 16u)
         return tek_clear(s, dst);
      if (s->tek_on && c != 18u && c != 25u && c != 29u)
         n = tek_leave(s, dst);
   }
   return n + out_byte(s, c, dst + n);
}

/* ---- the copy cursor -------------------------------------------------------
   The BBC's cursor editing has two cursors: the one where typing goes, which is
   the terminal's cursor, and the copy cursor that the cursor keys move over the
   text and COPY copies from.  The MOS moves the second one itself and never
   sends it down the serial line, so the translator follows it from the keys the
   daemon sends and shows it as a reversed cell.  The keys are queued and acted
   on once the Beeb has been quiet for a moment, so that it is the write cursor
   after the echo of whatever was typed before. */

#define COPY_QUIET_US 40000u

bool tnd_out_blank(const tnd_out_t *s)
{
   for (unsigned y = 0u; y < s->rows; y++)
      for (unsigned x = 0u; x < s->cols; x++)
         if ((s->at[y][x] & AT_KNOWN) != 0u && s->ch[y][x] != ' ')
            return false;
   return true;
}

size_t tnd_out_cursor_line(const tnd_out_t *s, char *dst, size_t max)
{
   size_t n = 0u;
   bool   any = false;

   if (!s->sync || s->cy >= s->rows || s->cx > s->cols)
      return 0u;
   for (unsigned x = s->wl; x < s->cx && n + 1u < max; x++) {
      uint8_t c = s->ch[s->cy][x];
      if ((s->at[s->cy][x] & AT_KNOWN) == 0u)
         return 0u;
      if (s->tele)
         c = (c >= 0x80u && c < 0xA0u) ? (uint8_t)' ' : (uint8_t)(c & 0x7Fu);
      if (c < 32u || c >= 127u)
         c = ' ';
      if (c != ' ')
         any = true;
      dst[n++] = (char)c;
   }
   if (!any)
      return 0u;
   dst[n] = 0;
   return n;
}

bool tnd_out_has_line(const tnd_out_t *s, const char *text, size_t len)
{
   if (len == 0u || len > s->cols)
      return false;
   for (unsigned y = 0u; y < s->rows; y++) {
      unsigned x = 0u;
      while (x < len && s->ch[y][x] == (uint8_t)text[x] && (s->at[y][x] & AT_KNOWN) != 0u)
         x++;
      if (x != len)
         continue;
      while (x < s->cols && s->ch[y][x] == ' ')
         x++;
      if (x == s->cols)
         return true;
   }
   return false;
}

void tnd_out_origin_lost(tnd_out_t *s)
{
   s->tsgr = s->coloured ? 0xFFu : TTX_SGR_PLAIN;
   s->origin_ok = false;
   s->dirty = false;
   s->colok = true;                              /* the cursor's column is the Beeb's */
   s->hl_on = s->cc_on = false;
   s->npend = 0u;
}

size_t tnd_out_dirty_row(tnd_out_t *s, uint8_t dst[TND_REPAINT_MAX])
{
   size_t n = 0u;

   if (!s->dirty)
      return 0u;
   if (!s->sync || !s->origin_ok || s->dt > s->db || s->dl > s->dr || s->db >= s->rows || s->dr >= s->cols) {
      s->dirty = false;
      return 0u;
   }
   unsigned y = s->dt;
   uint8_t  prev = AT_DEF;
   n += put_pos(dst, s->dl, y);
   n += put_str(dst + n, "\x1b[0m");
   if (s->tele) {
      unsigned done;
      s->tsgr = TTX_SGR_PLAIN;
      n += ttx_cells(s, &s->tsgr, dst + n, y, s->dl, s->dr, TND_REPAINT_MAX - 80u - n, &done);
      n += put_str(dst + n, "\x1b[0m");
      if (s->hl_on && s->hly == y && s->hlx >= s->dl && s->hlx <= s->dr)
         s->hl_on = false;
      n += put_finish(s, dst + n);
      s->tsgr = s->coloured ? 0xFFu : TTX_SGR_PLAIN;
      if (y >= s->db)
         s->dirty = false;
      else
         s->dt = (uint8_t)(y + 1u);
      return n;
   }
   for (unsigned x = s->dl; x <= s->dr; x++) {
      uint8_t at = (s->at[y][x] & AT_KNOWN) != 0u ? (uint8_t)(s->at[y][x] & ~AT_KNOWN) : AT_DEF;
      if (n + 24u > TND_REPAINT_MAX - 48u)
         break;                                  /* a row of colour changes at every cell: cut it off */
      if (at != prev) {
         n += put_attr(s, dst + n, at);
         prev = at;
      }
      n += put_glyph(s, dst + n, s->ch[y][x]);
   }
   n += put_str(dst + n, "\x1b[0m");
   if (s->hl_on && s->hly == y && s->hlx >= s->dl && s->hlx <= s->dr)
      s->hl_on = false;                          /* the copy cursor's mark was drawn over */
   n += put_finish(s, dst + n);                  /* the Beeb's cursor and colours, for what comes next */
   if (y >= s->db)
      s->dirty = false;
   else
      s->dt = (uint8_t)(y + 1u);
   return n;
}

void tnd_out_copykey(tnd_out_t *s, uint8_t key)
{
   if (!s->sync)
      return;
   if (s->npend < sizeof s->pend)
      s->pend[s->npend++] = key;
}

static void cc_apply(tnd_out_t *s, uint8_t op)
{
   if (op == 13u || op == 27u) {                /* RETURN or ESCAPE ends the editing */
      s->cc_on = false;
      return;
   }
   if (!s->cc_on && op == 139u)                 /* COPY with no copy cursor: the MOS only beeps */
      return;
   if (!s->cc_on && !s->cc_exact) {
      /* The MOS edits with the copy cursor only inside a line being typed (OSWORD 0), which
         always has something on the cursor's row first: BASIC's ">", INPUT's "?", the text.
         A cursor at the start of its row is a program printing - a listing waiting in paged
         mode for SHIFT - and the key just waits in the Beeb's buffer: no mark is started. */
      if (s->cx <= s->wl)
         return;
      /* In paged mode a listing also waits for SHIFT where a long line wraps, its cursor anywhere
         on the row; a line being typed there starts with BASIC's ">" or INPUT's "?". */
      if (s->paged) {
         uint8_t first = (s->at[s->cy][s->wl] & AT_KNOWN) != 0u ? s->ch[s->cy][s->wl] : (uint8_t)' ';
         if (first != '>' && first != '?')
            return;
      }
   }
   if (!s->cc_on) {
      s->cc_on = true;
      s->ccx = s->cx;
      s->ccy = s->cy;
   }
   /* As the MOS moves it (OS 1.20 &D8F0): VDU 8 to 11 with scrolling off, so it wraps inside
      the text window - off the top to the bottom row and back (&CD3F), off either side to the
      other side of the row above or below. */
   switch (op) {
   case 143u:                                   /* up */
      s->ccy = s->ccy > s->wt ? (uint8_t)(s->ccy - 1u) : s->wb;
      break;
   case 142u:                                   /* down */
      s->ccy = s->ccy < s->wb ? (uint8_t)(s->ccy + 1u) : s->wt;
      break;
   case 140u:                                   /* left */
      if (s->ccx > s->wl) {
         s->ccx--;
      } else {
         s->ccx = s->wr;
         s->ccy = s->ccy > s->wt ? (uint8_t)(s->ccy - 1u) : s->wb;
      }
      break;
   case 141u:                                   /* right, and COPY: it moves on past what it copied */
   case 139u:
      if (s->ccx < s->wr) {
         s->ccx++;
      } else {
         s->ccx = s->wl;
         s->ccy = s->ccy < s->wb ? (uint8_t)(s->ccy + 1u) : s->wt;
      }
      break;
   default:
      break;
   }
}

/* One teletext cell as the chip shows it, reversed for the copy cursor's mark. */
static size_t ttx_mark(const tnd_out_t *s, uint8_t *dst, unsigned x, unsigned y, bool reverse)
{
   ttx_t    t;
   uint32_t cp; uint8_t fg, bg; bool fl;
   uint8_t  tsgr = 0xFFu;
   size_t   n;

   ttx_state(s, y, x, &t);
   ttx_step(&t, s->ch[y][x], &cp, &fg, &bg, &fl);
   n = ttx_sgr(&tsgr, dst, fg, bg, fl);
   if (reverse)
      n += put_str(dst + n, "\x1b[7m");
   n += ttx_put_cp(s, dst + n, cp);
   return n;
}

size_t tnd_out_redraw(tnd_out_t *s, uint8_t dst[TND_COPY_MAX])
{
   size_t n;

   if (!s->sync)                                 /* the cursor is not known: nothing to draw by */
      return 0u;
   n = put_str(dst, "\x1b[0m\x1b[2J");
   s->origin_ok = true;
   s->tsgr = s->coloured ? 0xFFu : TTX_SGR_PLAIN;
   s->hl_on = false;
   if (s->tele) {
      memset(s->lattr, 0xFF, s->rows);             /* not known after the clear: all sent again */
      n += ttx_sync_lattr(s, dst + n, TND_COPY_MAX - 16u - n);
   }
   mark_dirty(s, 0u, 0u, (unsigned)s->cols - 1u, (unsigned)s->rows - 1u);
   n += put_pos(dst + n, s->cx, s->cy);
   return n;
}

size_t tnd_out_copyflush(tnd_out_t *s, uint32_t now_us, uint8_t dst[TND_COPY_MAX])
{
   size_t n = 0u;

   if (s->npend == 0u)
      return 0u;
   if (!s->sync) {                               /* the marks are drawn where the model says */
      s->npend = 0u;
      return 0u;
   }
   if ((uint32_t)(now_us - s->last_us) < COPY_QUIET_US)
      return 0u;
   if (!s->origin_ok)       /* the terminal's rows are not the Beeb's (drawn under the greeting) */
      return tnd_out_redraw(s, dst);              /* drawn again first, and the mark after */
   if (s->dirty)                                 /* rows still to draw: the mark after them */
      return 0u;

   for (unsigned i = 0u; i < s->npend; i++)
      cc_apply(s, s->pend[i]);
   s->npend = 0u;

   if (s->hl_on) {                              /* the old mark goes: the cell as it was */
      n += put_pos(dst + n, s->hlx, s->hly);
      if (s->tele) {
         n += ttx_mark(s, dst + n, s->hlx, s->hly, false);
      } else {
         n += put_attr(s, dst + n, s->at[s->hly][s->hlx]);
         n += put_glyph(s, dst + n, s->ch[s->hly][s->hlx]);
      }
      s->hl_on = false;
   }
   if (s->cc_on && (s->ccx != s->cx || s->ccy != s->cy)
       && (s->at[s->ccy][s->ccx] & AT_KNOWN) != 0u) {
      n += put_pos(dst + n, s->ccx, s->ccy);
      if (s->tele) {
         n += ttx_mark(s, dst + n, s->ccx, s->ccy, true);
      } else {
         n += put_attr(s, dst + n, s->at[s->ccy][s->ccx]);
         n += put_str(dst + n, "\x1b[7m");
         n += put_glyph(s, dst + n, s->ch[s->ccy][s->ccx]);
      }
      s->hl_on = true;
      s->hlx = s->ccx;
      s->hly = s->ccy;
   }
   if (n != 0u) {
      n += put_finish(s, dst + n);
      s->tsgr = s->coloured ? 0xFFu : TTX_SGR_PLAIN;
   }
   return n;
}

bool tnd_stalled(bool pending, uint32_t now_us, uint32_t progress_us, uint32_t limit_us)
{
   return pending && (uint32_t)(now_us - progress_us) >= limit_us;
}

/* ---- the login's rules (see telnetd_proto.h) --------------------------------- */

void tnd_login_reset(tnd_login_t *l)
{
   memset(l, 0, sizeof *l);
}

static uint32_t count_down(uint32_t left, uint32_t dt)
{
   return left > dt ? left - dt : 0u;
}

void tnd_login_tick(tnd_login_t *l, uint32_t now_us)
{
   uint32_t dt = l->started ? now_us - l->last_us : 0u;   /* unsigned: right across the clock's wrap */
   l->started = true;
   l->last_us = now_us;
   if (dt == 0u)
      return;
   l->hold_left_us = count_down(l->hold_left_us, dt);
   for (unsigned i = 0u; i <= TND_LOGIN_ADDRS; i++) {
      tnd_login_addr_t *e = (i < TND_LOGIN_ADDRS) ? &l->a[i] : &l->over;
      if (!e->used)
         continue;
      e->lock_left_us = count_down(e->lock_left_us, dt);
      e->decay_left_us = count_down(e->decay_left_us, dt);
      if (e->lock_left_us == 0u && e->decay_left_us == 0u)
         e->used = false;                       /* forgotten: nothing against it any more */
   }
}

static tnd_login_addr_t *login_find(tnd_login_t *l, uint32_t addr, bool make)
{
   tnd_login_addr_t *spare = NULL;
   for (unsigned i = 0u; i < TND_LOGIN_ADDRS; i++) {
      tnd_login_addr_t *e = &l->a[i];
      if (e->used && e->addr == addr)
         return e;
      if (!e->used && spare == NULL)
         spare = e;
   }
   if (!make)
      return NULL;
   if (spare == NULL) {                         /* full: the unlocked one closest to being forgotten */
      for (unsigned i = 0u; i < TND_LOGIN_ADDRS; i++) {
         tnd_login_addr_t *e = &l->a[i];
         if (e->lock_left_us == 0u && (spare == NULL || e->decay_left_us < spare->decay_left_us))
            spare = e;
      }
      if (spare == NULL) {                      /* every one locked out: the shared count */
         l->over.used = true;
         return &l->over;
      }
   }
   memset(spare, 0, sizeof *spare);
   spare->used = true;
   spare->addr = addr;
   return spare;
}

bool tnd_login_locked(const tnd_login_t *l, uint32_t addr)
{
   bool all_locked = true;
   for (unsigned i = 0u; i < TND_LOGIN_ADDRS; i++) {
      if (l->a[i].used && l->a[i].addr == addr)
         return l->a[i].lock_left_us != 0u;
      if (!l->a[i].used || l->a[i].lock_left_us == 0u)
         all_locked = false;
   }
   return all_locked && l->over.lock_left_us != 0u;   /* not in the table: the shared count's lock */
}

bool tnd_login_held(const tnd_login_t *l)
{
   return l->hold_left_us != 0u;
}

bool tnd_login_fail(tnd_login_t *l, uint32_t addr)
{
   tnd_login_addr_t *e = login_find(l, addr, true);
   l->hold_left_us = TND_HOLD_US;
   e->decay_left_us = TND_FAIL_DECAY_US;
   if (e->fails < 255u)
      e->fails++;
   if (e->fails % 3u != 0u)
      return false;
   {
      uint32_t lock = TND_LOCK_US;
      for (unsigned i = 0u; i < e->lockouts && lock <= TND_LOCK_MAX_US / 2u; i++)
         lock *= 2u;                            /* longer each time: 1, 2, 4 ... 64 minutes */
      if (lock > TND_LOCK_MAX_US)
         lock = TND_LOCK_MAX_US;
      if (e->lockouts < 255u)
         e->lockouts++;
      e->lock_left_us = lock;
      if (e->decay_left_us < lock)
         e->decay_left_us = lock;               /* the count outlives the lock */
   }
   return true;
}

void tnd_login_ok(tnd_login_t *l, uint32_t addr)
{
   tnd_login_addr_t *e = login_find(l, addr, false);
   if (e != NULL)
      memset(e, 0, sizeof *e);
}

bool tnd_login_may_replace(uint32_t old_addr, uint32_t new_addr, uint32_t idle_us)
{
   return old_addr == new_addr || idle_us >= TND_REPLACE_IDLE_US;
}
