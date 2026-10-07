#pragma once
/* Host-test stub of wolfSSH's wolfsftp.h (see ssh.h). */
#include "wolfssh/ssh.h"
#define WOLFSSH_FXF_READ  0x01
#define WOLFSSH_FXF_WRITE 0x02
#define WOLFSSH_FXF_CREAT 0x08
#define WOLFSSH_FXF_TRUNC 0x10
typedef struct WS_SFTP_FILEATRB WS_SFTP_FILEATRB;
typedef struct WS_SFTPNAME {
   char *fName;
   char *lName;
   struct WS_SFTPNAME *next;
} WS_SFTPNAME;
int  wolfSSH_SFTP_connect(WOLFSSH *ssh);
WS_SFTPNAME *wolfSSH_SFTP_RealPath(WOLFSSH *ssh, char *dir);
WS_SFTPNAME *wolfSSH_SFTP_LS(WOLFSSH *ssh, char *dir);
void wolfSSH_SFTPNAME_list_free(WS_SFTPNAME *name);
int  wolfSSH_SFTP_Remove(WOLFSSH *ssh, char *f);
int  wolfSSH_SFTP_MKDIR(WOLFSSH *ssh, char *dir, WS_SFTP_FILEATRB *atr);
int  wolfSSH_SFTP_RMDIR(WOLFSSH *ssh, char *dir);
int  wolfSSH_SFTP_Open(WOLFSSH *ssh, char *dir, word32 reason,
                       WS_SFTP_FILEATRB *atr, byte *handle, word32 *handleSz);
int  wolfSSH_SFTP_SendReadPacket(WOLFSSH *ssh, byte *handle, word32 handleSz,
                                 const word32 ofst[2], byte *out, word32 outSz);
int  wolfSSH_SFTP_SendWritePacket(WOLFSSH *ssh, byte *handle, word32 handleSz,
                                  const word32 ofst[2], byte *in, word32 inSz);
int  wolfSSH_SFTP_Close(WOLFSSH *ssh, byte *handle, word32 handleSz);
