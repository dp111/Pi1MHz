/* net_telnet.h - TELNET (RFC 854) input filter for the N: TELNET: scheme.
 *
 * A pure, stateful byte filter: it strips TELNET IAC command sequences from a
 * server->client TCP stream so the Beeb sees clean text, and it emits the
 * minimal option-negotiation replies (client->server) needed to settle a
 * line-mode session.  No I/O - the net service feeds RX bytes in and sends the
 * reply bytes out - so it is portable and host-testable with byte vectors.
 *
 * Policy (deliberately minimal, "just give me clean text"):
 *   - IAC IAC            -> one literal 0xFF in the output
 *   - IAC WILL ECHO/SGA  -> reply IAC DO  <opt>   (accept server echo/SGA)
 *   - IAC WILL <other>   -> reply IAC DONT <opt>  (refuse)
 *   - IAC DO   <opt>     -> reply IAC WONT <opt>  (we enable nothing)
 *   - IAC WONT/DONT <opt>-> no reply (server is declining/disabling)
 *   - IAC SB ... IAC SE  -> subnegotiation, dropped entirely
 *   - CR NUL             -> CR   (NUL dropped)
 * A respond-once bitmap suppresses a second reply about the same option, so a
 * misbehaving peer cannot drive a negotiation loop.
 */
#ifndef NET_TELNET_H
#define NET_TELNET_H

#include <stddef.h>
#include <stdint.h>

#define TELNET_PORT   23u

/* commands */
#define TN_IAC        255u
#define TN_DONT       254u
#define TN_DO         253u
#define TN_WONT       252u
#define TN_WILL       251u
#define TN_SB         250u
#define TN_SE         240u
/* options we accept */
#define TN_OPT_ECHO   1u
#define TN_OPT_SGA    3u

/* Parser state (net_telnet.c internal values; exposed only so the context can
   live inside a net handle). */
typedef struct {
   uint8_t state;        /* current parse state           */
   uint8_t cmd;          /* WILL/WONT/DO/DONT being parsed */
   uint8_t seen[32];     /* respond-once bitmap over 256 options */
} telnet_ctx_t;

void telnet_reset(telnet_ctx_t *ctx);

/* Filter `in` (in_len bytes): append clean data to out[<=out_cap] and any
   negotiation replies to rep[<=rep_cap], reporting the counts.  Partial IAC/CR
   sequences carry across calls via ctx.  out never needs more than in_len
   bytes (filtering only removes). */
void telnet_filter(telnet_ctx_t *ctx,
                   const uint8_t *in,  size_t in_len,
                   uint8_t *out, size_t out_cap, size_t *out_len,
                   uint8_t *rep, size_t rep_cap, size_t *rep_len);

/* Escape outbound data: each literal 0xFF becomes IAC IAC (0xFF 0xFF) so it is
   not taken as a command.  Writes the escaped bytes to out (up to out_cap),
   returns the escaped length, and reports how many input bytes were consumed
   (fewer than in_len if out filled - the caller sends the rest next time). */
size_t telnet_escape(const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t out_cap, size_t *consumed);

#endif /* NET_TELNET_H */
