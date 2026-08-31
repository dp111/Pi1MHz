/* Bare-metal Pi1MHz wolfSSH provider.
 *
 * This file is copied into the Pi1MHz source tree by install.sh.  It keeps
 * every lwIP, FatFs and crypto operation in the normal poll context; the FIQ
 * wrapper in secure_service.c only latches command bytes.
 */
#include "secure_service_wolfssh.h"

#include "BeebSCSI/fatfs/ff.h"
#include "rpi/base.h"
#include "rpi/systimer.h"
#include "wifi/wifi_lwip.h"
#include "lwip/altcp.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include <wolfssh/error.h>
#include <wolfssh/ssh.h>
#include <wolfssh/wolfsftp.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SSH_RX_SIZE 16384u
#define SSH_FILE_SIZE 4096u
#define SSH_DIR "Pi1MHz/ssh"
#define SSH_PRIVATE SSH_DIR "/id_ed25519"
#define SSH_PUBLIC SSH_DIR "/id_ed25519.pub"
#define SSH_HOSTS SSH_DIR "/known_hosts"
#define SSH_HOSTS_TMP SSH_DIR "/known_hosts.tmp"
#define SSH_HOSTS_BAK SSH_DIR "/known_hosts.bak"

#define NTS_ERR_DNS 0x24u
#define NTS_ERR_INUSE 0x21u
#define NTS_ERR_CONN 0x25u
#define NTS_ERR_PROTOCOL 0x2Bu

#define BCM_RNG_BASE (PERIPHERAL_BASE + 0x104000u)
#define BCM_RNG_CTRL (*(volatile uint32_t *)(BCM_RNG_BASE + 0x00u))
#define BCM_RNG_STATUS (*(volatile uint32_t *)(BCM_RNG_BASE + 0x04u))
#define BCM_RNG_DATA (*(volatile uint32_t *)(BCM_RNG_BASE + 0x08u))
#define BCM_RNG_INT_MASK (*(volatile uint32_t *)(BCM_RNG_BASE + 0x10u))

typedef enum {
    SSH_IDLE, SSH_RESOLVING, SSH_CONNECTING, SSH_HANDSHAKE, SSH_RESIZE,
    SSH_UP, SSH_FAILED
} ssh_stage;

typedef struct {
    ssh_stage stage;
    struct altcp_pcb *pcb;
    ip_addr_t address;
    bool dns_done;
    bool dns_ok;
    bool eof;
    bool transport_error;
    bool trust_unknown;
    bool host_unknown;
    bool host_changed;
    uint16_t port;
    uint16_t rx_head;
    uint16_t rx_tail;
    uint16_t rx_count;
    char host[192];
    char host_id[224];
    char username[64];
    char fingerprint[96];
    char host_key_type[64];
    char host_key[2048];
    WOLFSSH_CTX *wolf_ctx;
    WOLFSSH *ssh;
    byte *public_key;
    word32 public_key_size;
    const byte *public_key_type;
    word32 public_key_type_size;
    byte *private_key;
    word32 private_key_size;
    const byte *private_key_type;
    word32 private_key_type_size;
    byte password[128];
    word32 password_size;
    bool sftp_mode;
    byte sftp_handle[WOLFSSH_MAX_HANDLE];
    word32 sftp_handle_size;
    word32 sftp_offset[2];
    uint8_t sftp_transfer; /* 0 none, 1 download, 2 upload */
    char sftp_cwd[256];
} pi_ssh;

static pi_ssh client;
static uint8_t rx_ring[SSH_RX_SIZE];
static uint8_t file_buffer[SSH_FILE_SIZE + 1u];
static bool provider_ready;
static bool wolfssh_started;
static bool wolfssh_init_attempted;
static bool rng_started;
static bool rng_ready;
static bool rng_have_last;
static uint32_t rng_last;
static uint8_t rng_sample_count;
static uint8_t rng_zero_count;
static uint8_t rng_ones_count;

/* Warm-up is not waited for here. rng_begin discards 0x40000 oscillator bits
   and rng_poll samples the result from the cooperative poll loop, so by the
   time a caller reaches this the generator is running at its hardware rate and
   a word is due in microseconds. The bound is a safety net against a generator
   that stops, not a settling time: at 750ms it was long enough to stall the
   1MHz bus service for most of a second. */
#define RNG_WORD_DEADLINE_US 5000u

static int rng_word(uint32_t *out)
{
    uint32_t started_us = RPI_GetSystemTime();
    while ((BCM_RNG_STATUS >> 24) == 0u) {
        if (RPI_GetSystemTime() - started_us >= RNG_WORD_DEADLINE_US) return -1;
        RPI_WaitMicroSeconds(1u);
    }
    *out = BCM_RNG_DATA;
    if (rng_have_last && *out == rng_last) return -1;
    rng_last = *out;
    rng_have_last = true;
    return 0;
}

/* Named by CUSTOM_RAND_GENERATE_BLOCK in user_settings.h. */
int nts_bcm_random_block(unsigned char *out, unsigned int length)
{
    uint32_t word = 0;
    unsigned int available = 0;
    /* Fail fast rather than block. The settling wait belongs to rng_poll, and
       a caller which arrives before the generator is ready must retry: the
       capability byte reports readiness so the host can wait for it rather
       than have the Pi stall inside a command. */
    if (!rng_started || !rng_ready || out == NULL) return -1;
    while (length != 0u) {
        if (available == 0u) {
            if (rng_word(&word) != 0) return -1;
            available = 4u;
        }
        *out++ = (uint8_t)word;
        word >>= 8;
        available--;
        length--;
    }
    return 0;
}

static void rng_begin(void)
{
    BCM_RNG_INT_MASK |= 1u;
    BCM_RNG_STATUS = 0x00040000u; /* discard the first 0x40000 oscillator bits */
    BCM_RNG_CTRL |= 1u;
    rng_started = true;
    rng_ready = false;
    rng_have_last = false;
    rng_sample_count = 0u;
    rng_zero_count = 0u;
    rng_ones_count = 0u;
}

static void rng_poll(void)
{
    uint32_t word;
    if (!rng_started || rng_ready || (BCM_RNG_STATUS >> 24) == 0u) return;

    word = BCM_RNG_DATA;
    if (rng_have_last && word == rng_last) {
        rng_started = false;
        return;
    }
    rng_last = word;
    rng_have_last = true;
    for (unsigned int i = 0; i < 4u; i++) {
        uint8_t value = (uint8_t)word;
        if (value == 0u) rng_zero_count++;
        if (value == 0xffu) rng_ones_count++;
        word >>= 8;
    }
    rng_sample_count++;
    if (rng_sample_count == 8u) {
        rng_ready = rng_zero_count != 32u && rng_ones_count != 32u;
        if (!rng_ready) rng_started = false;
    }
}

static int port_random(void *opaque, uint8_t *out, size_t length)
{
    (void)opaque;
    if (length > UINT32_MAX) return -1;
    return nts_bcm_random_block(out, (unsigned int)length);
}

static int read_file(const char *path, uint8_t *buffer, size_t maximum,
                     size_t *length)
{
    FIL file;
    UINT got = 0;
    FSIZE_t size;
    if (f_open(&file, path, FA_READ) != FR_OK) return -1;
    size = f_size(&file);
    if (size > maximum || f_read(&file, buffer, (UINT)size, &got) != FR_OK ||
        got != (UINT)size) {
        (void)f_close(&file);
        return -1;
    }
    (void)f_close(&file);
    *length = got;
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t length)
{
    FIL file;
    UINT wrote = 0;
    if (length > UINT_MAX ||
        f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    if (f_write(&file, data, (UINT)length, &wrote) != FR_OK ||
        wrote != (UINT)length || f_sync(&file) != FR_OK) {
        (void)f_close(&file);
        return -1;
    }
    return f_close(&file) == FR_OK ? 0 : -1;
}

static void free_keys(void)
{
    if (client.private_key != NULL) {
        memset(client.private_key, 0, client.private_key_size);
        free(client.private_key);
    }
    free(client.public_key);
    client.private_key = NULL;
    client.public_key = NULL;
    client.private_key_size = client.public_key_size = 0;
}

static int load_keys(void)
{
    size_t length;
    int result;
    if (read_file(SSH_PRIVATE, file_buffer, SSH_FILE_SIZE, &length) != 0)
        return -1;
    result = wolfSSH_ReadKey_buffer(file_buffer, (word32)length,
        WOLFSSH_FORMAT_OPENSSH, &client.private_key, &client.private_key_size,
        &client.private_key_type, &client.private_key_type_size, NULL);
    memset(file_buffer, 0, length);
    if (result != WS_SUCCESS) return -1;
    if (read_file(SSH_PUBLIC, file_buffer, SSH_FILE_SIZE, &length) != 0)
        return -1;
    result = wolfSSH_ReadKey_buffer(file_buffer, (word32)length,
        WOLFSSH_FORMAT_SSH, &client.public_key, &client.public_key_size,
        &client.public_key_type, &client.public_key_type_size, NULL);
    memset(file_buffer, 0, length);
    return result == WS_SUCCESS ? 0 : -1;
}

static int auth_callback(byte type, WS_UserAuthData *data, void *opaque)
{
    pi_ssh *state = opaque;
    WS_UserAuthData_PublicKey *key;
    if (type == WOLFSSH_USERAUTH_PASSWORD && state->password_size != 0u) {
        data->sf.password.password = state->password;
        data->sf.password.passwordSz = state->password_size;
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    if (type != WOLFSSH_USERAUTH_PUBLICKEY || state->public_key == NULL ||
        state->private_key == NULL) return WOLFSSH_USERAUTH_FAILURE;
    key = &data->sf.publicKey;
    key->publicKeyType = state->public_key_type;
    key->publicKeyTypeSz = state->public_key_type_size;
    key->publicKey = state->public_key;
    key->publicKeySz = state->public_key_size;
    key->privateKey = state->private_key;
    key->privateKeySz = state->private_key_size;
    return WOLFSSH_USERAUTH_SUCCESS;
}

static int find_known_host(void)
{
    FILINFO info;
    size_t length;
    char *line;
    FRESULT stat_result = f_stat(SSH_HOSTS, &info);
    if (stat_result == FR_NO_FILE || stat_result == FR_NO_PATH) return 0;
    if (stat_result != FR_OK ||
        read_file(SSH_HOSTS, file_buffer, SSH_FILE_SIZE, &length) != 0)
        return -1;
    file_buffer[length] = 0;
    line = (char *)file_buffer;
    while (*line != '\0') {
        char *end = strchr(line, '\n');
        char *space = strchr(line, ' ');
        char *key_space;
        if (end == NULL) end = line + strlen(line);
        key_space = space != NULL ? strchr(space + 1, ' ') : NULL;
        if (space != NULL && key_space != NULL && key_space < end &&
            (size_t)(space - line) == strlen(client.host_id) &&
            memcmp(line, client.host_id, (size_t)(space - line)) == 0) {
            size_t type_len = (size_t)(key_space - space - 1);
            size_t key_len = (size_t)(end - key_space - 1);
            int result = type_len == strlen(client.host_key_type) &&
                key_len == strlen(client.host_key) &&
                memcmp(space + 1, client.host_key_type, type_len) == 0 &&
                memcmp(key_space + 1, client.host_key, key_len) == 0 ? 1 : -1;
            memset(file_buffer, 0, length + 1u);
            return result;
        }
        line = *end == '\0' ? end : end + 1;
    }
    memset(file_buffer, 0, length + 1u);
    return 0;
}

static int persist_known_host(void)
{
    FILINFO info;
    size_t length = 0;
    int count;
    bool had_old = f_stat(SSH_HOSTS, &info) == FR_OK;
    if (had_old && read_file(SSH_HOSTS, file_buffer, SSH_FILE_SIZE, &length) != 0)
        return -1;
    count = snprintf((char *)file_buffer + length, sizeof(file_buffer) - length,
                     "%s %s %s\n", client.host_id, client.host_key_type,
                     client.host_key);
    if (count <= 0 || (size_t)count >= sizeof(file_buffer) - length) return -1;
    if (write_file(SSH_HOSTS_TMP, file_buffer, length + (size_t)count) != 0)
        return -1;
    memset(file_buffer, 0, length + (size_t)count);
    (void)f_unlink(SSH_HOSTS_BAK);
    if (had_old && f_rename(SSH_HOSTS, SSH_HOSTS_BAK) != FR_OK) goto fail;
    if (f_rename(SSH_HOSTS_TMP, SSH_HOSTS) != FR_OK) {
        if (had_old) (void)f_rename(SSH_HOSTS_BAK, SSH_HOSTS);
        goto fail;
    }
    if (had_old) (void)f_unlink(SSH_HOSTS_BAK);
    return 0;
fail:
    (void)f_unlink(SSH_HOSTS_TMP);
    return -1;
}

static int hostkey_callback(const byte *key, word32 key_size, void *opaque)
{
    pi_ssh *state = opaque;
    wc_Sha256 sha;
    byte digest[WC_SHA256_DIGEST_SIZE];
    byte encoded[64];
    word32 encoded_size = sizeof(encoded);
    word32 host_key_size = sizeof(state->host_key) - 1u;
    word32 name_size;
    int known;
    int hash_result;
    if (key_size < 4u) return -1;
    name_size = ((word32)key[0] << 24) | ((word32)key[1] << 16) |
                ((word32)key[2] << 8) | key[3];
    if (name_size == 0u || name_size >= sizeof(state->host_key_type) ||
        name_size > key_size - 4u ||
        Base64_Encode_NoNl(key, key_size, (byte *)state->host_key,
                           &host_key_size) != 0) return -1;
    state->host_key[host_key_size] = '\0';
    memcpy(state->host_key_type, key + 4u, name_size);
    state->host_key_type[name_size] = '\0';
    hash_result = wc_InitSha256(&sha);
    if (hash_result == 0) hash_result = wc_Sha256Update(&sha, key, key_size);
    if (hash_result == 0) hash_result = wc_Sha256Final(&sha, digest);
    wc_Sha256Free(&sha);
    if (hash_result != 0 || Base64_Encode_NoNl(digest, sizeof(digest),
                                               encoded, &encoded_size) != 0)
        return -1;
    while (encoded_size != 0u && encoded[encoded_size - 1u] == '=')
        encoded_size--;
    if (encoded_size + 8u >= sizeof(state->fingerprint)) return -1;
    memcpy(state->fingerprint, "SHA256:", 7u);
    memcpy(state->fingerprint + 7u, encoded, encoded_size);
    state->fingerprint[7u + encoded_size] = '\0';
    known = find_known_host();
    if (known > 0) return 0;
    if (known < 0) { state->host_changed = true; return -1; }
    if (!state->trust_unknown) { state->host_unknown = true; return -1; }
    return persist_known_host();
}

static uint16_t ring_free(void) { return (uint16_t)(SSH_RX_SIZE - client.rx_count); }

static err_t nts_tcp_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p,
                          err_t error)
{
    pi_ssh *state = arg;
    if (state == NULL) { if (p != NULL) pbuf_free(p); return ERR_OK; }
    if (error != ERR_OK) {
        if (p != NULL) pbuf_free(p);
        state->transport_error = true;
        return ERR_OK;
    }
    if (p == NULL) { state->eof = true; return ERR_OK; }
    if (p->tot_len > ring_free()) return ERR_MEM;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const uint8_t *source = q->payload;
        for (uint16_t i = 0; i < q->len; i++) {
            rx_ring[state->rx_head] = source[i];
            state->rx_head = (uint16_t)((state->rx_head + 1u) &
                                        (SSH_RX_SIZE - 1u));
        }
        state->rx_count = (uint16_t)(state->rx_count + q->len);
    }
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t nts_tcp_connected(void *arg, struct altcp_pcb *pcb, err_t error)
{
    pi_ssh *state = arg;
    (void)pcb;
    if (state != NULL)
        state->stage = error == ERR_OK ? SSH_HANDSHAKE : SSH_FAILED;
    return ERR_OK;
}

static void nts_tcp_error(void *arg, err_t error)
{
    pi_ssh *state = arg;
    (void)error;
    if (state != NULL) {
        state->pcb = NULL;
        state->transport_error = true;
        state->stage = SSH_FAILED;
    }
}

static int io_recv(WOLFSSH *ssh, void *out, word32 size, void *opaque)
{
    pi_ssh *state = opaque;
    uint8_t *destination = out;
    word32 count = size < state->rx_count ? size : state->rx_count;
    (void)ssh;
    for (word32 i = 0; i < count; i++) {
        destination[i] = rx_ring[state->rx_tail];
        state->rx_tail = (uint16_t)((state->rx_tail + 1u) &
                                    (SSH_RX_SIZE - 1u));
    }
    state->rx_count = (uint16_t)(state->rx_count - count);
    if (count != 0u) return (int)count;
    if (state->transport_error) return WS_CBIO_ERR_CONN_RST;
    if (state->eof) return WS_CBIO_ERR_CONN_CLOSE;
    return WS_CBIO_ERR_WANT_READ;
}

static int io_send(WOLFSSH *ssh, void *data, word32 size, void *opaque)
{
    pi_ssh *state = opaque;
    u16_t available;
    word32 count;
    err_t result;
    (void)ssh;
    if (state->pcb == NULL || state->transport_error)
        return WS_CBIO_ERR_CONN_RST;
    available = altcp_sndbuf(state->pcb);
    count = size < available ? size : available;
    if (count == 0u) return WS_CBIO_ERR_WANT_WRITE;
    result = altcp_write(state->pcb, data, (u16_t)count, TCP_WRITE_FLAG_COPY);
    if (result == ERR_MEM) return WS_CBIO_ERR_WANT_WRITE;
    if (result != ERR_OK) return WS_CBIO_ERR_CONN_RST;
    (void)altcp_output(state->pcb);
    wifi_lwip_rx_kick();
    return (int)count;
}

static void dns_found(const char *name, const ip_addr_t *address, void *opaque)
{
    pi_ssh *state = opaque;
    (void)name;
    if (state == NULL) return;
    state->dns_done = true;
    state->dns_ok = address != NULL;
    if (address != NULL) state->address = *address;
}

static int parse_url(const char *url)
{
    const char *start, *end, *colon;
    size_t length;
    unsigned long port = 0;
    if (strncmp(url, "TCP://", 6u) != 0) return -1;
    start = url + 6u;
    end = strchr(start, '/');
    if (end == NULL) end = start + strlen(start);
    colon = end;
    while (colon > start && colon[-1] != ':') colon--;
    if (colon == start) return -1;
    length = (size_t)(colon - start - 1);
    if (length == 0u || length >= sizeof(client.host)) return -1;
    memcpy(client.host, start, length); client.host[length] = '\0';
    for (const char *p = colon; p < end; p++) {
        if (*p < '0' || *p > '9') return -1;
        port = port * 10u + (unsigned long)(*p - '0');
        if (port > 65535u) return -1;
    }
    if (port == 0u) return -1;
    client.port = (uint16_t)port;
    if (port == 22u) {
        if (length >= sizeof(client.host_id)) return -1;
        memcpy(client.host_id, client.host, length + 1u);
    } else {
        int count = snprintf(client.host_id, sizeof(client.host_id),
                             "[%s]:%u", client.host, (unsigned)client.port);
        if (count <= 0 || (size_t)count >= sizeof(client.host_id)) return -1;
    }
    return 0;
}

static bool want_io(int result)
{
    int error = client.ssh != NULL ? wolfSSH_get_error(client.ssh) : result;
    return result == WS_WANT_READ || result == WS_WANT_WRITE ||
           error == WS_WANT_READ || error == WS_WANT_WRITE;
}

static bool channel_finished(int result)
{
    int error = client.ssh != NULL ? wolfSSH_get_error(client.ssh) : result;
    return result == WS_EOF || result == WS_CHANNEL_CLOSED ||
           error == WS_EOF || error == WS_CHANNEL_CLOSED;
}

static void clear_password(void)
{
    volatile byte *p = client.password;
    size_t count = sizeof(client.password);
    while (count-- != 0u) *p++ = 0;
    client.password_size = 0;
}

static void close_connection(bool keep_password)
{
    byte saved_password[128];
    word32 saved_size = 0;
    if (keep_password && client.password_size != 0u) {
        saved_size = client.password_size;
        memcpy(saved_password, client.password, saved_size);
    }
    if (client.ssh != NULL) {
        if (client.stage == SSH_UP) (void)wolfSSH_shutdown(client.ssh);
        wolfSSH_free(client.ssh);
    }
    if (client.wolf_ctx != NULL) wolfSSH_CTX_free(client.wolf_ctx);
    if (client.pcb != NULL) {
        struct altcp_pcb *pcb = client.pcb;
        client.pcb = NULL;
        altcp_arg(pcb, NULL); altcp_recv(pcb, NULL); altcp_sent(pcb, NULL);
        altcp_poll(pcb, NULL, 0); altcp_err(pcb, NULL);
        if (altcp_close(pcb) != ERR_OK) altcp_abort(pcb);
    }
    free_keys();
    memset(&client, 0, sizeof(client));
    if (saved_size != 0u) {
        memcpy(client.password, saved_password, saved_size);
        client.password_size = saved_size;
        memset(saved_password, 0, sizeof(saved_password));
    }
}

static void close_client(void) { close_connection(false); }

static uint8_t start_connection(const char *url, const char *username,
                                int trust_unknown, bool sftp_mode)
{
    err_t result;
    if (parse_url(url) != 0 || *username == '\0' ||
        strlen(username) >= sizeof(client.username)) return NTS_ERR_PARAM;
    strcpy(client.username, username);
    client.sftp_mode = sftp_mode;
    client.trust_unknown = trust_unknown != 0;
    if (load_keys() != 0) {
        free_keys();
        if (client.password_size == 0u) return NTS_AUTH_FAILED;
    }
    result = dns_gethostbyname(client.host, &client.address, dns_found, &client);
    if (result == ERR_INPROGRESS) { client.stage = SSH_RESOLVING; return NTS_PENDING; }
    if (result != ERR_OK) return NTS_ERR_DNS;
    client.dns_done = client.dns_ok = true;
    client.stage = SSH_RESOLVING;
    return NTS_PENDING;
}

static uint8_t start_tcp(void)
{
    if (!client.dns_done) return NTS_PENDING;
    if (!client.dns_ok) return NTS_ERR_DNS;
    if (!wifi_lwip_get_context()->address_ready) return NTS_PENDING;
    client.pcb = altcp_new_ip_type(NULL, IPADDR_TYPE_V4);
    if (client.pcb == NULL) return NTS_ERR_CONN;
#if !LWIP_ALTCP
    client.pcb->rcv_wnd = SSH_RX_SIZE;
    client.pcb->rcv_ann_wnd = SSH_RX_SIZE;
#endif
    altcp_arg(client.pcb, &client);
    altcp_recv(client.pcb, nts_tcp_recv);
    altcp_err(client.pcb, nts_tcp_error);
    if (altcp_connect(client.pcb, &client.address, client.port,
                      nts_tcp_connected) != ERR_OK) return NTS_ERR_CONN;
    client.stage = SSH_CONNECTING;
    return NTS_PENDING;
}

static uint8_t start_wolfssh(void)
{
    client.wolf_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (client.wolf_ctx == NULL) return NTS_ERR_CONN;
    wolfSSH_SetIORecv(client.wolf_ctx, io_recv);
    wolfSSH_SetIOSend(client.wolf_ctx, io_send);
    wolfSSH_SetUserAuth(client.wolf_ctx, auth_callback);
    wolfSSH_CTX_SetPublicKeyCheck(client.wolf_ctx, hostkey_callback);
    client.ssh = wolfSSH_new(client.wolf_ctx);
    if (client.ssh == NULL ||
        wolfSSH_SetUsername(client.ssh, client.username) != WS_SUCCESS)
        return NTS_ERR_CONN;
    if (!client.sftp_mode &&
        wolfSSH_SetChannelType(client.ssh, WOLFSSH_SESSION_TERMINAL,
                               NULL, 0) != WS_SUCCESS)
        return NTS_ERR_CONN;
    wolfSSH_SetIOReadCtx(client.ssh, &client);
    wolfSSH_SetIOWriteCtx(client.ssh, &client);
    wolfSSH_SetUserAuthCtx(client.ssh, &client);
    wolfSSH_SetPublicKeyCheckCtx(client.ssh, &client);
    return NTS_PENDING;
}

static uint8_t port_open_mode(void *opaque, const char *url,
                              const char *username, int trust_unknown,
                              char fingerprint[96], bool sftp_mode)
{
    int result;
    uint8_t status = NTS_PENDING;
    (void)opaque;
    if (client.stage != SSH_IDLE && client.sftp_mode != sftp_mode)
        return NTS_ERR_INUSE;
    switch (client.stage) {
    case SSH_IDLE: status = start_connection(url, username, trust_unknown,
                                              sftp_mode); break;
    case SSH_RESOLVING: status = start_tcp(); break;
    case SSH_CONNECTING: return NTS_PENDING;
    case SSH_HANDSHAKE:
        if (client.ssh == NULL) {
            status = start_wolfssh();
            if (status != NTS_PENDING) break;
        }
        result = client.sftp_mode ? wolfSSH_SFTP_connect(client.ssh) :
                                    wolfSSH_connect(client.ssh);
        if (result == WS_SUCCESS) {
            free_keys();
            clear_password();
            client.stage = client.sftp_mode ? SSH_UP : SSH_RESIZE;
            if (client.sftp_mode) {
                strcpy(client.sftp_cwd, ".");
                return NTS_OK;
            }
            return NTS_PENDING;
        }
        if (want_io(result)) return NTS_PENDING;
        strcpy(fingerprint, client.fingerprint);
        status = client.host_unknown ? NTS_HOSTKEY_UNKNOWN :
                 (client.host_changed ? NTS_ERR_PROTOCOL : NTS_AUTH_FAILED);
        break;
    case SSH_RESIZE:
        result = wolfSSH_ChangeTerminalSize(client.ssh, 40u, 24u, 0u, 0u);
        if (result == WS_SUCCESS) { client.stage = SSH_UP; return NTS_OK; }
        if (want_io(result)) return NTS_PENDING;
        status = NTS_ERR_PROTOCOL;
        break;
    case SSH_UP: return NTS_OK;
    case SSH_FAILED: status = NTS_ERR_CONN; break;
    }
    if (status != NTS_PENDING)
        close_connection(status == NTS_HOSTKEY_UNKNOWN);
    return status;
}

static uint8_t port_open(void *opaque, const char *url, const char *username,
                         int trust_unknown, char fingerprint[96])
{
    return port_open_mode(opaque, url, username, trust_unknown, fingerprint,
                          false);
}

static uint8_t port_sftp_open(void *opaque, const char *url,
                              const char *username, int trust_unknown,
                              char fingerprint[96])
{
    return port_open_mode(opaque, url, username, trust_unknown, fingerprint,
                          true);
}

static bool sftp_want_io(void)
{
    int error = client.ssh != NULL ? wolfSSH_get_error(client.ssh) : 0;
    return error == WS_WANT_READ || error == WS_WANT_WRITE ||
           error == WS_REKEYING;
}

static int sftp_fail_or_pending(void)
{
    return sftp_want_io() ? -(int)NTS_PENDING : -(int)NTS_ERR_PROTOCOL;
}

static int sftp_resolve_path(const char *path, char out[512])
{
    int count;
    if (path == NULL) return -1;
    if (*path == '/' || !strcmp(client.sftp_cwd, "."))
        count = snprintf(out, 512u, "%s", *path != '\0' ? path : ".");
    else
        count = snprintf(out, 512u, "%s/%s", client.sftp_cwd,
                         *path != '\0' ? path : ".");
    return count > 0 && count < 512 ? 0 : -1;
}

static int sftp_copy_name(WS_SFTPNAME *name, uint8_t *out, size_t maximum)
{
    size_t used = 0;
    for (WS_SFTPNAME *entry = name; entry != NULL; entry = entry->next) {
        const char *text = entry->lName != NULL ? entry->lName : entry->fName;
        size_t length = text != NULL ? strlen(text) : 0u;
        if (length + 1u > maximum - used) return -(int)NTS_ERR_PROTOCOL;
        memcpy(out + used, text, length);
        used += length;
        out[used++] = '\n';
    }
    return (int)used;
}

static int port_sftp_path(void *opaque, uint8_t operation, const char *path,
                          uint8_t *out, size_t maximum)
{
    char full[512];
    int result;
    WS_SFTPNAME *names;
    (void)opaque;
    if (client.stage != SSH_UP || !client.sftp_mode ||
        sftp_resolve_path(path, full) != 0)
        return -(int)NTS_ERR_CONN;
    switch (operation) {
    case NTS_SEC_SFTP_PWD: {
        size_t length = strlen(client.sftp_cwd);
        if (length + 1u > maximum) return -(int)NTS_ERR_PARAM;
        memcpy(out, client.sftp_cwd, length);
        out[length++] = '\n';
        return (int)length;
    }
    case NTS_SEC_SFTP_CD:
        names = wolfSSH_SFTP_RealPath(client.ssh, full);
        if (names == NULL) return sftp_fail_or_pending();
        if (names->fName == NULL || strlen(names->fName) >= sizeof(client.sftp_cwd)) {
            wolfSSH_SFTPNAME_list_free(names);
            return -(int)NTS_ERR_PROTOCOL;
        }
        strcpy(client.sftp_cwd, names->fName);
        wolfSSH_SFTPNAME_list_free(names);
        return 0;
    case NTS_SEC_SFTP_LS:
        names = wolfSSH_SFTP_LS(client.ssh, full);
        if (names == NULL) {
            int error = wolfSSH_get_error(client.ssh);
            if (sftp_want_io()) return -(int)NTS_PENDING;
            return error == WS_SUCCESS || error == WOLFSSH_FTP_EOF ? 0 :
                   -(int)NTS_ERR_PROTOCOL;
        }
        result = sftp_copy_name(names, out, maximum);
        wolfSSH_SFTPNAME_list_free(names);
        return result;
    case NTS_SEC_SFTP_DELETE:
        result = wolfSSH_SFTP_Remove(client.ssh, full);
        break;
    case NTS_SEC_SFTP_MKDIR:
        result = wolfSSH_SFTP_MKDIR(client.ssh, full, NULL);
        break;
    case NTS_SEC_SFTP_RMDIR:
        result = wolfSSH_SFTP_RMDIR(client.ssh, full);
        break;
    default:
        return -(int)NTS_ERR_PARAM;
    }
    if (result == WS_SUCCESS) return 0;
    return sftp_fail_or_pending();
}

static int port_sftp_transfer_open(const char *path, bool upload)
{
    char full[512];
    word32 flags = upload ? (WOLFSSH_FXF_WRITE | WOLFSSH_FXF_CREAT |
                             WOLFSSH_FXF_TRUNC) : WOLFSSH_FXF_READ;
    int result;
    if (client.stage != SSH_UP || !client.sftp_mode ||
        client.sftp_transfer != 0u || sftp_resolve_path(path, full) != 0)
        return -(int)NTS_ERR_CONN;
    client.sftp_handle_size = sizeof(client.sftp_handle);
    result = wolfSSH_SFTP_Open(client.ssh, full, flags, NULL,
                               client.sftp_handle,
                               &client.sftp_handle_size);
    if (result != WS_SUCCESS) return sftp_fail_or_pending();
    client.sftp_offset[0] = client.sftp_offset[1] = 0u;
    client.sftp_transfer = upload ? 2u : 1u;
    return 0;
}

static int port_sftp_get_open(void *opaque, const char *path)
{
    (void)opaque;
    return port_sftp_transfer_open(path, false);
}

static int port_sftp_put_open(void *opaque, const char *path)
{
    (void)opaque;
    return port_sftp_transfer_open(path, true);
}

static void sftp_advance(word32 amount)
{
    word32 old = client.sftp_offset[0];
    client.sftp_offset[0] += amount;
    if (client.sftp_offset[0] < old) client.sftp_offset[1]++;
}

static int port_sftp_get_read(void *opaque, uint8_t *out, size_t maximum)
{
    int result;
    (void)opaque;
    if (client.sftp_transfer != 1u || maximum > UINT32_MAX)
        return -(int)NTS_ERR_CONN;
    result = wolfSSH_SFTP_SendReadPacket(client.ssh, client.sftp_handle,
                                         client.sftp_handle_size,
                                         client.sftp_offset, out,
                                         (word32)maximum);
    if (result >= 0) {
        sftp_advance((word32)result);
        return result;
    }
    return sftp_fail_or_pending();
}

static int port_sftp_put_write(void *opaque, const uint8_t *data, size_t length)
{
    int result;
    (void)opaque;
    if (client.sftp_transfer != 2u || length == 0u || length > UINT32_MAX)
        return -(int)NTS_ERR_PARAM;
    result = wolfSSH_SFTP_SendWritePacket(client.ssh, client.sftp_handle,
                                          client.sftp_handle_size,
                                          client.sftp_offset,
                                          (byte *)(uintptr_t)data,
                                          (word32)length);
    if (result >= 0) {
        if ((size_t)result != length) return -(int)NTS_ERR_PROTOCOL;
        sftp_advance((word32)result);
        return result;
    }
    return sftp_fail_or_pending();
}

static int port_sftp_transfer_close(void *opaque)
{
    int result;
    (void)opaque;
    if (client.sftp_transfer == 0u) return 0;
    result = wolfSSH_SFTP_Close(client.ssh, client.sftp_handle,
                                client.sftp_handle_size);
    if (result != WS_SUCCESS) return sftp_fail_or_pending();
    client.sftp_transfer = 0u;
    client.sftp_handle_size = 0u;
    return 0;
}

static void port_sftp_close(void *opaque) { (void)opaque; close_client(); }

static int port_read(void *opaque, uint8_t *out, size_t maximum)
{
    int result;
    (void)opaque;
    if (client.stage != SSH_UP || maximum > UINT32_MAX) return -(int)NTS_ERR_CONN;
    result = wolfSSH_stream_read(client.ssh, out, (word32)maximum);
    if (result >= 0) return result;
    /* A normal remote shell exit closes the SSH channel before the peer must
       close its TCP connection. Report that as EOF, not protocol error &2B. */
    if (channel_finished(result)) return -(int)NTS_EOF;
    if (want_io(result)) return 0;
    return client.eof && client.rx_count == 0u ? -(int)NTS_EOF :
                                                 -(int)NTS_ERR_PROTOCOL;
}

static int port_write(void *opaque, const uint8_t *data, size_t length)
{
    int result;
    (void)opaque;
    if (client.stage != SSH_UP || length > UINT32_MAX) return -(int)NTS_ERR_CONN;
    result = wolfSSH_stream_send(client.ssh, (byte *)(uintptr_t)data,
                                 (word32)length);
    if (result >= 0) return result;
    return want_io(result) ? 0 : -(int)NTS_ERR_PROTOCOL;
}

static int port_password(void *opaque, const uint8_t *password, size_t length)
{
    (void)opaque;
    if (password == NULL || client.stage != SSH_IDLE || length == 0u ||
        length > sizeof(client.password)) return -1;
    clear_password();
    memcpy(client.password, password, length);
    client.password_size = (word32)length;
    return 0;
}

static void port_close(void *opaque) { (void)opaque; close_client(); }

static const nts_secure_port port = {
    port_random, port_open, port_read, port_write, port_password, port_close,
    port_sftp_open, port_sftp_path, port_sftp_get_open, port_sftp_get_read,
    port_sftp_put_open, port_sftp_put_write, port_sftp_transfer_close,
    port_sftp_close
};

const nts_secure_port *nts_pi_wolfssh_port(void) { return &port; }
void *nts_pi_wolfssh_context(void) { return &client; }
int nts_pi_wolfssh_ready(void) { return provider_ready; }
int nts_pi_wolfssh_random_ready(void) { return rng_ready; }
void nts_pi_wolfssh_poll(void)
{
    wifi_lwip_rx_kick();
    rng_poll();
    if (rng_ready && !wolfssh_init_attempted) {
        wolfssh_init_attempted = true;
        provider_ready = wolfSSH_Init() == WS_SUCCESS;
        wolfssh_started = provider_ready;
    }
}

void nts_pi_wolfssh_reset(void)
{
    close_client();
    if (wolfssh_started) {
        (void)wolfSSH_Cleanup();
        wolfssh_started = false;
    }
    wolfssh_init_attempted = false;
    (void)f_mkdir("Pi1MHz");
    (void)f_mkdir(SSH_DIR);
    provider_ready = false;
    rng_begin();
}
