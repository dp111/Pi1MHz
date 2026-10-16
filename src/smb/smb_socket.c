/* smb_socket.c - the socket calls libsmb2 makes, over lwIP's raw TCP API.

   lwIP runs NO_SYS with no sockets layer (wifi/lwipopts.h), and libsmb2
   calls socket/connect/readv/writev/close directly (lib/socket.c has no
   transport hook).  smb_port.h renames those calls to the smbp_* functions
   here.  Everything is non-blocking, as libsmb2 asks: connect answers
   EINPROGRESS, readv/writev answer EAGAIN, and smbp_revents() tells the
   pump (smb_client.c) which of libsmb2's events are ready - the job poll()
   does on a real OS.  Main loop only: the lwIP callbacks run inside
   wifi_lwip_poll(). */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#include "smb_socket.h"
/* After lwIP's headers: the renames apply to the port headers' prototypes,
   which then declare the smbp_* functions defined here.  lwIP's byte-order
   macros go first - its lwip_htons() and friends stay. */
#undef htons
#undef htonl
#undef ntohs
#undef ntohl
#include "port/smb_port.h"
#include <sys/socket.h>                /* smb/port/include, this file only */
#include <sys/uio.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>

/* One connection, plus one mid-reconnect: libsmb2 opens its next socket
   before the old one's close has gone through. */
#define SMBP_SOCKETS   2
#define SMBP_FD_BASE   10              /* any non-negative fd is valid to libsmb2 */

typedef enum { SK_FREE, SK_CREATED, SK_CONNECTING, SK_CONNECTED, SK_FAILED } sk_state_t;

typedef struct {
   sk_state_t      state;
   struct tcp_pcb *pcb;
   struct pbuf    *rx;                 /* received, not yet read (a chain) */
   uint16_t        rx_off;             /* bytes of rx already read */
   bool            peer_closed;        /* FIN seen: readv answers 0 once rx is empty */
   int             err;                /* SO_ERROR */
} smbp_sock_t;

static smbp_sock_t sk[SMBP_SOCKETS];

static smbp_sock_t *sock_of(int fd)
{
   int i = fd - SMBP_FD_BASE;
   if (i < 0 || i >= SMBP_SOCKETS || sk[i].state == SK_FREE)
      return NULL;
   return &sk[i];
}

/* ---- lwIP callbacks (inside wifi_lwip_poll) ------------------------------ */

static err_t sk_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
   smbp_sock_t *s = (smbp_sock_t *)arg;
   (void)pcb;
   if (s == NULL) {
      if (p != NULL)
         pbuf_free(p);
      return ERR_OK;
   }
   if (p == NULL) {                    /* the server closed its side */
      s->peer_closed = true;
      return ERR_OK;
   }
   if (err != ERR_OK) {
      pbuf_free(p);
      return err;
   }
   if (s->rx == NULL)
      s->rx = p;
   else
      pbuf_cat(s->rx, p);
   /* tcp_recved() is called as the bytes are read, so the window closes
      while libsmb2 is not reading: the server waits, nothing is dropped */
   return ERR_OK;
}

static void sk_error(void *arg, err_t err)
{
   smbp_sock_t *s = (smbp_sock_t *)arg;
   if (s == NULL)
      return;
   s->pcb = NULL;                      /* lwIP has freed it */
   s->state = SK_FAILED;
   s->err = (err == ERR_RST) ? ECONNRESET :
            (err == ERR_ABRT) ? ECONNABORTED : ECONNREFUSED;
}

static err_t sk_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
   smbp_sock_t *s = (smbp_sock_t *)arg;
   (void)pcb;
   if (s == NULL)
      return ERR_OK;
   if (err != ERR_OK) {
      s->state = SK_FAILED;
      s->err = ECONNREFUSED;
      return ERR_OK;
   }
   s->state = SK_CONNECTED;
   return ERR_OK;
}

/* ---- the socket calls --------------------------------------------------- */

int smbp_socket(int domain, int type, int protocol)
{
   (void)protocol;
   if (domain != AF_INET || type != SOCK_STREAM) {
      errno = EAFNOSUPPORT;
      return -1;
   }
   for (int i = 0; i < SMBP_SOCKETS; i++) {
      if (sk[i].state != SK_FREE)
         continue;
      struct tcp_pcb *pcb = tcp_new();
      if (pcb == NULL) {
         errno = ENOMEM;
         return -1;
      }
      memset(&sk[i], 0, sizeof sk[i]);
      sk[i].state = SK_CREATED;
      sk[i].pcb = pcb;
      tcp_arg(pcb, &sk[i]);
      tcp_recv(pcb, sk_recv);
      tcp_err(pcb, sk_error);
      tcp_nagle_disable(pcb);
      return SMBP_FD_BASE + i;
   }
   errno = EMFILE;
   return -1;
}

int smbp_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
   smbp_sock_t *s = sock_of(fd);
   struct sockaddr_in sin;
   if (s == NULL || s->pcb == NULL || s->state != SK_CREATED || addr == NULL ||
       len < sizeof sin) {
      errno = EINVAL;
      return -1;
   }
   memcpy(&sin, addr, sizeof sin);     /* a sockaddr is only byte-aligned */
   if (sin.sin_family != AF_INET) {
      errno = EAFNOSUPPORT;
      return -1;
   }
   ip_addr_t ip;
   ip_addr_set_ip4_u32(&ip, sin.sin_addr.s_addr);      /* network order, as lwIP keeps it */
   err_t rc = tcp_connect(s->pcb, &ip, lwip_ntohs(sin.sin_port), sk_connected);
   if (rc != ERR_OK) {
      errno = ECONNREFUSED;
      return -1;
   }
   s->state = SK_CONNECTING;
   errno = EINPROGRESS;
   return -1;
}

int smbp_close(int fd)
{
   smbp_sock_t *s = sock_of(fd);
   if (s == NULL) {
      errno = EBADF;
      return -1;
   }
   if (s->pcb != NULL) {
      tcp_arg(s->pcb, NULL);
      tcp_recv(s->pcb, NULL);
      tcp_err(s->pcb, NULL);
      if (tcp_close(s->pcb) != ERR_OK)
         tcp_abort(s->pcb);
   }
   if (s->rx != NULL)
      pbuf_free(s->rx);
   memset(s, 0, sizeof *s);            /* SK_FREE */
   return 0;
}

ssize_t smbp_readv(int fd, const struct iovec *iov, int iovcnt)
{
   smbp_sock_t *s = sock_of(fd);
   if (s == NULL) {
      errno = EBADF;
      return -1;
   }
   if (s->rx == NULL) {
      if (s->state == SK_FAILED) {
         errno = s->err ? s->err : ECONNRESET;
         return -1;
      }
      if (s->peer_closed)
         return 0;
      errno = EAGAIN;
      return -1;
   }
   size_t done = 0;
   for (int i = 0; i < iovcnt && s->rx != NULL; i++) {
      size_t want = iov[i].iov_len;
      uint8_t *dst = (uint8_t *)iov[i].iov_base;
      while (want > 0u && s->rx != NULL) {
         uint16_t avail = (uint16_t)(s->rx->tot_len - s->rx_off);
         uint16_t n = (want < avail) ? (uint16_t)want : avail;
         pbuf_copy_partial(s->rx, dst, n, s->rx_off);
         dst += n;
         want -= n;
         done += n;
         s->rx_off = (uint16_t)(s->rx_off + n);
         /* drop the pbufs read completely */
         while (s->rx != NULL && s->rx_off >= s->rx->len) {
            struct pbuf *next = s->rx->next;
            s->rx_off = (uint16_t)(s->rx_off - s->rx->len);
            if (next != NULL)
               pbuf_ref(next);
            pbuf_free(s->rx);          /* frees this one only: next held */
            s->rx = next;
         }
      }
   }
   if (s->pcb != NULL && done > 0u)
      tcp_recved(s->pcb, (u16_t)done);
   return (ssize_t)done;
}

ssize_t smbp_writev(int fd, const struct iovec *iov, int iovcnt)
{
   smbp_sock_t *s = sock_of(fd);
   if (s == NULL) {
      errno = EBADF;
      return -1;
   }
   if (s->state != SK_CONNECTED || s->pcb == NULL) {
      errno = (s->state == SK_FAILED) ? (s->err ? s->err : EPIPE) : ENOTCONN;
      return -1;
   }
   size_t room = tcp_sndbuf(s->pcb);
   if (room == 0u || tcp_sndqueuelen(s->pcb) >= TCP_SND_QUEUELEN - 4) {
      errno = EAGAIN;
      return -1;
   }
   size_t done = 0;
   for (int i = 0; i < iovcnt && room > 0u; i++) {
      size_t n = iov[i].iov_len < room ? iov[i].iov_len : room;
      if (n == 0u)
         continue;
      /* 16-bit lengths: split a long vector */
      size_t off = 0;
      while (off < n) {
         u16_t chunk = (u16_t)((n - off) > 0xFFFFu ? 0xFFFFu : (n - off));
         /* PSH on the request's last segment: MORE only while more follows */
         bool last = off + chunk == n && (i == iovcnt - 1 || n < iov[i].iov_len || room == n);
         err_t rc = tcp_write(s->pcb, (const uint8_t *)iov[i].iov_base + off, chunk,
                              TCP_WRITE_FLAG_COPY | (last ? 0u : TCP_WRITE_FLAG_MORE));
         if (rc != ERR_OK)
            goto out;                  /* out of segments: send what went in */
         off += chunk;
         done += chunk;
      }
      room -= n;
      if (n < iov[i].iov_len)
         break;
   }
out:
   if (done == 0u) {
      errno = EAGAIN;
      return -1;
   }
   tcp_output(s->pcb);
   return (ssize_t)done;
}

int smbp_getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
   smbp_sock_t *s = sock_of(fd);
   if (s == NULL) {
      errno = EBADF;
      return -1;
   }
   if (level == SOL_SOCKET && name == SO_ERROR && val != NULL && len != NULL &&
       *len >= sizeof(int)) {
      *(int *)val = (s->state == SK_FAILED) ? (s->err ? s->err : ECONNREFUSED) : 0;
      *len = sizeof(int);
      return 0;
   }
   errno = ENOPROTOOPT;
   return -1;
}

int smbp_setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
   (void)level; (void)name; (void)val; (void)len;
   return sock_of(fd) != NULL ? 0 : -1;          /* TCP_NODELAY is set at creation */
}

int smbp_fcntl(int fd, int cmd, ...)
{
   (void)cmd;
   return sock_of(fd) != NULL ? 0 : -1;          /* always non-blocking */
}

/* The client never uses libsmb2's synchronous calls, the only users. */
int smbp_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
   (void)fds; (void)nfds; (void)timeout;
   return 0;
}

int smbp_bind(int fd, const struct sockaddr *addr, socklen_t len)   { (void)fd; (void)addr; (void)len; errno = EOPNOTSUPP; return -1; }
int smbp_listen(int fd, int backlog)                                { (void)fd; (void)backlog; errno = EOPNOTSUPP; return -1; }
int smbp_accept(int fd, struct sockaddr *addr, socklen_t *len)     { (void)fd; (void)addr; (void)len; errno = EOPNOTSUPP; return -1; }

/* Numeric IPv4 only: smb_client.c resolves a name with the firmware's DNS
   (asynchronous) before it hands libsmb2 the dotted address. */
int smbp_getaddrinfo(const char *node, const char *service,
                     const struct addrinfo *hints, struct addrinfo **res)
{
   (void)hints;
   ip4_addr_t ip;
   if (node == NULL || !ip4addr_aton(node, &ip))
      return EAI_NONAME;
   struct {
      struct addrinfo    ai;
      struct sockaddr_in sin;
   } *r = calloc(1, sizeof *r);
   if (r == NULL)
      return EAI_MEMORY;
   r->sin.sin_len = sizeof r->sin;
   r->sin.sin_family = AF_INET;
   r->sin.sin_port = lwip_htons((uint16_t)(service ? atoi(service) : 445));
   r->sin.sin_addr.s_addr = ip4_addr_get_u32(&ip);
   r->ai.ai_family = AF_INET;
   r->ai.ai_socktype = SOCK_STREAM;
   r->ai.ai_addrlen = sizeof r->sin;
   r->ai.ai_addr = (struct sockaddr *)&r->sin;
   *res = &r->ai;
   return 0;
}

void smbp_freeaddrinfo(struct addrinfo *res)
{
   free(res);                          /* one allocation per lookup, above */
}

struct protoent *smbp_getprotobyname(const char *name)
{
   static char tcp_name[] = "tcp";
   static struct protoent tcp = { tcp_name, NULL, 6 };
   return (name != NULL && strcmp(name, "tcp") == 0) ? &tcp : NULL;
}

uint16_t smbp_htons(uint16_t v) { return lwip_htons(v); }
uint32_t smbp_htonl(uint32_t v) { return lwip_htonl(v); }
uint16_t smbp_ntohs(uint16_t v) { return lwip_ntohs(v); }
uint32_t smbp_ntohl(uint32_t v) { return lwip_ntohl(v); }

/* ---- for the pump ------------------------------------------------------- */

int smbp_revents(int fd, int events)
{
   smbp_sock_t *s = sock_of(fd);
   if (s == NULL)
      return 0;
   /* bytes that arrived before the error are still the server's reply */
   if (s->state == SK_FAILED && s->rx == NULL)
      return POLLERR;
   int r = 0;
   if ((events & POLLIN) && (s->rx != NULL || s->peer_closed))
      r |= POLLIN;
   if ((events & POLLOUT) && s->state == SK_CONNECTED && s->pcb != NULL &&
       tcp_sndbuf(s->pcb) > 0u)
      r |= POLLOUT;
   return r;
}

void smbp_close_all(void)
{
   for (int i = 0; i < SMBP_SOCKETS; i++)
      if (sk[i].state != SK_FREE)
         (void)smbp_close(SMBP_FD_BASE + i);
}
