#ifndef WIFI_MD5_H
#define WIFI_MD5_H

/* Small public-domain MD5 (RFC 1321).  Sufficient for digest-auth
   nonce/response computation; not intended for any security-sensitive
   use beyond that. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
   uint32_t state[4];     /* a, b, c, d            */
   uint64_t count;        /* total bytes processed */
   uint8_t  buffer[64];   /* partial block         */
} md5_ctx_t;

#define MD5_DIGEST_LEN 16
#define MD5_HEX_LEN    32          /* lowercase hex, no NUL */

/* Fixed-width holder for a lowercase MD5 hex digest (NOT NUL-terminated).
   Use this everywhere the digest is stored as bytes so the missing NUL
   is part of the type rather than a comment on the buffer: a stray
   strcmp() or strlen() reads as a buffer overread instead of "happens to
   work today". */
typedef char md5_hex_t[MD5_HEX_LEN];

void md5_init(md5_ctx_t *ctx);
void md5_update(md5_ctx_t *ctx, const void *data, size_t len);
void md5_final(md5_ctx_t *ctx, uint8_t out[MD5_DIGEST_LEN]);

/* Convenience: one-shot MD5 of n input buffers concatenated; writes the
   32-character lowercase hex into out (must be at least 32 bytes; NOT
   NUL-terminated). */
void md5_hex_cat(md5_hex_t out,
                 const void *const parts[], const size_t lens[],
                 unsigned int nparts);

#endif
