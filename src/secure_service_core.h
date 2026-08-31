#ifndef NETTOOLS_SECURE_SERVICE_CORE_H
#define NETTOOLS_SECURE_SERVICE_CORE_H

#include <stddef.h>
#include <stdint.h>

enum {
    NTS_SEC_CAPS = 94,
    NTS_SEC_RANDOM = 95,
    NTS_SEC_SSH_OPEN = 96,
    NTS_SEC_SSH_READ = 97,
    NTS_SEC_SSH_WRITE = 98,
    NTS_SEC_SSH_CLOSE = 99,
    NTS_SEC_SSH_PASSWORD = 100,
    NTS_SEC_SFTP_OPEN = 101,
    NTS_SEC_SFTP_PWD = 102,
    NTS_SEC_SFTP_CD = 103,
    NTS_SEC_SFTP_LS = 104,
    NTS_SEC_SFTP_DELETE = 105,
    NTS_SEC_SFTP_MKDIR = 106,
    NTS_SEC_SFTP_RMDIR = 107,
    NTS_SEC_SFTP_GET_OPEN = 108,
    NTS_SEC_SFTP_GET_READ = 109,
    NTS_SEC_SFTP_PUT_OPEN = 110,
    NTS_SEC_SFTP_PUT_WRITE = 111,
    NTS_SEC_SFTP_TRANSFER_CLOSE = 112,
    NTS_SEC_SFTP_CLOSE = 113,
    NTS_OK = 0,
    NTS_PENDING = 1,
    NTS_EOF = 0x20,
    NTS_ERR_PARAM = 0x23,
    NTS_ERR_UNSUPPORTED = 0x27,
    NTS_ERR_PROTOCOL = 0x2B,
    NTS_HOSTKEY_UNKNOWN = 0x2C,
    NTS_AUTH_FAILED = 0x2D
};

typedef struct nts_secure_port {
    int (*random)(void *opaque, uint8_t *out, size_t length);
    uint8_t (*ssh_open)(void *opaque, const char *url, const char *username,
                        int trust_unknown, char fingerprint[96]);
    int (*ssh_read)(void *opaque, uint8_t *out, size_t maximum);
    int (*ssh_write)(void *opaque, const uint8_t *data, size_t length);
    int (*ssh_password)(void *opaque, const uint8_t *password, size_t length);
    void (*ssh_close)(void *opaque);
    uint8_t (*sftp_open)(void *opaque, const char *url, const char *username,
                         int trust_unknown, char fingerprint[96]);
    int (*sftp_path)(void *opaque, uint8_t operation, const char *path,
                     uint8_t *out, size_t maximum);
    int (*sftp_get_open)(void *opaque, const char *path);
    int (*sftp_get_read)(void *opaque, uint8_t *out, size_t maximum);
    int (*sftp_put_open)(void *opaque, const char *path);
    int (*sftp_put_write)(void *opaque, const uint8_t *data, size_t length);
    int (*sftp_transfer_close)(void *opaque);
    void (*sftp_close)(void *opaque);
} nts_secure_port;

typedef struct nts_secure_service {
    const nts_secure_port *port;
    void *opaque;
    int managed_ssh;
} nts_secure_service;

uint8_t nts_secure_dispatch(nts_secure_service *service, uint8_t *command,
                            uint8_t *jim, size_t jim_size);

#endif
