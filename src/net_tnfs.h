/* net_tnfs.h - TNFS (Trivial Network File System) wire codec.
 *
 * Pure request-builder / reply-parser functions for the TNFS UDP protocol
 * (spec: github.com/spectrumero/tnfsd/blob/master/tnfs-protocol.md), used by
 * the net service's `N:TNFS://` adapter.  No I/O, no lwIP, no net_service
 * state - just byte buffers, so it is portable and host-testable in isolation.
 *
 * Every datagram starts with a 4-byte header {connid_lo, connid_hi, seq, cmd};
 * the reply echoes seq and (for MOUNT) fills in the session connid.  Build
 * functions write a full request into the caller's buffer and return its
 * length (0 = would overflow / bad argument).  Parse functions validate a
 * reply against the expected (connid, seq, cmd) and extract the body fields.
 */
#ifndef NET_TNFS_H
#define NET_TNFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TNFS_PORT            16384u
#define TNFS_HDR_LEN         4u
#define TNFS_VERSION         0x0102u   /* protocol 1.2: (major<<8)|minor, LE on the wire */

/* Commands */
#define TNFS_CMD_MOUNT       0x00u
#define TNFS_CMD_UMOUNT      0x01u
#define TNFS_CMD_OPENDIR     0x10u
#define TNFS_CMD_READDIR     0x11u
#define TNFS_CMD_CLOSEDIR    0x12u
#define TNFS_CMD_READ        0x21u
#define TNFS_CMD_WRITE       0x22u
#define TNFS_CMD_CLOSE       0x23u
#define TNFS_CMD_STAT        0x24u
#define TNFS_CMD_LSEEK       0x25u
#define TNFS_CMD_OPEN        0x29u

/* Status / error byte (0 = OK; else a POSIX errno, with these specials) */
#define TNFS_OK              0x00u
#define TNFS_EAGAIN          0x07u   /* busy: reply body is a 16-bit LE backoff-ms */
#define TNFS_EOF             0x21u   /* end of file / end of directory */

/* OPEN flags (16-bit LE) */
#define TNFS_O_RDONLY        0x0001u
#define TNFS_O_WRONLY        0x0002u
#define TNFS_O_RDWR          0x0003u
#define TNFS_O_APPEND        0x0008u
#define TNFS_O_CREAT         0x0100u
#define TNFS_O_TRUNC         0x0200u
#define TNFS_O_EXCL          0x0400u

/* ---- request builders: return the datagram length, or 0 on overflow ------ */

size_t tnfs_build_mount   (uint8_t *buf, size_t cap, uint8_t seq,
                           const char *mountpoint, const char *user,
                           const char *pass);
size_t tnfs_build_umount  (uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq);
size_t tnfs_build_open    (uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                           uint16_t flags, uint16_t mode, const char *path);
size_t tnfs_build_read    (uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                           uint8_t fd, uint16_t size);
size_t tnfs_build_close   (uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                           uint8_t fd);
size_t tnfs_build_opendir (uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                           const char *path);
size_t tnfs_build_readdir (uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                           uint8_t dirhandle);
size_t tnfs_build_closedir(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                           uint8_t dirhandle);
size_t tnfs_build_stat    (uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                           const char *path);

/* ---- reply parsing ------------------------------------------------------- */

/* A parsed reply header + status.  `status` is valid only when `ok` (a
 * well-formed reply of at least header+status bytes that matches the expected
 * seq/cmd).  For EAGAIN, `backoff_ms` holds the requested retry delay. */
typedef struct {
   bool     ok;            /* header well-formed and seq/cmd matched  */
   uint16_t connid;        /* session id echoed/assigned in the header */
   uint8_t  seq;
   uint8_t  cmd;
   uint8_t  status;        /* TNFS_OK / TNFS_EOF / errno               */
   uint16_t backoff_ms;    /* valid when status == TNFS_EAGAIN         */
   const uint8_t *body;    /* payload after the status byte            */
   size_t   body_len;
} tnfs_reply_t;

/* Parse a datagram as a reply to (want_seq, want_cmd).  Returns false and
 * leaves out->ok=false if the packet is too short or seq/cmd don't match. */
bool tnfs_parse_reply(const uint8_t *pkt, size_t len,
                      uint8_t want_seq, uint8_t want_cmd, tnfs_reply_t *out);

/* Typed extractors over a parsed, status==OK reply (return false on short body):
 * MOUNT: server version + minimum retry-timeout ms; the session id is out->connid. */
bool tnfs_reply_mount (const tnfs_reply_t *r, uint16_t *server_ver, uint16_t *retry_ms);
bool tnfs_reply_open  (const tnfs_reply_t *r, uint8_t *fd);
bool tnfs_reply_opendir(const tnfs_reply_t *r, uint8_t *dirhandle);
/* READ: point `data`/`data_len` at the file bytes; caller copies them out. */
bool tnfs_reply_read  (const tnfs_reply_t *r, const uint8_t **data, uint16_t *data_len);
/* READDIR: the null-terminated entry name (points into the packet). */
bool tnfs_reply_readdir(const tnfs_reply_t *r, const char **name);
/* STAT: file size in bytes (the field most callers want). */
bool tnfs_reply_stat_size(const tnfs_reply_t *r, uint32_t *size);

#endif /* NET_TNFS_H */
