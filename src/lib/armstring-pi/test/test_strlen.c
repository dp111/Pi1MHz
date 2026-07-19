/* Bare-metal exhaustive test for strlen-arm1176.S, run under qemu-system-arm.
 * Uses the MMU-enabled start.S harness (see run.sh / start.S). */

typedef unsigned int  size_t_;
typedef unsigned char u8;

size_t_ strlen(const char *s);   /* the assembly under test */

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

#define GUARD  64
#define MAXLEN 600
static char buf[GUARD + MAXLEN + GUARD];

static unsigned checks, failures;

static void fail(unsigned len, unsigned off, const char *what)
{
    char msg[128]; char *p = msg;
    const char *s = "FAIL len="; while (*s) *p++ = *s++;
    put_uint(&p, len);
    s = " off="; while (*s) *p++ = *s++;
    put_uint(&p, off);
    *p++ = ' ';
    while (*what) *p++ = *what++;
    *p++ = '\n'; *p = 0;
    sh_write0(msg);
    failures++;
}

/* Fill the whole span with non-zero bytes (never 0x00) so only the byte we
   place on purpose terminates the string. */
static u8 pat(unsigned i)
{
    u8 b = (u8)(i * 31u + 7u);
    return b ? b : 1;
}

static void one(unsigned len, unsigned off)
{
    unsigned i;
    char *s = buf + GUARD + off;

    for (i = 0; i < sizeof buf; i++) buf[i] = (char)pat(i);
    s[len] = '\0';

    size_t_ got = strlen(s);
    checks++;
    if (got != len) fail(len, off, "wrong length");
}

int main(void)
{
    unsigned len, off;
    sh_write0("START\n");

    for (len = 0; len <= 300; len++)
        for (off = 0; off < 16; off++)
            one(len, off);

    static const unsigned big[] = { 511, 512, 513 };
    for (unsigned i = 0; i < sizeof big / sizeof big[0]; i++)
        for (off = 0; off < 8; off++)
            one(big[i], off);

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
