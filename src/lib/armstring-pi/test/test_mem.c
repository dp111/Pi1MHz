/* Bare-metal test for memcpy / memmove / memcmp, run under qemu-system-arm.
 * Build with -mno-unaligned-access (see run.sh for why). */

typedef unsigned int  size_t_;
typedef unsigned char u8;

void *memcpy(void *d, const void *s, size_t_ n);
void *memmove(void *d, const void *s, size_t_ n);
int   memcmp(const void *a, const void *b, size_t_ n);

static void sh_write0(const char *s)
{
    register int op asm("r0") = 0x04;
    register const char *p asm("r1") = s;
    asm volatile("svc 0x123456" :: "r"(op), "r"(p) : "memory");
}

static void put_uint(char **out, unsigned v)
{
    char tmp[12]; int i = 0;
    if (!v) { *(*out)++ = '0'; return; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) *(*out)++ = tmp[--i];
}

static unsigned checks, failures;

static void fail(const char *what, unsigned a, unsigned b, unsigned c)
{
    char msg[160]; char *p = msg;
    while (*what) *p++ = *what++;
    *p++ = ' '; put_uint(&p, a);
    *p++ = ' '; put_uint(&p, b);
    *p++ = ' '; put_uint(&p, c);
    *p++ = '\n'; *p = 0;
    sh_write0(msg);
    failures++;
}

#define GUARD  64
#define MAXLEN 1200
#define SPAN   (GUARD + MAXLEN + GUARD)
static u8 dst[SPAN];
static u8 src[SPAN];
static u8 ovl[SPAN * 2];

static u8 pat(unsigned i) { return (u8)(i * 7u + 3u); }

/* ---- memcpy: non-overlapping, all alignment pairs ---- */
static void t_copy(unsigned len, unsigned soff, unsigned doff)
{
    unsigned i;
    u8 *s = src + GUARD + soff, *d = dst + GUARD + doff;

    for (i = 0; i < SPAN; i++) { src[i] = pat(i); dst[i] = 0xAA; }

    void *ret = memcpy(d, s, len);
    checks++;

    if (ret != (void *)d)            { fail("copy-ret", len, soff, doff); return; }
    for (i = 0; i < len; i++)
        if (d[i] != s[i])            { fail("copy-body", len, soff, doff); return; }
    for (i = 0; i < GUARD + doff; i++)
        if (dst[i] != 0xAA)          { fail("copy-under", len, soff, doff); return; }
    for (i = GUARD + doff + len; i < SPAN; i++)
        if (dst[i] != 0xAA)          { fail("copy-over", len, soff, doff); return; }
    for (i = 0; i < SPAN; i++)
        if (src[i] != pat(i))        { fail("copy-srcmod", len, soff, doff); return; }
}

/* ---- memmove: overlapping both directions ---- */
static void t_move(unsigned len, unsigned soff, int delta)
{
    unsigned i;
    unsigned sbase = GUARD + soff;
    unsigned dbase = (unsigned)((int)sbase + delta);
    u8 *s = ovl + sbase, *d = ovl + dbase;

    for (i = 0; i < sizeof ovl; i++) ovl[i] = pat(i);

    void *ret = memmove(d, s, len);
    checks++;

    if (ret != (void *)d) { fail("move-ret", len, soff, (unsigned)delta); return; }
    /* destination must equal what the source held *before* the move */
    for (i = 0; i < len; i++)
        if (d[i] != pat(sbase + i)) { fail("move-body", len, soff, (unsigned)delta); return; }
}

/* ---- memcmp: sign and zero ---- */
static void t_cmp(unsigned len, unsigned off, unsigned diffat)
{
    unsigned i;
    u8 *a = src + GUARD + off, *b = dst + GUARD + off;

    for (i = 0; i < SPAN; i++) { src[i] = pat(i); dst[i] = pat(i); }
    checks++;

    if (memcmp(a, b, len) != 0) { fail("cmp-equal", len, off, diffat); return; }
    if (diffat >= len) return;

    b[diffat] = (u8)(a[diffat] + 1);
    if (memcmp(a, b, len) >= 0) { fail("cmp-lt", len, off, diffat); return; }
    if (memcmp(b, a, len) <= 0) { fail("cmp-gt", len, off, diffat); return; }
}

int main(void)
{
    unsigned len, soff, doff, i;
    sh_write0("START\n");

    for (len = 0; len <= 128; len++)
        for (soff = 0; soff < 8; soff++)
            for (doff = 0; doff < 8; doff++)
                t_copy(len, soff, doff);

    static const unsigned big[] = { 255, 256, 257, 511, 512, 513, 1023, 1024, 1100 };
    for (i = 0; i < sizeof big / sizeof big[0]; i++)
        for (soff = 0; soff < 8; soff++)
            for (doff = 0; doff < 8; doff++)
                t_copy(big[i], soff, doff);

    /* overlap: forward and backward, by every offset that can straddle a word
       and a full 32-byte block */
    static const int deltas[] = { -33, -32, -17, -9, -4, -1, 1, 4, 9, 17, 32, 33 };
    for (len = 0; len <= 96; len++)
        for (soff = 0; soff < 8; soff++)
            for (i = 0; i < sizeof deltas / sizeof deltas[0]; i++)
                t_move(len, soff + 64, deltas[i]);
    for (i = 0; i < sizeof big / sizeof big[0]; i++)
        for (soff = 0; soff < 4; soff++) {
            unsigned k;
            for (k = 0; k < sizeof deltas / sizeof deltas[0]; k++)
                t_move(big[i], soff + 64, deltas[k]);
        }

    for (len = 0; len <= 128; len++)
        for (soff = 0; soff < 8; soff++)
            t_cmp(len, soff, len ? (len - 1) : 0);
    for (len = 1; len <= 128; len++)
        for (soff = 0; soff < 8; soff++)
            t_cmp(len, soff, 0);

    {
        char msg[96]; char *p = msg;
        const char *s = "checks="; while (*s) *p++ = *s++;
        put_uint(&p, checks);
        s = " failures="; while (*s) *p++ = *s++;
        put_uint(&p, failures);
        *p++ = '\n'; *p = 0;
        sh_write0(msg);
    }
    for (;;) {}
}
