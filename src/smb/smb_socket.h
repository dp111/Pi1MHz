/* smb_socket.h - the socket shim under libsmb2 (smb_socket.c). */
#ifndef SMB_SOCKET_H
#define SMB_SOCKET_H

/* Which of libsmb2's poll events (POLLIN/POLLOUT, from smb2_which_events)
   are ready on fd, plus POLLERR for a failed connection. */
int  smbp_revents(int fd, int events);

/* Drop every socket at once (a link loss, a BREAK). */
void smbp_close_all(void);

#endif
