#ifndef SMBP_NETINET_IN_H
#define SMBP_NETINET_IN_H
#include <sys/socket.h>
struct in_addr     { uint32_t s_addr; };
struct sockaddr_in { uint8_t sin_len; sa_family_t sin_family; uint16_t sin_port;
                     struct in_addr sin_addr; char sin_zero[8]; };
#define INADDR_ANY 0u
uint16_t htons(uint16_t v);
uint32_t htonl(uint32_t v);
uint16_t ntohs(uint16_t v);
uint32_t ntohl(uint32_t v);
#endif
