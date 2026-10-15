#ifndef SMBP_NETDB_H
#define SMBP_NETDB_H
#include <sys/socket.h>
struct addrinfo { int ai_flags, ai_family, ai_socktype, ai_protocol; socklen_t ai_addrlen;
                  struct sockaddr *ai_addr; char *ai_canonname; struct addrinfo *ai_next; };
struct protoent { char *p_name; char **p_aliases; int p_proto; };
#define EAI_AGAIN    2
#define EAI_NONAME   3
#define EAI_SERVICE  4
#define EAI_FAIL     5
#define EAI_MEMORY   6
int  getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                 struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
struct protoent *getprotobyname(const char *name);
#endif
