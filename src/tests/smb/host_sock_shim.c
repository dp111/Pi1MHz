/* The test's stand-in for smb/smb_socket.c: the same smbp_* interface over
   real sockets.  smb_port.h first, as libsmb2 has it, so the libc
   prototypes below are its smbp_* ones. */
#include "port/smb_port.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include "smb_socket.h"
#include "host_sock.h"

#define MAXFD 8
static int open_fds[MAXFD];
static int n_open;

int smbp_socket(int d, int t, int p)
{
   int fd = hs_socket(d, t, p);
   if (fd >= 0 && n_open < MAXFD)
      open_fds[n_open++] = fd;
   return fd;
}
int smbp_connect(int fd, const struct sockaddr *a, socklen_t l) { return hs_connect(fd, a, l); }
int smbp_close(int fd)
{
   for (int i = 0; i < n_open; i++)
      if (open_fds[i] == fd) {
         open_fds[i] = open_fds[--n_open];
         break;
      }
   return hs_close(fd);
}
ssize_t smbp_readv(int fd, const struct iovec *iov, int n)  { return (ssize_t)hs_readv(fd, iov, n); }
ssize_t smbp_writev(int fd, const struct iovec *iov, int n) { return (ssize_t)hs_writev(fd, iov, n); }
int smbp_getsockopt(int fd, int lv, int nm, void *v, socklen_t *l)       { return hs_getsockopt(fd, lv, nm, v, l); }
int smbp_setsockopt(int fd, int lv, int nm, const void *v, socklen_t l)  { return hs_setsockopt(fd, lv, nm, v, l); }
int smbp_fcntl(int fd, int cmd, ...)            { (void)cmd; return hs_nonblock(fd); }
int smbp_poll(struct pollfd *f, nfds_t n, int t) { (void)f; (void)n; (void)t; return 0; }
int smbp_getaddrinfo(const char *n, const char *s, const struct addrinfo *h, struct addrinfo **r)
{
   return hs_getaddrinfo(n, s, h, r);
}
void smbp_freeaddrinfo(struct addrinfo *r)       { hs_freeaddrinfo(r); }
int smbp_revents(int fd, int events)             { return hs_revents(fd, events); }
void smbp_close_all(void)                        { while (n_open > 0) hs_close(open_fds[--n_open]); }
/* the server side and byte order: unused by the client, but referenced */
int smbp_bind(int fd, const struct sockaddr *a, socklen_t l) { (void)fd; (void)a; (void)l; errno = EOPNOTSUPP; return -1; }
int smbp_listen(int fd, int b)                               { (void)fd; (void)b; errno = EOPNOTSUPP; return -1; }
int smbp_accept(int fd, struct sockaddr *a, socklen_t *l)    { (void)fd; (void)a; (void)l; errno = EOPNOTSUPP; return -1; }
uint16_t smbp_htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
uint32_t smbp_htonl(uint32_t v) { return __builtin_bswap32(v); }
uint16_t smbp_ntohs(uint16_t v) { return smbp_htons(v); }
uint32_t smbp_ntohl(uint32_t v) { return smbp_htonl(v); }
