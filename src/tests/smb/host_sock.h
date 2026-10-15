#ifndef HOST_SOCK_H
#define HOST_SOCK_H
/* Real libc calls, from a file that does not rename them (host_sock_real.c) */
int  hs_socket(int domain, int type, int protocol);
int  hs_connect(int fd, const void *addr, unsigned len);
long hs_readv(int fd, const void *iov, int n);
long hs_writev(int fd, const void *iov, int n);
int  hs_close(int fd);
int  hs_getsockopt(int fd, int level, int name, void *val, void *len);
int  hs_setsockopt(int fd, int level, int name, const void *val, unsigned len);
int  hs_nonblock(int fd);
int  hs_revents(int fd, int events);
int  hs_getaddrinfo(const char *node, const char *service, const void *hints, void *res);
void hs_freeaddrinfo(void *res);
#endif
