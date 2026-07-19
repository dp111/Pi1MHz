/* Bare-metal exhaustive test for memset.S, run under qemu-system-arm. */

typedef unsigned int   size_t_;
typedef unsigned char  u8;

void *memset(void *s, int c, size_t_ n);   /* the assembly under test */

static void sh_write0(const char *s)
{
    register int op asm("r0") = 0x04;
    register const char *p asm("r1") = s;
    asm volatile("svc 0x123456" :: "r"(op), "r"(p) : "memory");
}

static void sh_exit(int code)
{
    (void)code;
    for (;;) {}          /* qemu is killed by the timeout in run.sh */
}

static void put_uint(char **out, unsigned v)
{
    char tmp[12]; int i = 0;
    if (!v) { *(*out)++ = '0'; return; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) *(*out)++ = tmp[--i];
}

#define GUARD 64
#define MAXLEN 1200
static u8 buf[GUARD + MAXLEN + GUARD];

static unsigned failures = 0;
static unsigned checks   = 0;

static void report(unsigned len, unsigned off, unsigned fill, const char *what)
{
    char msg[160]; char *p = msg;
    const char *s = "FAIL len="; while (*s) *p++ = *s++;
    put_uint(&p, len);
    s = " off="; while (*s) *p++ = *s++;
    put_uint(&p, off);
    s = " fill="; while (*s) *p++ = *s++;
    put_uint(&p, fill);
    *p++ = ' ';
    while (*what) *p++ = *what++;
    *p++ = '\n'; *p = 0;
    sh_write0(msg);
    failures++;
}

static void one(unsigned len, unsigned off, unsigned fill)
{
    unsigned i;
    u8 *target = buf + GUARD + off;

    /* poison the whole buffer without going through memset */
    for (i = 0; i < sizeof buf; i++) buf[i] = 0xAA;

    void *ret = memset(target, (int)fill, len);
    checks++;

    if (ret != (void *)target) { report(len, off, fill, "bad return"); return; }

    for (i = 0; i < len; i++)
        if (target[i] != (u8)fill) { report(len, off, fill, "body wrong"); return; }

    /* nothing before the target may have changed */
    for (i = 0; i < GUARD + off; i++)
        if (buf[i] != 0xAA) { report(len, off, fill, "underrun"); return; }

    /* nothing after it either */
    for (i = GUARD + off + len; i < sizeof buf; i++)
        if (buf[i] != 0xAA) { report(len, off, fill, "overrun"); return; }
}

int main(void)
{
    static const unsigned fills[] = { 0x00, 0x5A, 0xFF, 0x101 /* tests c&0xff */ };
    unsigned len, off, f;
    sh_write0("START\n");

    /* dense sweep over the sizes where the head/tail/bulk paths interact */
    for (len = 0; len <= 200; len++)
        for (off = 0; off < 16; off++)
            for (f = 0; f < 4; f++)
                one(len, off, fills[f]);

    /* a few large ones to exercise the 32-byte bulk loop properly */
    static const unsigned big[] = { 255, 256, 257, 511, 512, 513, 1023, 1024, 1100 };
    for (len = 0; len < sizeof big / sizeof big[0]; len++)
        for (off = 0; off < 8; off++)
            for (f = 0; f < 4; f++)
                one(big[len], off, fills[f]);

    {
        char msg[96]; char *p = msg;
        const char *s = "checks="; while (*s) *p++ = *s++;
        put_uint(&p, checks);
        s = " failures="; while (*s) *p++ = *s++;
        put_uint(&p, failures);
        *p++ = '\n'; *p = 0;
        sh_write0(msg);
    }
    sh_write0(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    sh_exit((int)failures);
    return 0;
}
