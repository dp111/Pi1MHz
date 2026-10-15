/* libsmb2 configuration for Pi1MHz (bare metal, newlib, lwIP raw API).
   The socket calls libsmb2 makes are redirected to the shim in
   smb/smb_socket.c by smb_port.h, force-included into every libsmb2 file. */
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDIO_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_NETDB_H 1
#define HAVE_POLL_H 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_LINGER 1                  /* struct linger is in our sys/socket.h */
#define HAVE_ARC4RANDOM_BUF 1          /* the BCM2835 hardware RNG */
#define STDC_HEADERS 1
#define CONFIGURE_OPTION_TCP_LINGER 1  /* no SO_LINGER/SO_REUSEADDR games */
