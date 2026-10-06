#pragma once
/* Host-test stub of wolfSSH's ssh.h: the types and calls the Pi1MHz
   provider uses, with wolfSSH's shapes.  test_provider.c defines them. */
#include <stdint.h>
typedef uint8_t  byte;
typedef uint32_t word32;
typedef struct WOLFSSH_CTX WOLFSSH_CTX;
typedef struct WOLFSSH WOLFSSH;

#define WOLFSSH_ENDPOINT_CLIENT 1
#define WOLFSSH_FORMAT_SSH      1
#define WOLFSSH_FORMAT_OPENSSH  2
#define WOLFSSH_SESSION_TERMINAL 2
#define WOLFSSH_USERAUTH_PASSWORD  1
#define WOLFSSH_USERAUTH_PUBLICKEY 2
#define WOLFSSH_USERAUTH_SUCCESS   0
#define WOLFSSH_USERAUTH_FAILURE   1
#define WOLFSSH_MAX_HANDLE 256

typedef struct {
   const byte *publicKeyType; word32 publicKeyTypeSz;
   const byte *publicKey;     word32 publicKeySz;
   const byte *privateKey;    word32 privateKeySz;
} WS_UserAuthData_PublicKey;
typedef struct { const byte *password; word32 passwordSz; } WS_UserAuthData_Password;
typedef struct {
   union { WS_UserAuthData_Password password; WS_UserAuthData_PublicKey publicKey; } sf;
} WS_UserAuthData;

typedef int (*WS_CallbackIORecv)(WOLFSSH *, void *, word32, void *);
typedef int (*WS_CallbackIOSend)(WOLFSSH *, void *, word32, void *);
typedef int (*WS_CallbackUserAuth)(byte, WS_UserAuthData *, void *);
typedef int (*WS_CallbackPublicKeyCheck)(const byte *, word32, void *);

int  wolfSSH_Init(void);
int  wolfSSH_Cleanup(void);
WOLFSSH_CTX *wolfSSH_CTX_new(byte side, void *heap);
void wolfSSH_CTX_free(WOLFSSH_CTX *ctx);
void wolfSSH_SetIORecv(WOLFSSH_CTX *ctx, WS_CallbackIORecv cb);
void wolfSSH_SetIOSend(WOLFSSH_CTX *ctx, WS_CallbackIOSend cb);
void wolfSSH_SetUserAuth(WOLFSSH_CTX *ctx, WS_CallbackUserAuth cb);
void wolfSSH_CTX_SetPublicKeyCheck(WOLFSSH_CTX *ctx, WS_CallbackPublicKeyCheck cb);
WOLFSSH *wolfSSH_new(WOLFSSH_CTX *ctx);
void wolfSSH_free(WOLFSSH *ssh);
int  wolfSSH_SetUsername(WOLFSSH *ssh, const char *username);
int  wolfSSH_SetChannelType(WOLFSSH *ssh, byte type, byte *data, word32 sz);
void wolfSSH_SetIOReadCtx(WOLFSSH *ssh, void *ctx);
void wolfSSH_SetIOWriteCtx(WOLFSSH *ssh, void *ctx);
void wolfSSH_SetUserAuthCtx(WOLFSSH *ssh, void *ctx);
void wolfSSH_SetPublicKeyCheckCtx(WOLFSSH *ssh, void *ctx);
int  wolfSSH_connect(WOLFSSH *ssh);
int  wolfSSH_shutdown(WOLFSSH *ssh);
int  wolfSSH_get_error(const WOLFSSH *ssh);
int  wolfSSH_stream_read(WOLFSSH *ssh, byte *buf, word32 bufSz);
int  wolfSSH_stream_send(WOLFSSH *ssh, byte *buf, word32 bufSz);
int  wolfSSH_ChangeTerminalSize(WOLFSSH *ssh, word32 columns, word32 rows,
                                word32 widthPixels, word32 heightPixels);
int  wolfSSH_ReadKey_buffer(const byte *in, word32 inSz, byte format,
                            byte **out, word32 *outSz, const byte **outType,
                            word32 *outTypeSz, void *heap);
