/* Host tests for the secure service's ABI core.
 *
 * secure_service_core.c is provider-independent - it only ever reaches the
 * outside world through the nts_secure_port function table - so the whole
 * command decoder can be exercised here with a stub port.  That matters more
 * than usual for this file: PI1MHZ_SSH defaults OFF, so no firmware build
 * compiles it at all, and without this suite a change to it is checked by
 * nothing. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "secure_service_core.h"

#define JIM_SIZE 0x30000u          /* must cover NTS_FINGERPRINT_ADDRESS */
#define FINGERPRINT_ADDRESS 0x020500u

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

/* ---- stub port ---------------------------------------------------------- */

static int      stub_random_calls;
static size_t   stub_random_length;
static int      stub_read_return;      /* what ssh_read/get_read report */
static int      stub_write_return;
static uint8_t  stub_open_status;
static char     stub_fingerprint[96];
static int      stub_password_calls;
static uint8_t  stub_password_seen[8];

static int stub_random(void *opaque, uint8_t *out, size_t length)
{
   (void)opaque;
   stub_random_calls++;
   stub_random_length = length;
   memset(out, 0xA5, length);
   return 0;
}

static uint8_t stub_open(void *opaque, const char *url, const char *username,
                         int trust_unknown, char fingerprint[96])
{
   (void)opaque; (void)url; (void)username; (void)trust_unknown;
   memcpy(fingerprint, stub_fingerprint, sizeof stub_fingerprint);
   return stub_open_status;
}

static int stub_read(void *opaque, uint8_t *out, size_t maximum)
{
   (void)opaque;
   if (stub_read_return > 0 && (size_t)stub_read_return <= maximum)
      memset(out, 0x5A, (size_t)stub_read_return);
   return stub_read_return;
}

static int stub_write(void *opaque, const uint8_t *data, size_t length)
{
   (void)opaque; (void)data; (void)length;
   return stub_write_return;
}

static int stub_password(void *opaque, const uint8_t *password, size_t length)
{
   (void)opaque;
   stub_password_calls++;
   memcpy(stub_password_seen, password,
          length < sizeof stub_password_seen ? length : sizeof stub_password_seen);
   return 0;
}

static void stub_close(void *opaque) { (void)opaque; }

static const nts_secure_port stub_port = {
   .random = stub_random,
   .ssh_open = stub_open,
   .ssh_read = stub_read,
   .ssh_write = stub_write,
   .ssh_password = stub_password,
   .ssh_close = stub_close,
   .sftp_open = stub_open,
   .sftp_path = NULL,
   .sftp_get_open = NULL,
   .sftp_get_read = NULL,
   .sftp_put_open = NULL,
   .sftp_put_write = NULL,
   .sftp_transfer_close = NULL,
   .sftp_close = stub_close,
};

/* ---- helpers ------------------------------------------------------------ */

static uint8_t *jim;
static uint8_t command[64];
static nts_secure_service service;

static void wr32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr24at(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
}

static uint8_t run(void)
{
   return nts_secure_dispatch(&service, command, jim, JIM_SIZE);
}

int main(void)
{
   jim = calloc(1, JIM_SIZE);
   if (jim == NULL) return 1;
   service.port = &stub_port;
   service.opaque = NULL;
   service.managed_ssh = 1;
   service.random_ready = 1;

   puts("== capabilities ==");
   {
      uint8_t direct[64] = { 0 };
      memset(command, 0, sizeof command);
      command[0] = NTS_SEC_CAPS;
      ok(run() == NTS_OK, "CAPS answers OK");
      ok(command[4] == 0xB8u && command[5] == 0x88u, "CAPS carries the ABI signature");
      ok(memcmp(command + 8, "NTS", 3) == 0, "CAPS carries the provider tag");
      ok((command[3] & 1u) != 0u, "random bit set when the RNG is ready");
      ok((command[3] & 2u) != 0u, "managed-SSH bit set when SSH is ready");
      ok((command[3] & 4u) != 0u, "password bit follows the port table");
      ok((command[3] & 8u) != 0u, "sftp bit follows the port table");

      /* The FIQ fast path calls the helper directly; it must produce exactly
         what the dispatcher produces, or the host sees two different answers
         depending on which path served it. */
      direct[0] = NTS_SEC_CAPS;
      nts_secure_write_caps(&service, direct);
      ok(memcmp(direct, command, 11) == 0, "FIQ and dispatch CAPS replies agree");

      service.random_ready = 0;
      nts_secure_write_caps(&service, direct);
      ok((direct[3] & 1u) == 0u, "random bit clears when the RNG is not up");
      service.random_ready = 1;
   }

   puts("== random ==");
   {
      memset(command, 0, sizeof command);
      command[0] = NTS_SEC_RANDOM;
      command[1] = 0; command[2] = 0;
      wr32(command + 4, 0x100u);
      ok(run() == NTS_ERR_PARAM, "zero length refused");

      command[1] = 65u; command[2] = 0;
      ok(run() == NTS_ERR_PARAM, "over 64 bytes refused");

      command[1] = 16u;
      wr32(command + 4, JIM_SIZE - 8u);
      ok(run() == NTS_ERR_PARAM, "run past the end of JIM refused");

      wr32(command + 4, JIM_SIZE);
      ok(run() == NTS_ERR_PARAM, "address at the end of JIM refused");

      wr32(command + 4, 0xFFFFFFF0u);
      ok(run() == NTS_ERR_PARAM, "address+length overflow refused");

      stub_random_calls = 0;
      command[1] = 64u;
      wr32(command + 4, JIM_SIZE - 64u);
      ok(run() == NTS_OK, "exact fit at the top of JIM accepted");
      ok(stub_random_calls == 1 && stub_random_length == 64u, "provider saw the request");
      ok(jim[JIM_SIZE - 1u] == 0xA5u, "bytes landed in JIM");
   }

   puts("== strings ==");
   {
      memset(command, 0, sizeof command);
      command[0] = NTS_SEC_SSH_OPEN;
      /* An unterminated string running to the end of the window must be
         rejected rather than read past it. */
      memset(jim + JIM_SIZE - 4u, 'x', 4u);
      wr32(command + 2, JIM_SIZE - 4u);
      wr32(command + 6, 0x200u);
      strcpy((char *)jim + 0x200u, "user");
      ok(run() == NTS_ERR_PARAM, "unterminated URL refused");

      strcpy((char *)jim + 0x300u, "ssh://host");
      wr32(command + 2, 0x300u);
      jim[0x200u] = 0;                       /* empty username */
      ok(run() == NTS_ERR_PARAM, "empty username refused");
   }

   puts("== host key ==");
   {
      memset(command, 0, sizeof command);
      command[0] = NTS_SEC_SSH_OPEN;
      strcpy((char *)jim + 0x300u, "ssh://host");
      strcpy((char *)jim + 0x200u, "user");
      wr32(command + 2, 0x300u);
      wr32(command + 6, 0x200u);
      strcpy(stub_fingerprint, "SHA256:abcdef");
      stub_open_status = NTS_HOSTKEY_UNKNOWN;
      ok(run() == NTS_HOSTKEY_UNKNOWN, "unknown host key reported");
      ok(strcmp((char *)jim + FINGERPRINT_ADDRESS, "SHA256:abcdef") == 0,
         "fingerprint published at the agreed address");

      /* A fingerprint with no terminator inside the buffer must not be
         copied out of it. */
      memset(stub_fingerprint, 'x', sizeof stub_fingerprint);
      ok(run() == NTS_ERR_PARAM, "unterminated fingerprint refused");
      memset(stub_fingerprint, 0, sizeof stub_fingerprint);
      stub_open_status = NTS_OK;
   }

   puts("== transfer counts ==");
   {
      memset(command, 0, sizeof command);
      command[0] = NTS_SEC_SSH_READ;
      wr24at(command + 1, 32u);
      wr32(command + 4, 0x400u);
      stub_read_return = 16;
      ok(run() == NTS_OK, "short read accepted");
      ok(command[1] == 16u && command[2] == 0u && command[3] == 0u,
         "byte count reported back");

      /* A provider that claims more than the caller's buffer must not have
         that count passed on to the Beeb, which would then read bytes the
         provider never wrote. */
      stub_read_return = 64;
      ok(run() == NTS_ERR_PROTOCOL, "over-long read count refused");

      command[0] = NTS_SEC_SSH_WRITE;
      wr24at(command + 1, 32u);
      stub_write_return = 64;
      ok(run() == NTS_ERR_PROTOCOL, "over-long write count refused");
      stub_write_return = 8;
      ok(run() == NTS_OK && command[1] == 8u, "short write accepted");

      command[0] = NTS_SEC_SSH_READ;
      wr24at(command + 1, 32u);
      stub_read_return = -(int)NTS_EOF;
      ok(run() == NTS_EOF, "provider error passed through");
      stub_read_return = 0;
   }

   puts("== password ==");
   {
      memset(command, 0, sizeof command);
      command[0] = NTS_SEC_SSH_PASSWORD;
      memcpy(jim + 0x500u, "secret", 6);
      command[1] = 6u;
      wr32(command + 4, 0x500u);
      stub_password_calls = 0;
      ok(run() == NTS_OK, "password accepted");
      ok(stub_password_calls == 1 && memcmp(stub_password_seen, "secret", 6) == 0,
         "provider saw the password");
      ok(memcmp(jim + 0x500u, "\0\0\0\0\0\0", 6) == 0,
         "password wiped from JIM after use");

      command[1] = 0u;
      ok(run() == NTS_ERR_PARAM, "zero-length password refused");
      command[1] = 128u;
      ok(run() == NTS_ERR_PARAM, "over-long password refused");
   }

   puts("== unsupported ==");
   {
      nts_secure_service bare = { .port = &stub_port, .opaque = NULL,
                                  .managed_ssh = 0, .random_ready = 0 };
      memset(command, 0, sizeof command);
      command[0] = 200u;
      ok(run() == NTS_ERR_UNSUPPORTED, "command outside the ABI refused");

      command[0] = NTS_SEC_SSH_OPEN;
      strcpy((char *)jim + 0x300u, "ssh://host");
      strcpy((char *)jim + 0x200u, "user");
      wr32(command + 2, 0x300u);
      wr32(command + 6, 0x200u);
      ok(nts_secure_dispatch(&bare, command, jim, JIM_SIZE) == NTS_ERR_UNSUPPORTED,
         "SSH refused while the provider is not up");

      ok(nts_secure_dispatch(NULL, command, jim, JIM_SIZE) == NTS_ERR_PARAM,
         "null service refused");
      bare.port = NULL;
      ok(nts_secure_dispatch(&bare, command, jim, JIM_SIZE) == NTS_ERR_PARAM,
         "null port refused");
      ok(nts_secure_dispatch(&service, NULL, jim, JIM_SIZE) == NTS_ERR_PARAM,
         "null command refused");
   }

   free(jim);
   printf("\n%d checks, %d failures\n", checks, fails);
   return fails != 0;
}
