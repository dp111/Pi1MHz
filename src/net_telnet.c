/* net_telnet.c - TELNET IAC input filter.  See net_telnet.h. */

#include "net_telnet.h"

enum { S_DATA = 0, S_IAC, S_OPT, S_SB, S_SB_IAC, S_CR };

void telnet_reset(telnet_ctx_t *ctx)
{
   unsigned i;
   ctx->state = S_DATA;
   ctx->cmd   = 0u;
   for (i = 0; i < sizeof ctx->seen; i++) ctx->seen[i] = 0u;
}

static void emit(uint8_t *buf, size_t cap, size_t *len, uint8_t b)
{
   if (*len < cap) buf[(*len)++] = b;
}

/* Queue an IAC <cmd> <opt> reply, at most once per option (loop guard). */
static void reply(telnet_ctx_t *ctx, uint8_t *rep, size_t cap, size_t *len,
                  uint8_t cmd, uint8_t opt)
{
   uint8_t mask = (uint8_t)(1u << (opt & 7u));
   if (ctx->seen[opt >> 3] & mask) return;
   ctx->seen[opt >> 3] |= mask;
   emit(rep, cap, len, TN_IAC);
   emit(rep, cap, len, cmd);
   emit(rep, cap, len, opt);
}

void telnet_filter(telnet_ctx_t *ctx,
                   const uint8_t *in, size_t in_len,
                   uint8_t *out, size_t out_cap, size_t *out_len,
                   uint8_t *rep, size_t rep_cap, size_t *rep_len)
{
   size_t i;
   *out_len = 0;
   *rep_len = 0;
   for (i = 0; i < in_len; i++) {
      uint8_t b = in[i];
      switch (ctx->state) {
      case S_DATA:
         if      (b == TN_IAC) ctx->state = S_IAC;
         else if (b == 0x0Du) { emit(out, out_cap, out_len, 0x0Du); ctx->state = S_CR; }
         else                  emit(out, out_cap, out_len, b);
         break;

      case S_CR:                                    /* a CR was just emitted */
         if      (b == 0x00u) ctx->state = S_DATA;                 /* CR NUL -> CR */
         else if (b == TN_IAC) ctx->state = S_IAC;
         else if (b == 0x0Du)  emit(out, out_cap, out_len, 0x0Du); /* CR CR: stay */
         else { emit(out, out_cap, out_len, b); ctx->state = S_DATA; }
         break;

      case S_IAC:
         if      (b == TN_IAC) { emit(out, out_cap, out_len, 0xFFu); ctx->state = S_DATA; }
         else if (b == TN_WILL || b == TN_WONT || b == TN_DO || b == TN_DONT)
                               { ctx->cmd = b; ctx->state = S_OPT; }
         else if (b == TN_SB)  ctx->state = S_SB;
         else                  ctx->state = S_DATA;  /* SE / NOP / other: ignore */
         break;

      case S_OPT:                                   /* b is the option code */
         if (ctx->cmd == TN_WILL)
            reply(ctx, rep, rep_cap, rep_len,
                  (b == TN_OPT_ECHO || b == TN_OPT_SGA) ? TN_DO : TN_DONT, b);
         else if (ctx->cmd == TN_DO)
            reply(ctx, rep, rep_cap, rep_len, TN_WONT, b);
         /* WONT / DONT need no reply */
         ctx->state = S_DATA;
         break;

      case S_SB:                                    /* drop subnegotiation data */
         if (b == TN_IAC) ctx->state = S_SB_IAC;
         break;

      case S_SB_IAC:
         if      (b == TN_SE)  ctx->state = S_DATA;  /* end of subnegotiation */
         else                  ctx->state = S_SB;    /* IAC IAC or stray: keep skipping */
         break;

      default:
         ctx->state = S_DATA;
         break;
      }
   }
}

size_t telnet_escape(const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t out_cap, size_t *consumed)
{
   size_t i = 0, o = 0;
   while (i < in_len) {
      if (in[i] == TN_IAC) {
         if (o + 2u > out_cap) break;         /* need room for both 0xFF bytes */
         out[o++] = TN_IAC;
         out[o++] = TN_IAC;
      } else {
         if (o + 1u > out_cap) break;
         out[o++] = in[i];
      }
      i++;
   }
   *consumed = i;
   return o;
}
