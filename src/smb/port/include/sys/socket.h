/* The part of <sys/socket.h> libsmb2 uses, for the Pi1MHz socket shim. */
#ifndef SMBP_SYS_SOCKET_H
#define SMBP_SYS_SOCKET_H
#include <stdint.h>
#include <sys/types.h>
typedef uint32_t socklen_t;
typedef uint8_t  sa_family_t;
struct sockaddr         { uint8_t sa_len; sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { uint8_t ss_len; sa_family_t ss_family; char ss_pad[126]; };
#define AF_UNSPEC    0
#define AF_INET      2
#define PF_INET      AF_INET
#define SOCK_STREAM  1
#define SOL_SOCKET   0xfff
#define SO_ERROR     0x1007
#define SO_KEEPALIVE 0x0008
#define SO_REUSEADDR 0x0004
#define SO_LINGER    0x0080
#define SOL_TCP      6
#define IPPROTO_TCP  6
struct linger { int l_onoff; int l_linger; };
int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
#endif
