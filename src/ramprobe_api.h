/* ramprobe_api.h - the RAM probe's HTTP routes, apart from the web server. */
#ifndef RAMPROBE_API_H
#define RAMPROBE_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAMPROBE_API_POKE_MAX 512u         /* a poke's bytes in one request */
#define RAMPROBE_API_OUT      (8192u + 64u)   /* room for 4 KB of a peek, in hex */

#define RAMPROBE_API_BODY_MAX 32768u       /* a poke's bytes in a request body (raw, not hex) */

/* Answers a request for path, with query (or NULL), into out: the HTTP status,
   or 0 if it isn't one of these routes (or the probe is off). */
int ramprobe_api(bool post, const char *path, const char *query, char *out, size_t n);

/* POST /bbc/poke with its bytes as the request body instead of hex= (up to
   RAMPROBE_API_BODY_MAX).  The same answer as ramprobe_api. */
int ramprobe_api_body(const char *path, const char *query, const uint8_t *body, size_t len, char *out, size_t n);

/* The server's part of wait=1, which a client puts on a POST to be answered when the work is
   over rather than at once (so it needn't poll GET /bbc): true if the server should hold this
   request's answer, and then ramprobe_api_done() says when to send ramprobe_api_status(). */
bool ramprobe_api_hold(bool post, const char *query, int status);
bool ramprobe_api_done(void);
int  ramprobe_api_status(char *out, size_t n);

#endif
