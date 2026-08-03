/* Interop test: validates net_tnfs.c against a REAL FujiNet tnfsd.
 * Not part of run_tests.sh (needs a running server).  To run:
 *   git clone https://github.com/FujiNetWIFI/tnfsd; cd tnfsd/src
 *   mv atari-boot-xex-file.asm{,.bak}; make OS=LINUX      # skip the xa dep
 *   mkdir /tmp/tr; echo 'Hello from real tnfsd!' > /tmp/tr/HELLO.TXT
 *   ./bin/tnfsd /tmp/tr &
 *   gcc -I../../src -o tl $THIS ../../src/net_tnfs.c && ./tl
 * Result 2026-08-03: 12 checks, 0 failures (tnfsd 26.0606.1-dev).
 */
#include "net_tnfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static int sock;
static struct sockaddr_in srv;
static int checks, fails;
#define CHECK(c,m) do{checks++; if(c)printf("  ok: %s\n",m); else{printf("  FAIL: %s\n",m);fails++;}}while(0)

/* send a request and read the reply into rep (via our parser) */
static uint8_t g_buf[1600];
static int xact(const uint8_t *req, size_t rlen, uint8_t seq, uint8_t cmd, tnfs_reply_t *rep)
{
   ssize_t n;
   sendto(sock, req, rlen, 0, (struct sockaddr*)&srv, sizeof srv);
   n = recv(sock, g_buf, sizeof g_buf, 0);
   if (n < 0) return 0;
   return tnfs_parse_reply(g_buf, (size_t)n, seq, cmd, rep);
}

int main(void)
{
   uint8_t req[512];
   tnfs_reply_t rep;
   uint16_t connid = 0;
   struct timeval tv = { 2, 0 };
   size_t n;

   sock = socket(AF_INET, SOCK_DGRAM, 0);
   setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
   memset(&srv, 0, sizeof srv);
   srv.sin_family = AF_INET;
   srv.sin_port = htons(16384);
   srv.sin_addr.s_addr = htonl(0x7F000001);   /* 127.0.0.1 */

   printf("== net_tnfs codec vs real tnfsd ==\n");

   /* MOUNT / */
   n = tnfs_build_mount(req, sizeof req, 1, "/", NULL, NULL);
   CHECK(xact(req, n, 1, TNFS_CMD_MOUNT, &rep) && rep.status == TNFS_OK, "MOUNT / -> OK");
   { uint16_t ver=0, retry=0; CHECK(tnfs_reply_mount(&rep,&ver,&retry), "MOUNT reply parsed (ver/retry)"); }
   connid = rep.connid;
   printf("     session connid=%u\n", connid);

   /* OPENDIR / then READDIR the entries */
   n = tnfs_build_opendir(req, sizeof req, connid, 2, "/");
   uint8_t dh = 0xFF;
   CHECK(xact(req, n, 2, TNFS_CMD_OPENDIR, &rep) && rep.status==TNFS_OK && tnfs_reply_opendir(&rep,&dh),
         "OPENDIR / -> dir handle");
   {
      int got_hello = 0, seq = 3, count = 0;
      while (count < 32) {
         const char *name = NULL;
         n = tnfs_build_readdir(req, sizeof req, connid, (uint8_t)seq, dh);
         if (!xact(req, n, (uint8_t)seq, TNFS_CMD_READDIR, &rep)) break;
         if (rep.status == TNFS_EOF) break;
         if (rep.status != TNFS_OK || !tnfs_reply_readdir(&rep, &name)) break;
         printf("     dir entry: %s\n", name);
         if (strcmp(name, "HELLO.TXT") == 0) got_hello = 1;
         seq++; count++;
      }
      CHECK(got_hello, "READDIR listed HELLO.TXT");
      n = tnfs_build_closedir(req, sizeof req, connid, 40, dh);
      xact(req, n, 40, TNFS_CMD_CLOSEDIR, &rep);
   }

   /* OPEN /HELLO.TXT read-only, READ it */
   n = tnfs_build_open(req, sizeof req, connid, 41, TNFS_O_RDONLY, 0, "/HELLO.TXT");
   uint8_t fd = 0xFF;
   CHECK(xact(req, n, 41, TNFS_CMD_OPEN, &rep) && rep.status==TNFS_OK && tnfs_reply_open(&rep,&fd),
         "OPEN /HELLO.TXT -> fd");
   {
      const uint8_t *data = NULL; uint16_t dl = 0;
      n = tnfs_build_read(req, sizeof req, connid, 42, fd, 200);
      CHECK(xact(req, n, 42, TNFS_CMD_READ, &rep) && rep.status==TNFS_OK && tnfs_reply_read(&rep,&data,&dl),
            "READ HELLO.TXT -> data");
      CHECK(dl > 0 && memcmp(data, "Hello from real tnfsd!", 22) == 0, "content matches what tnfsd serves");
      n = tnfs_build_close(req, sizeof req, connid, 43, fd);
      xact(req, n, 43, TNFS_CMD_CLOSE, &rep);
   }

   /* WRITE a new file then read it back */
   n = tnfs_build_open(req, sizeof req, connid, 44,
                       TNFS_O_WRONLY|TNFS_O_CREAT|TNFS_O_TRUNC, 0x01A4, "/FROMBEEB.TXT");
   CHECK(xact(req, n, 44, TNFS_CMD_OPEN, &rep) && rep.status==TNFS_OK && tnfs_reply_open(&rep,&fd),
         "OPEN(write) /FROMBEEB.TXT -> fd");
   {
      static const char *msg = "written via our codec";
      uint16_t wrote = 0;
      n = tnfs_build_write(req, sizeof req, connid, 45, fd, (const uint8_t*)msg, (uint16_t)strlen(msg));
      CHECK(xact(req, n, 45, TNFS_CMD_WRITE, &rep) && rep.status==TNFS_OK && tnfs_reply_write(&rep,&wrote),
            "WRITE -> bytes accepted");
      CHECK(wrote == strlen(msg), "server accepted all the bytes");
      n = tnfs_build_close(req, sizeof req, connid, 46, fd);
      xact(req, n, 46, TNFS_CMD_CLOSE, &rep);
      /* reopen + read back */
      n = tnfs_build_open(req, sizeof req, connid, 47, TNFS_O_RDONLY, 0, "/FROMBEEB.TXT");
      xact(req, n, 47, TNFS_CMD_OPEN, &rep); tnfs_reply_open(&rep, &fd);
      const uint8_t *data=NULL; uint16_t dl=0;
      n = tnfs_build_read(req, sizeof req, connid, 48, fd, 200);
      xact(req, n, 48, TNFS_CMD_READ, &rep); tnfs_reply_read(&rep, &data, &dl);
      CHECK(dl == strlen(msg) && memcmp(data, msg, dl) == 0, "read-back matches what we wrote");
      n = tnfs_build_close(req, sizeof req, connid, 49, fd); xact(req, n, 49, TNFS_CMD_CLOSE, &rep);
   }

   n = tnfs_build_umount(req, sizeof req, connid, 50);
   CHECK(xact(req, n, 50, TNFS_CMD_UMOUNT, &rep) && rep.status==TNFS_OK, "UMOUNT -> OK");

   printf("\n%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
