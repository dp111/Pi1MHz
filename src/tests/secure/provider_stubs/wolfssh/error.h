#pragma once
/* Host-test stub: the wolfSSH return codes secure_service_wolfssh.c names.
   The values only need to be distinct. */
enum {
   WS_SUCCESS = 0,
   WS_FATAL_ERROR = -1001,
   WS_WANT_READ = -1002,
   WS_WANT_WRITE = -1003,
   WS_EOF = -1004,
   WS_CHANNEL_CLOSED = -1005,
   WS_REKEYING = -1006,
   WS_CBIO_ERR_WANT_READ = -1010,
   WS_CBIO_ERR_WANT_WRITE = -1011,
   WS_CBIO_ERR_CONN_RST = -1012,
   WS_CBIO_ERR_CONN_CLOSE = -1013,
   WOLFSSH_FTP_EOF = 1
};
