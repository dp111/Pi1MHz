/* Host tests for the wolfSSH provider (secure_service_wolfssh.c).
 *
 * PI1MHZ_SSH is off by default and wolfSSH is not carried in the tree, so no
 * firmware build compiles this file.  Here it is built against stand-ins for
 * wolfSSH, wolfCrypt, FatFs and the lwIP glue (provider_stubs/, the net
 * suite's lwIP headers) and driven through its nts_secure_port.  The fakes
 * record what the provider asks of the outside world: which paths it opens,
 * which address it connects to, when it kicks the WiFi RX. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "secure_service_wolfssh.h"
#include "BeebSCSI/fatfs/ff.h"
#include "BeebSCSI/filesystem.h"
#include "rpi/hwrng.h"
#include "wifi/wifi_lwip.h"
#include "lwip/altcp.h"
#include "lwip/dns.h"
#include "wolfssh/error.h"
#include "wolfssh/ssh.h"
#include "wolfssh/wolfsftp.h"
#include "wolfssl/wolfcrypt/coding.h"
#include "wolfssl/wolfcrypt/sha256.h"

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
   printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
   else { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- FatFs: every path the provider names ------------------------------- */
#define PATHS 32
static char paths[PATHS][96];
static int  npaths, mkdirs;
static char written_path[96];
static bool hosts_present;          /* known_hosts exists, holding hosts_text */
static char hosts_text[512];

static void saw(const char *what, const char *path)
{
   if (npaths < PATHS) snprintf(paths[npaths++], sizeof paths[0], "%s %s", what, path);
}
/* Every path recorded so far is under /Pi1MHz/ssh, absolute. */
static bool all_absolute(void)
{
   for (int i = 0; i < npaths; i++) {
      const char *p = strchr(paths[i], ' ') + 1;
      if (strcmp(p, "/Pi1MHz") != 0 && strncmp(p, "/Pi1MHz/ssh", 11) != 0) {
         printf("    relative or stray path: %s\n", paths[i]);
         return false;
      }
   }
   return npaths != 0;
}

static bool is_hosts(const char *path) { return strstr(path, "known_hosts") != NULL; }

FRESULT f_open(FIL *fp, const char *path, uint8_t mode)
{
   (void)mode;
   saw("open", path);
   if (is_hosts(path) && hosts_present) {
      fp->fsize = (FSIZE_t)strlen(hosts_text);
      fp->data = (const uint8_t *)hosts_text;
      return FR_OK;
   }
   return FR_NO_FILE;
}
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
   UINT n = btr < fp->fsize ? btr : (UINT)fp->fsize;
   memcpy(buff, fp->data, n);
   *br = n;
   return FR_OK;
}
FRESULT f_stat(const char *path, FILINFO *fno)
{
   saw("stat", path);
   if (is_hosts(path) && hosts_present) { fno->fsize = (FSIZE_t)strlen(hosts_text); return FR_OK; }
   return FR_NO_FILE;
}
FRESULT f_mkdir(const char *path) { saw("mkdir", path); mkdirs++; return FR_EXIST; }
bool filesystemWriteFileSafe(const char *filename, const uint8_t *address, uint32_t length)
{
   saw("write", filename);
   snprintf(written_path, sizeof written_path, "%s", filename);
   if (length >= sizeof hosts_text) return false;
   memcpy(hosts_text, address, length);
   hosts_text[length] = '\0';
   hosts_present = true;
   return true;
}

/* ---- hardware RNG: a generator that works --------------------------------- */
static uint32_t rng_word = 0x12345678u;
void hwrng_start(void) {}
int hwrng_word(uint32_t *out, uint32_t wait_us)
{
   (void)wait_us;
   rng_word = rng_word * 1103515245u + 12345u;
   *out = rng_word;
   return 0;
}

/* ---- WiFi glue ------------------------------------------------------------- */
static int kicks;
static const wifi_lwip_context_t wifi_ctx = { .address_ready = true };
const wifi_lwip_context_t *wifi_lwip_get_context(void) { return &wifi_ctx; }
void wifi_lwip_rx_kick(void) { kicks++; }

/* ---- lwIP: DNS answers when the test says, one pcb ------------------------- */
#define MAX_DNS 4
static struct { char name[64]; dns_found_callback cb; void *arg; } dns_q[MAX_DNS];
static int ndns;
static bool dns_sync;               /* answer at once from the "cache" */

err_t dns_gethostbyname(const char *hostname, ip_addr_t *addr,
                        dns_found_callback found, void *callback_arg)
{
   if (dns_sync) { IP_ADDR4(addr, 10, 0, 0, 9); return ERR_OK; }
   if (ndns < MAX_DNS) {
      snprintf(dns_q[ndns].name, sizeof dns_q[0].name, "%s", hostname);
      dns_q[ndns].cb = found;
      dns_q[ndns].arg = callback_arg;
      ndns++;
   }
   return ERR_INPROGRESS;
}
static void dns_answer(int i, uint8_t last_octet)
{
   ip_addr_t a;
   IP_ADDR4(&a, 10, 0, 0, last_octet);
   dns_q[i].cb(dns_q[i].name, &a, dns_q[i].arg);
}

static struct altcp_pcb the_pcb;
static int connects;
static ip_addr_t connected_to;

struct altcp_pcb *altcp_new_ip_type(void *allocator, u8_t ip_type)
{
   (void)allocator; (void)ip_type;
   memset(&the_pcb, 0, sizeof the_pcb);
   the_pcb.t_sndbuf = 4096;
   return &the_pcb;
}
void altcp_arg(struct altcp_pcb *c, void *arg) { c->arg = arg; }
void altcp_recv(struct altcp_pcb *c, altcp_recv_fn f) { c->recv = f; }
void altcp_sent(struct altcp_pcb *c, altcp_sent_fn f) { c->sent = f; }
void altcp_poll(struct altcp_pcb *c, altcp_poll_fn f, u8_t i) { (void)i; c->poll = f; }
void altcp_err(struct altcp_pcb *c, altcp_err_fn f) { c->err = f; }
err_t altcp_connect(struct altcp_pcb *c, const ip_addr_t *ip, u16_t port, altcp_connected_fn f)
{
   (void)port;
   c->connected = f;
   connected_to = *ip;
   connects++;
   return ERR_OK;
}
u16_t altcp_sndbuf(struct altcp_pcb *c) { return c->t_sndbuf; }
err_t altcp_write(struct altcp_pcb *c, const void *d, u16_t len, u8_t f)
{ (void)c; (void)d; (void)len; (void)f; return ERR_OK; }
void altcp_output(struct altcp_pcb *c) { (void)c; }
void altcp_recved(struct altcp_pcb *c, u16_t len) { (void)c; (void)len; }
err_t altcp_close(struct altcp_pcb *c) { c->t_closed = 1; return ERR_OK; }
void altcp_abort(struct altcp_pcb *c) { c->t_closed = 1; }
u8_t pbuf_free(struct pbuf *p) { (void)p; return 1; }

/* ---- wolfSSH: a server that shows a host key and lets us in ---------------- */
struct WOLFSSH_CTX { WS_CallbackPublicKeyCheck check; };
struct WOLFSSH { WOLFSSH_CTX *ctx; void *check_ctx; };
static WOLFSSH_CTX the_ctx;
static WOLFSSH the_ssh;

int wolfSSH_Init(void) { return WS_SUCCESS; }
int wolfSSH_Cleanup(void) { return WS_SUCCESS; }
WOLFSSH_CTX *wolfSSH_CTX_new(byte side, void *heap) { (void)side; (void)heap; return &the_ctx; }
void wolfSSH_CTX_free(WOLFSSH_CTX *ctx) { (void)ctx; }
void wolfSSH_SetIORecv(WOLFSSH_CTX *ctx, WS_CallbackIORecv cb) { (void)ctx; (void)cb; }
void wolfSSH_SetIOSend(WOLFSSH_CTX *ctx, WS_CallbackIOSend cb) { (void)ctx; (void)cb; }
void wolfSSH_SetUserAuth(WOLFSSH_CTX *ctx, WS_CallbackUserAuth cb) { (void)ctx; (void)cb; }
void wolfSSH_CTX_SetPublicKeyCheck(WOLFSSH_CTX *ctx, WS_CallbackPublicKeyCheck cb) { ctx->check = cb; }
WOLFSSH *wolfSSH_new(WOLFSSH_CTX *ctx) { the_ssh.ctx = ctx; return &the_ssh; }
void wolfSSH_free(WOLFSSH *ssh) { (void)ssh; }
int wolfSSH_SetUsername(WOLFSSH *ssh, const char *u) { (void)ssh; (void)u; return WS_SUCCESS; }
int wolfSSH_SetChannelType(WOLFSSH *ssh, byte t, byte *d, word32 s)
{ (void)ssh; (void)t; (void)d; (void)s; return WS_SUCCESS; }
void wolfSSH_SetIOReadCtx(WOLFSSH *ssh, void *c) { (void)ssh; (void)c; }
void wolfSSH_SetIOWriteCtx(WOLFSSH *ssh, void *c) { (void)ssh; (void)c; }
void wolfSSH_SetUserAuthCtx(WOLFSSH *ssh, void *c) { (void)ssh; (void)c; }
void wolfSSH_SetPublicKeyCheckCtx(WOLFSSH *ssh, void *c) { ssh->check_ctx = c; }
int wolfSSH_connect(WOLFSSH *ssh)
{
   static const byte key[] = { 0, 0, 0, 11, 's','s','h','-','e','d','2','5','5','1','9', 1, 2, 3 };
   return ssh->ctx->check(key, sizeof key, ssh->check_ctx) == 0 ? WS_SUCCESS : WS_FATAL_ERROR;
}
int wolfSSH_shutdown(WOLFSSH *ssh) { (void)ssh; return WS_SUCCESS; }
int wolfSSH_get_error(const WOLFSSH *ssh) { (void)ssh; return WS_FATAL_ERROR; }
int wolfSSH_stream_read(WOLFSSH *ssh, byte *b, word32 n) { (void)ssh; (void)b; (void)n; return 0; }
int wolfSSH_stream_send(WOLFSSH *ssh, byte *b, word32 n) { (void)ssh; (void)b; return (int)n; }
int wolfSSH_ChangeTerminalSize(WOLFSSH *ssh, word32 c, word32 r, word32 w, word32 h)
{ (void)ssh; (void)c; (void)r; (void)w; (void)h; return WS_SUCCESS; }
int wolfSSH_ReadKey_buffer(const byte *in, word32 inSz, byte format, byte **out,
                           word32 *outSz, const byte **outType, word32 *outTypeSz, void *heap)
{ (void)in; (void)inSz; (void)format; (void)out; (void)outSz; (void)outType; (void)outTypeSz; (void)heap;
  return WS_FATAL_ERROR; }
int wolfSSH_SFTP_connect(WOLFSSH *ssh) { (void)ssh; return WS_FATAL_ERROR; }
WS_SFTPNAME *wolfSSH_SFTP_RealPath(WOLFSSH *ssh, char *d) { (void)ssh; (void)d; return NULL; }
WS_SFTPNAME *wolfSSH_SFTP_LS(WOLFSSH *ssh, char *d) { (void)ssh; (void)d; return NULL; }
void wolfSSH_SFTPNAME_list_free(WS_SFTPNAME *n) { (void)n; }
int wolfSSH_SFTP_Remove(WOLFSSH *ssh, char *f) { (void)ssh; (void)f; return WS_FATAL_ERROR; }
int wolfSSH_SFTP_MKDIR(WOLFSSH *ssh, char *d, WS_SFTP_FILEATRB *a) { (void)ssh; (void)d; (void)a; return WS_FATAL_ERROR; }
int wolfSSH_SFTP_RMDIR(WOLFSSH *ssh, char *d) { (void)ssh; (void)d; return WS_FATAL_ERROR; }
int wolfSSH_SFTP_Open(WOLFSSH *ssh, char *d, word32 r, WS_SFTP_FILEATRB *a, byte *h, word32 *hs)
{ (void)ssh; (void)d; (void)r; (void)a; (void)h; (void)hs; return WS_FATAL_ERROR; }
int wolfSSH_SFTP_SendReadPacket(WOLFSSH *ssh, byte *h, word32 hs, const word32 o[2], byte *out, word32 n)
{ (void)ssh; (void)h; (void)hs; (void)o; (void)out; (void)n; return WS_FATAL_ERROR; }
int wolfSSH_SFTP_SendWritePacket(WOLFSSH *ssh, byte *h, word32 hs, const word32 o[2], byte *in, word32 n)
{ (void)ssh; (void)h; (void)hs; (void)o; (void)in; (void)n; return WS_FATAL_ERROR; }
int wolfSSH_SFTP_Close(WOLFSSH *ssh, byte *h, word32 hs) { (void)ssh; (void)h; (void)hs; return WS_FATAL_ERROR; }

/* Not real base64 or SHA-256: the provider only needs stable text. */
int Base64_Encode_NoNl(const byte *in, word32 inLen, byte *out, word32 *outLen)
{
   if (*outLen < inLen * 2u) return -1;
   for (word32 i = 0; i < inLen; i++) {
      out[2 * i] = (byte)('a' + (in[i] & 15u));
      out[2 * i + 1] = (byte)('a' + (in[i] >> 4));
   }
   *outLen = inLen * 2u;
   return 0;
}
int wc_InitSha256(wc_Sha256 *s) { (void)s; return 0; }
int wc_Sha256Update(wc_Sha256 *s, const byte *d, word32 n) { (void)s; (void)d; (void)n; return 0; }
int wc_Sha256Final(wc_Sha256 *s, byte *h) { (void)s; memset(h, 7, WC_SHA256_DIGEST_SIZE); return 0; }
void wc_Sha256Free(wc_Sha256 *s) { (void)s; }

/* ---- helpers ---------------------------------------------------------------- */
static const nts_secure_port *port;
static void *ctx;
static char fingerprint[96];

static void bring_up(void)
{
   nts_pi_wolfssh_reset();
   for (int i = 0; i < 16 && !nts_pi_wolfssh_ready(); i++)
      nts_pi_wolfssh_poll();
   port = nts_pi_wolfssh_port();
   ctx = nts_pi_wolfssh_context();
}
static uint8_t open_url(const char *url, int trust)
{
   return port->ssh_open(ctx, url, "user", trust, fingerprint);
}
static void password(void)
{
   static const uint8_t pw[] = "secret";
   (void)port->ssh_password(ctx, pw, sizeof pw - 1u);
}

int main(void)
{
   printf("== bring-up ==\n");
   bring_up();
   CHECK(nts_pi_wolfssh_ready(), "the provider comes up with a working RNG");

   printf("== N4: no WiFi RX kick without a session ==\n");
   kicks = 0;
   for (int i = 0; i < 100; i++) nts_pi_wolfssh_poll();
   CHECK(kicks == 0, "100 idle polls kick the RX %d times", kicks);
   password();
   CHECK(open_url("TCP://a.test:22", 0) == NTS_PENDING, "an open starts resolving");
   kicks = 0;
   nts_pi_wolfssh_poll();
   CHECK(kicks == 1, "a session being set up keeps the RX kicked (%d)", kicks);
   port->ssh_close(ctx);
   kicks = 0;
   for (int i = 0; i < 10; i++) nts_pi_wolfssh_poll();
   CHECK(kicks == 0, "and once it closes, idle polls stop kicking (%d)", kicks);

   printf("== N2: a late DNS answer for a closed session ==\n");
   ndns = 0; connects = 0;
   password();
   CHECK(open_url("TCP://a.test:22", 0) == NTS_PENDING && ndns == 1, "open host A: resolving");
   port->ssh_close(ctx);
   password();
   CHECK(open_url("TCP://b.test:22", 0) == NTS_PENDING && ndns == 2, "closed, reopened as host B");
   dns_answer(0, 1);                            /* A's answer arrives now */
   CHECK(open_url("TCP://b.test:22", 0) == NTS_PENDING, "B is still pending");
   CHECK(connects == 0, "A's late answer does not connect B's session (%d connects to .%u)",
         connects, (unsigned)(connected_to.addr >> 24));
   dns_answer(1, 2);                            /* now B's */
   (void)open_url("TCP://b.test:22", 0);
   CHECK(connects == 1 && (connected_to.addr >> 24) == 2u,
         "B's own answer connects to B (%d connects, to .%u)",
         connects, (unsigned)(connected_to.addr >> 24));
   port->ssh_close(ctx);

   printf("== N3: SSH files are found by absolute path ==\n");
   npaths = 0; mkdirs = 0;
   bring_up();                                  /* a BBC reset */
   CHECK(mkdirs == 0, "a reset makes no directories (%d f_mkdir)", mkdirs);
   dns_sync = true;
   password();
   CHECK(open_url("TCP://c.test:22", 1) == NTS_PENDING, "open host C, trusting a new key");
   (void)open_url("TCP://c.test:22", 1);        /* resolved: connect */
   the_pcb.connected(the_pcb.arg, &the_pcb, ERR_OK);
   (void)open_url("TCP://c.test:22", 1);        /* handshake: host key checked */
   CHECK(strcmp(written_path, "/Pi1MHz/ssh/known_hosts") == 0,
         "the new host key goes to /Pi1MHz/ssh/known_hosts (wrote '%s')", written_path);
   CHECK(strncmp(hosts_text, "c.test ssh-ed25519 ", 19) == 0, "as host C's line: %s", hosts_text);
   CHECK(mkdirs != 0, "the directory is made when it is first written to (%d f_mkdir)", mkdirs);
   CHECK(all_absolute(), "every path the provider used is absolute (%d paths)", npaths);
   for (int i = 0; i < npaths; i++) printf("    %s\n", paths[i]);
   port->ssh_close(ctx);
   dns_sync = false;

   printf("%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
