/* Small public-domain MD5 implementation (RFC 1321).  Compact, no
   tables - the per-round constants are folded into the STEP macro
   call sites.  Sufficient for digest-auth nonce/response computation;
   not intended for any security-sensitive use beyond that. */

#include "md5.h"

#include <string.h>

#define F(x, y, z)  (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z)  (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z)  ((x) ^ (y) ^ (z))
#define II(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROL(v, s) (((v) << (s)) | ((v) >> (32u - (s))))

#define STEP(f, a, b, c, d, x, t, s) do { \
   (a) += f((b), (c), (d)) + (x) + (uint32_t)(t); \
   (a) = ROL((a), (s)); \
   (a) += (b); \
} while (0)

static uint32_t md5_load_le32(const uint8_t *p)
{
   return ((uint32_t)p[0])
        | ((uint32_t)p[1] <<  8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static void md5_store_le32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v      );
   p[1] = (uint8_t)(v >>  8);
   p[2] = (uint8_t)(v >> 16);
   p[3] = (uint8_t)(v >> 24);
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
   uint32_t a = state[0];
   uint32_t b = state[1];
   uint32_t c = state[2];
   uint32_t d = state[3];
   uint32_t x[16];
   unsigned int i;

   for (i = 0u; i < 16u; ++i)
      x[i] = md5_load_le32(&block[i * 4u]);

   /* Round 1 */
   STEP(F, a, b, c, d, x[ 0], 0xd76aa478,  7);
   STEP(F, d, a, b, c, x[ 1], 0xe8c7b756, 12);
   STEP(F, c, d, a, b, x[ 2], 0x242070db, 17);
   STEP(F, b, c, d, a, x[ 3], 0xc1bdceee, 22);
   STEP(F, a, b, c, d, x[ 4], 0xf57c0faf,  7);
   STEP(F, d, a, b, c, x[ 5], 0x4787c62a, 12);
   STEP(F, c, d, a, b, x[ 6], 0xa8304613, 17);
   STEP(F, b, c, d, a, x[ 7], 0xfd469501, 22);
   STEP(F, a, b, c, d, x[ 8], 0x698098d8,  7);
   STEP(F, d, a, b, c, x[ 9], 0x8b44f7af, 12);
   STEP(F, c, d, a, b, x[10], 0xffff5bb1, 17);
   STEP(F, b, c, d, a, x[11], 0x895cd7be, 22);
   STEP(F, a, b, c, d, x[12], 0x6b901122,  7);
   STEP(F, d, a, b, c, x[13], 0xfd987193, 12);
   STEP(F, c, d, a, b, x[14], 0xa679438e, 17);
   STEP(F, b, c, d, a, x[15], 0x49b40821, 22);

   /* Round 2 */
   STEP(G, a, b, c, d, x[ 1], 0xf61e2562,  5);
   STEP(G, d, a, b, c, x[ 6], 0xc040b340,  9);
   STEP(G, c, d, a, b, x[11], 0x265e5a51, 14);
   STEP(G, b, c, d, a, x[ 0], 0xe9b6c7aa, 20);
   STEP(G, a, b, c, d, x[ 5], 0xd62f105d,  5);
   STEP(G, d, a, b, c, x[10], 0x02441453,  9);
   STEP(G, c, d, a, b, x[15], 0xd8a1e681, 14);
   STEP(G, b, c, d, a, x[ 4], 0xe7d3fbc8, 20);
   STEP(G, a, b, c, d, x[ 9], 0x21e1cde6,  5);
   STEP(G, d, a, b, c, x[14], 0xc33707d6,  9);
   STEP(G, c, d, a, b, x[ 3], 0xf4d50d87, 14);
   STEP(G, b, c, d, a, x[ 8], 0x455a14ed, 20);
   STEP(G, a, b, c, d, x[13], 0xa9e3e905,  5);
   STEP(G, d, a, b, c, x[ 2], 0xfcefa3f8,  9);
   STEP(G, c, d, a, b, x[ 7], 0x676f02d9, 14);
   STEP(G, b, c, d, a, x[12], 0x8d2a4c8a, 20);

   /* Round 3 */
   STEP(H, a, b, c, d, x[ 5], 0xfffa3942,  4);
   STEP(H, d, a, b, c, x[ 8], 0x8771f681, 11);
   STEP(H, c, d, a, b, x[11], 0x6d9d6122, 16);
   STEP(H, b, c, d, a, x[14], 0xfde5380c, 23);
   STEP(H, a, b, c, d, x[ 1], 0xa4beea44,  4);
   STEP(H, d, a, b, c, x[ 4], 0x4bdecfa9, 11);
   STEP(H, c, d, a, b, x[ 7], 0xf6bb4b60, 16);
   STEP(H, b, c, d, a, x[10], 0xbebfbc70, 23);
   STEP(H, a, b, c, d, x[13], 0x289b7ec6,  4);
   STEP(H, d, a, b, c, x[ 0], 0xeaa127fa, 11);
   STEP(H, c, d, a, b, x[ 3], 0xd4ef3085, 16);
   STEP(H, b, c, d, a, x[ 6], 0x04881d05, 23);
   STEP(H, a, b, c, d, x[ 9], 0xd9d4d039,  4);
   STEP(H, d, a, b, c, x[12], 0xe6db99e5, 11);
   STEP(H, c, d, a, b, x[15], 0x1fa27cf8, 16);
   STEP(H, b, c, d, a, x[ 2], 0xc4ac5665, 23);

   /* Round 4 */
   STEP(II, a, b, c, d, x[ 0], 0xf4292244,  6);
   STEP(II, d, a, b, c, x[ 7], 0x432aff97, 10);
   STEP(II, c, d, a, b, x[14], 0xab9423a7, 15);
   STEP(II, b, c, d, a, x[ 5], 0xfc93a039, 21);
   STEP(II, a, b, c, d, x[12], 0x655b59c3,  6);
   STEP(II, d, a, b, c, x[ 3], 0x8f0ccc92, 10);
   STEP(II, c, d, a, b, x[10], 0xffeff47d, 15);
   STEP(II, b, c, d, a, x[ 1], 0x85845dd1, 21);
   STEP(II, a, b, c, d, x[ 8], 0x6fa87e4f,  6);
   STEP(II, d, a, b, c, x[15], 0xfe2ce6e0, 10);
   STEP(II, c, d, a, b, x[ 6], 0xa3014314, 15);
   STEP(II, b, c, d, a, x[13], 0x4e0811a1, 21);
   STEP(II, a, b, c, d, x[ 4], 0xf7537e82,  6);
   STEP(II, d, a, b, c, x[11], 0xbd3af235, 10);
   STEP(II, c, d, a, b, x[ 2], 0x2ad7d2bb, 15);
   STEP(II, b, c, d, a, x[ 9], 0xeb86d391, 21);

   state[0] += a;
   state[1] += b;
   state[2] += c;
   state[3] += d;
}

void md5_init(md5_ctx_t *ctx)
{
   ctx->state[0] = 0x67452301u;
   ctx->state[1] = 0xefcdab89u;
   ctx->state[2] = 0x98badcfeu;
   ctx->state[3] = 0x10325476u;
   ctx->count    = 0u;
}

void md5_update(md5_ctx_t *ctx, const void *data, size_t len)
{
   const uint8_t *src = (const uint8_t *)data;
   size_t         have = (size_t)(ctx->count & 63u);
   size_t         need = 64u - have;

   ctx->count += (uint64_t)len;

   if (have != 0u && len >= need) {
      memcpy(&ctx->buffer[have], src, need);
      md5_transform(ctx->state, ctx->buffer);
      src += need;
      len -= need;
      have = 0u;
   }

   while (len >= 64u) {
      md5_transform(ctx->state, src);
      src += 64u;
      len -= 64u;
   }

   if (len > 0u)
      memcpy(&ctx->buffer[have], src, len);
}

void md5_final(md5_ctx_t *ctx, uint8_t out[MD5_DIGEST_LEN])
{
   uint8_t pad[64];
   uint8_t length_le[8];
   uint64_t bits = ctx->count * 8u;
   size_t   have = (size_t)(ctx->count & 63u);
   size_t   padlen = (have < 56u) ? (56u - have) : (120u - have);
   unsigned int i;

   /* RFC 1321 padding: a single 0x80, then zeros, then the original
      length in bits as a little-endian 64-bit field. */
   pad[0] = 0x80u;
   memset(&pad[1], 0, sizeof(pad) - 1u);
   md5_update(ctx, pad, padlen);

   for (i = 0u; i < 8u; ++i)
      length_le[i] = (uint8_t)(bits >> (i * 8u));
   md5_update(ctx, length_le, 8u);

   md5_store_le32(&out[ 0], ctx->state[0]);
   md5_store_le32(&out[ 4], ctx->state[1]);
   md5_store_le32(&out[ 8], ctx->state[2]);
   md5_store_le32(&out[12], ctx->state[3]);
}

void md5_hex_cat(char out[MD5_HEX_LEN],
                 const void *const parts[], const size_t lens[],
                 unsigned int nparts)
{
   static const char hex[] = "0123456789abcdef";
   md5_ctx_t ctx;
   uint8_t   digest[MD5_DIGEST_LEN];
   unsigned int i;

   md5_init(&ctx);
   for (i = 0u; i < nparts; ++i)
      md5_update(&ctx, parts[i], lens[i]);
   md5_final(&ctx, digest);

   for (i = 0u; i < MD5_DIGEST_LEN; ++i) {
      out[(i * 2u)     ] = hex[(digest[i] >> 4) & 0x0Fu];
      out[(i * 2u) + 1u] = hex[ digest[i]       & 0x0Fu];
   }
}
