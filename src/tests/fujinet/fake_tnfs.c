/* fake_tnfs.c - an in-memory TNFS server, and the fn_tnfs_io_* platform the
   FujiNet device talks to it through, for the host tests.

   Requests are queued by fn_tnfs_io_send and answered by fake_tnfs_step(),
   which hands each reply to fn_tnfs_input - so a request is never answered
   inside the call that sent it, as on a real network.  It can drop
   requests and answer EAGAIN on demand.  Hosts: "tnfs.test" resolves at
   once, "slow.test" after one "not yet", anything else fails. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fn_tnfs.h"
#include "fake_tnfs.h"

#define MAXF     16
#define MAXFD    16      /* more than the client's 8 handles, as a real server has */
#define QMAX     8

typedef struct { bool used; char path[256]; bool dir; uint8_t *data; uint32_t size; } ffile_t;
typedef struct { bool used; int file; uint32_t pos; bool is_dir; int next; } ffd_t;

static ffile_t F[MAXF];
static ffd_t   FD[MAXFD];
static uint32_t s_now;
static struct { uint8_t pkt[600]; uint16_t len; uint32_t ip; uint16_t port; } Q[QMAX];
static int s_q;
static int s_drop, s_eagain;
static bool s_slow_seen;
static fake_tnfs_stats_t S;

uint32_t fn_tnfs_io_now_ms(void) { return s_now; }
void fake_tnfs_advance(uint32_t ms) { s_now += ms; }
fake_tnfs_stats_t *fake_tnfs_stats(void) { return &S; }
void fake_tnfs_drop(int n) { s_drop = n; }
void fake_tnfs_eagain(int n) { s_eagain = n; }

fn_io fn_tnfs_io_resolve(const char *host, uint32_t *ip)
{
   if (strcmp(host, "tnfs.test") == 0) { *ip = 0x0A000001u; return FN_IO_OK; }
   if (strcmp(host, "slow.test") == 0) {
      if (!s_slow_seen) { s_slow_seen = true; return FN_IO_PENDING; }
      *ip = 0x0A000002u;
      return FN_IO_OK;
   }
   return FN_IO_FAIL;
}

bool fn_tnfs_io_send(uint32_t ip, uint16_t port, const uint8_t *pkt, uint16_t len)
{
   S.sent++;
   if (s_q == QMAX || len > sizeof Q[0].pkt) return false;
   memcpy(Q[s_q].pkt, pkt, len);
   Q[s_q].len = len;
   Q[s_q].ip = ip;
   Q[s_q].port = port;
   s_q++;
   return true;
}

static int find(const char *path)
{
   for (int i = 0; i < MAXF; i++) if (F[i].used && strcmp(F[i].path, path) == 0) return i;
   return -1;
}

static int add(const char *path, bool dir)
{
   for (int i = 0; i < MAXF; i++) if (!F[i].used) {
      memset(&F[i], 0, sizeof F[i]);
      F[i].used = true;
      F[i].dir = dir;
      snprintf(F[i].path, sizeof F[i].path, "%s", path);
      return i;
   }
   return -1;
}

void fake_tnfs_mkdir(const char *path) { if (find(path) < 0) add(path, true); }

void fake_tnfs_put(const char *path, const void *data, uint32_t size)
{
   int i = find(path);
   if (i < 0) i = add(path, false);
   free(F[i].data);
   F[i].data = malloc(size ? size : 1);
   memcpy(F[i].data, data, size);
   F[i].size = size;
}

const uint8_t *fake_tnfs_get(const char *path, uint32_t *size)
{
   int i = find(path);
   if (i < 0 || F[i].dir) return NULL;
   *size = F[i].size;
   return F[i].data;
}

int fake_tnfs_open_fds(void)
{
   int n = 0;
   for (int i = 0; i < MAXFD; i++) n += FD[i].used;
   return n;
}

void fake_tnfs_reset(void)
{
   for (int i = 0; i < MAXF; i++) free(F[i].data);
   memset(F, 0, sizeof F);
   memset(FD, 0, sizeof FD);
   memset(&S, 0, sizeof S);
   s_q = 0; s_drop = 0; s_eagain = 0; s_slow_seen = false;
   fake_tnfs_mkdir("/");
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

static void grow(ffile_t *f, uint32_t size)
{
   if (size <= f->size) return;
   f->data = realloc(f->data, size);
   memset(f->data + f->size, 0, size - f->size);   /* POSIX: the gap is zeros */
   f->size = size;
}

/* Answer one request into r (header copied, status at [4]); returns length. */
static uint16_t serve(const uint8_t *q, uint16_t n, uint8_t *r)
{
   uint8_t cmd = q[3];
   memcpy(r, q, 4);
   if (cmd == 0x00) { r[0] = 0x34; r[1] = 0x12; }             /* MOUNT assigns 0x1234 */
   else if (rd16(q) != 0x1234) { r[4] = 0x09; return 5; }    /* EBADF-ish: bad session */
   r[4] = 0;
   const uint8_t *b = q + 4;
   switch (cmd) {
   case 0x00: r[5] = 2; r[6] = 1; r[7] = 0xC8; r[8] = 0; return 9;  /* v1.2, retry 200 ms */
   case 0x01: return 5;
   case 0x29: {                                              /* OPEN */
      uint16_t fl = rd16(b);
      const char *path = (const char *)b + 4;
      int i = find(path);
      if (i >= 0 && F[i].dir) { r[4] = 0x15; return 5; }     /* EISDIR */
      if (i >= 0 && (fl & 0x0400) && (fl & 0x0100)) { r[4] = 0x11; return 5; }  /* EEXIST */
      if (i < 0) {
         if (!(fl & 0x0100)) { r[4] = 0x02; return 5; }       /* ENOENT */
         i = add(path, false);
      }
      if (fl & 0x0200) F[i].size = 0;
      for (int d = 0; d < MAXFD; d++) if (!FD[d].used) {
         FD[d] = (ffd_t){ .used = true, .file = i };
         S.opens++;
         r[5] = (uint8_t)d;
         return 6;
      }
      r[4] = 0x18; return 5;                                  /* EMFILE */
   }
   case 0x23:                                                 /* CLOSE */
      if (b[0] >= MAXFD || !FD[b[0]].used) { r[4] = 0x09; return 5; }
      FD[b[0]].used = false; S.closes++; return 5;
   case 0x25: {                                               /* LSEEK */
      if (b[0] >= MAXFD || !FD[b[0]].used) { r[4] = 0x09; return 5; }
      FD[b[0]].pos = rd32(b + 2);                             /* SEEK_SET only */
      S.seeks++;
      return 5;
   }
   case 0x21: {                                               /* READ */
      if (b[0] >= MAXFD || !FD[b[0]].used) { r[4] = 0x09; return 5; }
      ffile_t *f = &F[FD[b[0]].file];
      uint32_t pos = FD[b[0]].pos, want = rd16(b + 1);
      if (pos >= f->size) { r[4] = 0x21; return 5; }         /* EOF */
      uint32_t got = f->size - pos < want ? f->size - pos : want;
      r[5] = (uint8_t)got; r[6] = (uint8_t)(got >> 8);
      memcpy(r + 7, f->data + pos, got);
      FD[b[0]].pos += got;
      S.reads++;
      return (uint16_t)(7 + got);
   }
   case 0x22: {                                               /* WRITE */
      if (b[0] >= MAXFD || !FD[b[0]].used) { r[4] = 0x09; return 5; }
      ffile_t *f = &F[FD[b[0]].file];
      uint32_t len = rd16(b + 1), pos = FD[b[0]].pos;
      grow(f, pos + len);
      memcpy(f->data + pos, b + 3, len);
      FD[b[0]].pos += len;
      r[5] = (uint8_t)len; r[6] = (uint8_t)(len >> 8);
      S.writes++;
      return 7;
   }
   case 0x24: {                                               /* STAT */
      int i = find((const char *)b);
      if (i < 0) { r[4] = 0x02; return 5; }
      memset(r + 5, 0, 22);
      uint16_t mode = F[i].dir ? 0x41ED : 0x81A4;
      r[5] = (uint8_t)mode; r[6] = (uint8_t)(mode >> 8);
      uint32_t sz = F[i].dir ? 0 : F[i].size;
      for (int k = 0; k < 4; k++) r[11 + k] = (uint8_t)(sz >> (8 * k));
      uint32_t mt = 1700000000u;                              /* 2023-11-14 */
      for (int k = 0; k < 4; k++) r[19 + k] = (uint8_t)(mt >> (8 * k));
      return 27;
   }
   case 0x10: {                                               /* OPENDIR */
      int i = find((const char *)b);
      if (i < 0 || !F[i].dir) { r[4] = 0x02; return 5; }
      for (int d = 0; d < MAXFD; d++) if (!FD[d].used) {
         FD[d] = (ffd_t){ .used = true, .file = i, .is_dir = true, .next = -2 };
         r[5] = (uint8_t)d;
         return 6;
      }
      r[4] = 0x18; return 5;
   }
   case 0x11: {                                               /* READDIR: ".", "..", children */
      if (b[0] >= MAXFD || !FD[b[0]].used || !FD[b[0]].is_dir) { r[4] = 0x09; return 5; }
      ffd_t *d = &FD[b[0]];
      if (d->next == -2) { d->next = -1; strcpy((char *)r + 5, "."); return 7; }
      if (d->next == -1) { d->next = 0; strcpy((char *)r + 5, ".."); return 8; }
      const char *dir = F[d->file].path;
      size_t dl = strlen(dir);
      for (int i = d->next; i < MAXF; i++) {
         if (!F[i].used || i == d->file) continue;
         const char *p = F[i].path;
         if (strncmp(p, dir, dl) != 0) continue;
         const char *rest = p + dl;
         if (dl > 1) { if (*rest != '/') continue; rest++; }
         else if (*rest == '/') rest++;
         if (!*rest || strchr(rest, '/')) continue;
         d->next = i + 1;
         strcpy((char *)r + 5, rest);
         return (uint16_t)(5 + strlen(rest) + 1);
      }
      r[4] = 0x21; return 5;                                  /* EOF */
   }
   case 0x12:                                                 /* CLOSEDIR */
      if (b[0] < MAXFD) FD[b[0]].used = false;
      return 5;
   default:
      r[4] = 0x26; return 5;                                  /* ENOSYS */
   }
   (void)n;
}

void fake_tnfs_step(void)
{
   int n = s_q;
   s_q = 0;
   for (int i = 0; i < n; i++) {
      if (s_drop > 0) { s_drop--; S.dropped++; continue; }
      uint8_t reply[600];
      uint16_t rl;
      if (s_eagain > 0 && Q[i].pkt[3] != 0x00) {             /* busy: back off 50 ms */
         s_eagain--;
         memcpy(reply, Q[i].pkt, 4);
         reply[4] = 0x07; reply[5] = 50; reply[6] = 0;
         rl = 7;
      } else {
         rl = serve(Q[i].pkt, Q[i].len, reply);
      }
      fn_tnfs_input(Q[i].ip, Q[i].port, reply, rl);
   }
}
