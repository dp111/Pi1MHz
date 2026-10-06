#pragma once
/* Host-test stub (see wolfssh/ssh.h). */
#include "wolfssh/ssh.h"
#define WC_SHA256_DIGEST_SIZE 32
typedef struct { int dummy; } wc_Sha256;
int  wc_InitSha256(wc_Sha256 *sha);
int  wc_Sha256Update(wc_Sha256 *sha, const byte *data, word32 len);
int  wc_Sha256Final(wc_Sha256 *sha, byte *hash);
void wc_Sha256Free(wc_Sha256 *sha);
