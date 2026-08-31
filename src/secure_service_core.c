#include "secure_service_core.h"

#include <string.h>

#define NTS_FINGERPRINT_ADDRESS 0x020500u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr24(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
}

static int buffer_ok(uint32_t address, uint32_t length, size_t size)
{
    return address < size && length <= size - address;
}

static const char *jim_string(uint8_t *jim, size_t size, uint32_t address)
{
    if (address >= size || memchr(jim + address, 0, size - address) == NULL)
        return NULL;
    return (const char *)jim + address;
}

static size_t bounded_length(const char *text, size_t maximum)
{
    size_t length = 0;
    while (length < maximum && text[length] != '\0') length++;
    return length;
}

static void wipe_bytes(uint8_t *data, size_t length)
{
    volatile uint8_t *p = data;
    while (length-- != 0u) *p++ = 0;
}

uint8_t nts_secure_dispatch(nts_secure_service *service, uint8_t *command,
                            uint8_t *jim, size_t jim_size)
{
    uint32_t address;
    uint32_t length;
    int result;

    if (service == NULL || service->port == NULL || command == NULL ||
        jim == NULL)
        return NTS_ERR_PARAM;

    switch (command[0]) {
    case NTS_SEC_CAPS:
        command[1] = 1;
        command[2] = 1;
        command[3] = (uint8_t)(1u | (service->managed_ssh ? 2u : 0u));
        if (service->managed_ssh && service->port->ssh_password != NULL)
            command[3] |= 4u;
        if (service->managed_ssh && service->port->sftp_open != NULL)
            command[3] |= 8u;
        command[4] = 0xB8;
        command[5] = 0x88;
        command[6] = service->managed_ssh ? 1 : 0;
        command[7] = 1; /* wolfSSH/wolfCrypt provider */
        memcpy(command + 8, "NTS", 3);
        return NTS_OK;

    case NTS_SEC_RANDOM:
        length = rd16(command + 1);
        address = rd32(command + 4);
        if (length == 0 || length > 64 || !buffer_ok(address, length, jim_size))
            return NTS_ERR_PARAM;
        if (service->port->random == NULL ||
            service->port->random(service->opaque, jim + address, length) != 0)
            return NTS_ERR_UNSUPPORTED;
        return NTS_OK;

    case NTS_SEC_SSH_OPEN: {
        const char *url = jim_string(jim, jim_size, rd32(command + 2));
        const char *username = jim_string(jim, jim_size, rd32(command + 6));
        char fingerprint[96] = { 0 };
        if (!service->managed_ssh || service->port->ssh_open == NULL)
            return NTS_ERR_UNSUPPORTED;
        if (url == NULL || username == NULL || *username == '\0')
            return NTS_ERR_PARAM;
        result = service->port->ssh_open(service->opaque, url, username,
                                         (command[1] & 1u) != 0,
                                         fingerprint);
        if (result == NTS_HOSTKEY_UNKNOWN) {
            size_t count = bounded_length(fingerprint, sizeof(fingerprint));
            if (count == sizeof(fingerprint) ||
                !buffer_ok(NTS_FINGERPRINT_ADDRESS, (uint32_t)count + 1u,
                           jim_size))
                return NTS_ERR_PARAM;
            memcpy(jim + NTS_FINGERPRINT_ADDRESS, fingerprint, count + 1u);
        }
        return (uint8_t)result;
    }

    case NTS_SEC_SSH_READ:
        length = (uint32_t)command[1] | ((uint32_t)command[2] << 8) |
                 ((uint32_t)command[3] << 16);
        address = rd32(command + 4);
        if (service->port->ssh_read == NULL ||
            !buffer_ok(address, length, jim_size))
            return NTS_ERR_PARAM;
        result = service->port->ssh_read(service->opaque, jim + address,
                                         length);
        if (result < 0)
            return (uint8_t)-result;
        wr24(command + 1, (uint32_t)result);
        return NTS_OK;

    case NTS_SEC_SSH_WRITE:
        length = (uint32_t)command[1] | ((uint32_t)command[2] << 8) |
                 ((uint32_t)command[3] << 16);
        address = rd32(command + 4);
        if (service->port->ssh_write == NULL ||
            !buffer_ok(address, length, jim_size))
            return NTS_ERR_PARAM;
        result = service->port->ssh_write(service->opaque, jim + address,
                                          length);
        if (result < 0)
            return (uint8_t)-result;
        wr24(command + 1, (uint32_t)result);
        return NTS_OK;

    case NTS_SEC_SSH_CLOSE:
        if (service->port->ssh_close != NULL)
            service->port->ssh_close(service->opaque);
        return NTS_OK;

    case NTS_SEC_SSH_PASSWORD:
        length = command[1];
        address = rd32(command + 4);
        if (!service->managed_ssh || service->port->ssh_password == NULL ||
            length == 0u || length > 127u ||
            !buffer_ok(address, length, jim_size))
            return NTS_ERR_PARAM;
        result = service->port->ssh_password(service->opaque,
                                             jim + address, length);
        wipe_bytes(jim + address, length);
        return result == 0 ? NTS_OK : NTS_ERR_PARAM;

    case NTS_SEC_SFTP_OPEN: {
        const char *url = jim_string(jim, jim_size, rd32(command + 2));
        const char *username = jim_string(jim, jim_size, rd32(command + 6));
        char fingerprint[96] = { 0 };
        if (!service->managed_ssh || service->port->sftp_open == NULL)
            return NTS_ERR_UNSUPPORTED;
        if (url == NULL || username == NULL || *username == '\0')
            return NTS_ERR_PARAM;
        result = service->port->sftp_open(service->opaque, url, username,
                                          (command[1] & 1u) != 0,
                                          fingerprint);
        if (result == NTS_HOSTKEY_UNKNOWN) {
            size_t count = bounded_length(fingerprint, sizeof(fingerprint));
            if (count == sizeof(fingerprint) ||
                !buffer_ok(NTS_FINGERPRINT_ADDRESS, (uint32_t)count + 1u,
                           jim_size))
                return NTS_ERR_PARAM;
            memcpy(jim + NTS_FINGERPRINT_ADDRESS, fingerprint, count + 1u);
        }
        return (uint8_t)result;
    }

    case NTS_SEC_SFTP_PWD:
    case NTS_SEC_SFTP_CD:
    case NTS_SEC_SFTP_LS:
    case NTS_SEC_SFTP_DELETE:
    case NTS_SEC_SFTP_MKDIR:
    case NTS_SEC_SFTP_RMDIR: {
        const char *path = jim_string(jim, jim_size, rd32(command + 4));
        uint32_t output = rd32(command + 8);
        length = (uint32_t)command[1] | ((uint32_t)command[2] << 8) |
                 ((uint32_t)command[3] << 16);
        if (service->port->sftp_path == NULL || path == NULL ||
            !buffer_ok(output, length, jim_size))
            return NTS_ERR_PARAM;
        result = service->port->sftp_path(service->opaque, command[0], path,
                                          jim + output, length);
        if (result < 0) return (uint8_t)-result;
        if ((uint32_t)result > length) return NTS_ERR_PROTOCOL;
        wr24(command + 1, (uint32_t)result);
        return NTS_OK;
    }

    case NTS_SEC_SFTP_GET_OPEN:
    case NTS_SEC_SFTP_PUT_OPEN: {
        const char *path = jim_string(jim, jim_size, rd32(command + 4));
        if (path == NULL || *path == '\0') return NTS_ERR_PARAM;
        if (command[0] == NTS_SEC_SFTP_GET_OPEN) {
            if (service->port->sftp_get_open == NULL) return NTS_ERR_UNSUPPORTED;
            result = service->port->sftp_get_open(service->opaque, path);
        } else {
            if (service->port->sftp_put_open == NULL) return NTS_ERR_UNSUPPORTED;
            result = service->port->sftp_put_open(service->opaque, path);
        }
        return result < 0 ? (uint8_t)-result : NTS_OK;
    }

    case NTS_SEC_SFTP_GET_READ:
    case NTS_SEC_SFTP_PUT_WRITE:
        length = (uint32_t)command[1] | ((uint32_t)command[2] << 8) |
                 ((uint32_t)command[3] << 16);
        address = rd32(command + 4);
        if (!buffer_ok(address, length, jim_size)) return NTS_ERR_PARAM;
        if (command[0] == NTS_SEC_SFTP_GET_READ) {
            if (service->port->sftp_get_read == NULL) return NTS_ERR_UNSUPPORTED;
            result = service->port->sftp_get_read(service->opaque,
                                                   jim + address, length);
        } else {
            if (service->port->sftp_put_write == NULL) return NTS_ERR_UNSUPPORTED;
            result = service->port->sftp_put_write(service->opaque,
                                                    jim + address, length);
        }
        if (result < 0) return (uint8_t)-result;
        if ((uint32_t)result > length) return NTS_ERR_PROTOCOL;
        wr24(command + 1, (uint32_t)result);
        return NTS_OK;

    case NTS_SEC_SFTP_TRANSFER_CLOSE:
        if (service->port->sftp_transfer_close == NULL)
            return NTS_ERR_UNSUPPORTED;
        result = service->port->sftp_transfer_close(service->opaque);
        return result < 0 ? (uint8_t)-result : NTS_OK;

    case NTS_SEC_SFTP_CLOSE:
        if (service->port->sftp_close != NULL)
            service->port->sftp_close(service->opaque);
        return NTS_OK;

    default:
        return NTS_ERR_UNSUPPORTED;
    }
}
