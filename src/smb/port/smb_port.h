/* smb_port.h - force-included (-include) into every libsmb2 source file.

   libsmb2 calls the POSIX socket and process API directly.  Pi1MHz has no
   OS: lwIP runs NO_SYS with no sockets layer, and newlib's close/fcntl/time
   are stubs.  Each name libsmb2 uses is redirected here to an smbp_*
   function (smb/smb_socket.c, smb/smb_crypto.c), so the rest of the
   firmware keeps newlib's own symbols.  The macros come first so that the
   prototypes in newlib's headers, parsed afterwards, are renamed with them
   and keep their exact signatures. */
#ifndef SMB_PORT_H
#define SMB_PORT_H

#define socket          smbp_socket
#define connect         smbp_connect
#define setsockopt      smbp_setsockopt
#define getsockopt      smbp_getsockopt
#define bind            smbp_bind
#define listen          smbp_listen
#define accept          smbp_accept
#define readv           smbp_readv
#define writev          smbp_writev
#define close           smbp_close
#define fcntl           smbp_fcntl
#define poll            smbp_poll
#define getaddrinfo     smbp_getaddrinfo
#define freeaddrinfo    smbp_freeaddrinfo
#define getprotobyname  smbp_getprotobyname
#define htons           smbp_htons
#define htonl           smbp_htonl
#define ntohs           smbp_ntohs
#define ntohl           smbp_ntohl
#define time            smbp_time
#define getpid          smbp_getpid
#define gethostname     smbp_gethostname
#define getlogin_r      smbp_getlogin_r
#define arc4random_buf  smbp_random_bytes
#define fopen           smbp_fopen         /* NTLM_USER_FILE: there are no files */

#include <stddef.h>
#include <sys/types.h>
#include <time.h>
#include <stdio.h>

int  smbp_getlogin_r(char *buf, size_t size);
void smbp_random_bytes(void *buf, size_t len);

#endif
