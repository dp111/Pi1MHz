#ifndef SMBP_POLL_H
#define SMBP_POLL_H
struct pollfd { int fd; short events, revents; };
#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
typedef unsigned int nfds_t;
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
#endif
