/* smb_crypto.c - what libsmb2 needs from the platform besides sockets.

   - AES128_ECB_encrypt: in place of libsmb2's lib/aes.c.  Its reference AES
     expands the key for every 16-byte block; SMB3 signing (AES-CMAC) and
     sealing (AES-CCM) call it once per block with the same key, so here the
     schedule is kept and mbedTLS (already in the firmware) does the block.
     Measured with a qemu instruction count, CMAC over 64 KB: 252 -> 52
     instructions per byte on the ARM1176 (docs/dev/smb-network-storage-plan.md).
   - MD4 (the NT password hash): in place of lib/md4c.c, whose RSA licence
     is not GPL-compatible.  Written from the algorithm in RFC 1320.
   - Random bytes from the BCM2835 hardware RNG, and the small process
     identity calls (time, pid, host and login name). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "../rpi/hwrng.h"
#include "../rpi/systimer.h"

#include "port/smb_port.h"
#include "libsmb2/lib/md4.h"

/* ---- AES-128, one block ------------------------------------------------- */

void AES128_ECB_encrypt(uint8_t *input, const uint8_t *key, uint8_t *output);

void AES128_ECB_encrypt(uint8_t *input, const uint8_t *key, uint8_t *output)
{
   static mbedtls_aes_context ctx;
   static uint8_t ctx_key[16];
   static bool    ctx_set;
   if (!ctx_set || memcmp(ctx_key, key, sizeof ctx_key) != 0) {
      if (!ctx_set)
         mbedtls_aes_init(&ctx);
      mbedtls_aes_setkey_enc(&ctx, key, 128);
      memcpy(ctx_key, key, sizeof ctx_key);
      ctx_set = true;
   }
   uint8_t in[16];
   memcpy(in, input, sizeof in);           /* callers pass the same buffer both ways */
   mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, output);
}

/* ---- MD4 (RFC 1320) ----------------------------------------------------- */

#define MD4_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD4_G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define MD4_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD4_ROTL(v, s) (((v) << (s)) | ((v) >> (32 - (s))))

static void md4_block(uint32_t st[4], const unsigned char blk[64])
{
   uint32_t x[16];
   for (int i = 0; i < 16; i++)
      x[i] = (uint32_t)blk[4 * i] | ((uint32_t)blk[4 * i + 1] << 8) |
             ((uint32_t)blk[4 * i + 2] << 16) | ((uint32_t)blk[4 * i + 3] << 24);
   uint32_t a = st[0], b = st[1], c = st[2], d = st[3];

   static const uint8_t s1[4] = { 3, 7, 11, 19 };
   for (int i = 0; i < 16; i++) {
      uint32_t t = a + MD4_F(b, c, d) + x[i];
      t = MD4_ROTL(t, s1[i & 3]);
      a = d; d = c; c = b; b = t;
   }
   static const uint8_t s2[4] = { 3, 5, 9, 13 };
   static const uint8_t k2[16] = { 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 };
   for (int i = 0; i < 16; i++) {
      uint32_t t = a + MD4_G(b, c, d) + x[k2[i]] + 0x5A827999u;
      t = MD4_ROTL(t, s2[i & 3]);
      a = d; d = c; c = b; b = t;
   }
   static const uint8_t s3[4] = { 3, 9, 11, 15 };
   static const uint8_t k3[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
   for (int i = 0; i < 16; i++) {
      uint32_t t = a + MD4_H(b, c, d) + x[k3[i]] + 0x6ED9EBA1u;
      t = MD4_ROTL(t, s3[i & 3]);
      a = d; d = c; c = b; b = t;
   }
   st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void MD4Init(MD4_CTX *ctx)
{
   ctx->state[0] = 0x67452301u;
   ctx->state[1] = 0xEFCDAB89u;
   ctx->state[2] = 0x98BADCFEu;
   ctx->state[3] = 0x10325476u;
   ctx->count[0] = ctx->count[1] = 0;
}

void MD4Update(MD4_CTX *ctx, unsigned char *in, unsigned int len)
{
   unsigned int have = (ctx->count[0] >> 3) & 63u;
   uint32_t bits = (uint32_t)len << 3;
   ctx->count[0] += bits;
   if (ctx->count[0] < bits)
      ctx->count[1]++;
   ctx->count[1] += (uint32_t)len >> 29;
   while (len > 0u) {
      unsigned int n = 64u - have;
      if (n > len)
         n = len;
      memcpy(ctx->buffer + have, in, n);
      have += n;
      in += n;
      len -= n;
      if (have == 64u) {
         md4_block(ctx->state, ctx->buffer);
         have = 0;
      }
   }
}

void MD4Final(unsigned char digest[16], MD4_CTX *ctx)
{
   unsigned char lenbytes[8];
   for (int i = 0; i < 4; i++) {
      lenbytes[i]     = (unsigned char)(ctx->count[0] >> (8 * i));
      lenbytes[i + 4] = (unsigned char)(ctx->count[1] >> (8 * i));
   }
   unsigned int have = (ctx->count[0] >> 3) & 63u;
   unsigned char pad[64] = { 0x80 };
   MD4Update(ctx, pad, (have < 56u) ? (56u - have) : (120u - have));
   MD4Update(ctx, lenbytes, 8);
   for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
         digest[4 * i + j] = (unsigned char)(ctx->state[i] >> (8 * j));
   memset(ctx, 0, sizeof *ctx);
}

/* ---- the platform ------------------------------------------------------- */

/* NTLM's client challenge, the session salt and SMB3 sealing nonces.  The
   hardware RNG is started at boot by mbedTLS's entropy source; a stalled
   generator falls back to the free-running timer mixed in, which is weak -
   but a logon then still works rather than hanging the Beeb. */
void smbp_random_bytes(void *buf, size_t len)
{
   uint8_t *p = buf;
   hwrng_start();
   while (len > 0u) {
      uint32_t w;
      if (hwrng_word(&w, 10000u) != 0)
         w = RPI_GetSystemTime() * 2654435761u ^ (uint32_t)(uintptr_t)p;
      size_t n = len < 4u ? len : 4u;
      memcpy(p, &w, n);
      p += n;
      len -= n;
   }
}

/* Seconds since the Pi started.  libsmb2 uses time() for its timeouts
   (differences only) and as the timestamp in the NTLMv2 blob, so a logon
   says "1970".  Samba does not check it; whether Windows does is to be
   tested (GUESSED: it does not - only Kerberos checks clock skew). */
time_t smbp_time(time_t *t)
{
   time_t now = (time_t)(RPI_GetSystemTime64() / 1000000u);
   if (t != NULL)
      *t = now;
   return now;
}

pid_t smbp_getpid(void)
{
   return 1;
}

int smbp_gethostname(char *name, size_t len)
{
   if (len == 0u)
      return -1;
   strncpy(name, "Pi1MHz", len - 1u);
   name[len - 1u] = '\0';
   return 0;
}

/* libsmb2 can read passwords from a file named by $NTLM_USER_FILE; there is
   no such file here (the password is in Pi1MHz.cfg), and newlib's open()
   would only be a stub. */
FILE *smbp_fopen(const char *path, const char *mode)
{
   (void)path;
   (void)mode;
   return NULL;
}

/* No login: the user always comes from Pi1MHz.cfg (smb_client.c). */
int smbp_getlogin_r(char *buf, size_t size)
{
   (void)buf;
   (void)size;
   return -1;
}
