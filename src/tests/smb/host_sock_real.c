/* The host half of the test's socket shim: the real libc calls, in a file
   that does not include smb_port.h, so their names are not renamed. */
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include "host_sock.h"

int  hs_socket(int d, int t, int p)                    { return socket(d, t, p); }
int  hs_connect(int fd, const void *a, unsigned l)     { return connect(fd, (const struct sockaddr *)a, (socklen_t)l); }
long hs_readv(int fd, const void *iov, int n)          { return (long)readv(fd, (const struct iovec *)iov, n); }
long hs_writev(int fd, const void *iov, int n)         { return (long)writev(fd, (const struct iovec *)iov, n); }
int  hs_close(int fd)                                  { return close(fd); }
int  hs_getsockopt(int fd, int lv, int nm, void *v, void *l)        { return getsockopt(fd, lv, nm, v, (socklen_t *)l); }
int  hs_setsockopt(int fd, int lv, int nm, const void *v, unsigned l) { return setsockopt(fd, lv, nm, v, (socklen_t)l); }
int  hs_nonblock(int fd)                               { return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
int  hs_getaddrinfo(const char *n, const char *s, const void *h, void *r)
{
   return getaddrinfo(n, s, (const struct addrinfo *)h, (struct addrinfo **)r);
}
void hs_freeaddrinfo(void *r)                          { freeaddrinfo((struct addrinfo *)r); }

/* poll() in place of the firmware shim's view of lwIP's state */
int hs_revents(int fd, int events)
{
   struct pollfd p = { fd, (short)events, 0 };
   if (poll(&p, 1, 1) <= 0)
      return 0;
   return p.revents;
}
