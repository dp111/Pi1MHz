/* net_tnfs.c - TNFS wire codec.  See net_tnfs.h. */

#include "net_tnfs.h"
#include <string.h>

/* ---- little-endian buffer writers (bounds-checked) ----------------------- */

static size_t put_hdr(uint8_t *b, size_t cap, uint16_t connid,
                      uint8_t seq, uint8_t cmd)
{
   if (cap < TNFS_HDR_LEN) return 0u;
   b[0] = (uint8_t)connid;
   b[1] = (uint8_t)(connid >> 8);
   b[2] = seq;
   b[3] = cmd;
   return TNFS_HDR_LEN;                 /* the next write position */
}

static bool put_u8(uint8_t *b, size_t cap, size_t *pos, uint8_t v)
{
   if (*pos + 1u > cap) return false;
   b[(*pos)++] = v;
   return true;
}

static bool put_u16(uint8_t *b, size_t cap, size_t *pos, uint16_t v)
{
   if (*pos + 2u > cap) return false;
   b[*pos]     = (uint8_t)v;
   b[*pos + 1] = (uint8_t)(v >> 8);
   *pos += 2u;
   return true;
}

/* Append a NUL-terminated string (NULL -> a lone NUL, TNFS's "field absent"). */
static bool put_str(uint8_t *b, size_t cap, size_t *pos, const char *s)
{
   size_t body = s ? strlen(s) : 0u;
   if (*pos + body + 1u > cap) return false;
   if (body != 0u) memcpy(b + *pos, s, body);
   b[*pos + body] = 0u;
   *pos += body + 1u;
   return true;
}

/* ---- request builders ---------------------------------------------------- */

size_t tnfs_build_mount(uint8_t *buf, size_t cap, uint8_t seq,
                        const char *mountpoint, const char *user,
                        const char *pass)
{
   size_t pos = put_hdr(buf, cap, 0u, seq, TNFS_CMD_MOUNT);   /* connid 0 on MOUNT */
   if (pos == 0u) return 0u;
   if (!put_u16(buf, cap, &pos, TNFS_VERSION))               return 0u;
   if (!put_str(buf, cap, &pos, mountpoint ? mountpoint : "/")) return 0u;
   if (!put_str(buf, cap, &pos, user))                       return 0u;
   if (!put_str(buf, cap, &pos, pass))                       return 0u;
   return pos;
}

size_t tnfs_build_umount(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq)
{
   return put_hdr(buf, cap, connid, seq, TNFS_CMD_UMOUNT);   /* no body */
}

size_t tnfs_build_open(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                       uint16_t flags, uint16_t mode, const char *path)
{
   size_t pos = put_hdr(buf, cap, connid, seq, TNFS_CMD_OPEN);
   if (pos == 0u) return 0u;
   if (!put_u16(buf, cap, &pos, flags)) return 0u;
   if (!put_u16(buf, cap, &pos, mode))  return 0u;
   if (!put_str(buf, cap, &pos, path))  return 0u;
   return pos;
}

size_t tnfs_build_read(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                       uint8_t fd, uint16_t size)
{
   size_t pos = put_hdr(buf, cap, connid, seq, TNFS_CMD_READ);
   if (pos == 0u) return 0u;
   if (!put_u8 (buf, cap, &pos, fd))   return 0u;
   if (!put_u16(buf, cap, &pos, size)) return 0u;
   return pos;
}

size_t tnfs_build_write(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                        uint8_t fd, const uint8_t *data, uint16_t len)
{
   size_t pos = put_hdr(buf, cap, connid, seq, TNFS_CMD_WRITE);
   if (pos == 0u) return 0u;
   if (!put_u8 (buf, cap, &pos, fd))  return 0u;
   if (!put_u16(buf, cap, &pos, len)) return 0u;
   if ((size_t)len != 0u) {
      if (pos + len > cap) return 0u;
      memcpy(buf + pos, data, len);
      pos += len;
   }
   return pos;
}

size_t tnfs_build_close(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                        uint8_t fd)
{
   size_t pos = put_hdr(buf, cap, connid, seq, TNFS_CMD_CLOSE);
   if (pos == 0u) return 0u;
   if (!put_u8(buf, cap, &pos, fd)) return 0u;
   return pos;
}

size_t tnfs_build_opendir(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                          const char *path)
{
   size_t pos = put_hdr(buf, cap, connid, seq, TNFS_CMD_OPENDIR);
   if (pos == 0u) return 0u;
   if (!put_str(buf, cap, &pos, path)) return 0u;
   return pos;
}

size_t tnfs_build_readdir(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                          uint8_t dirhandle)
{
   size_t pos = put_hdr(buf, cap, connid, seq, TNFS_CMD_READDIR);
   if (pos == 0u) return 0u;
   if (!put_u8(buf, cap, &pos, dirhandle)) return 0u;
   return pos;
}

size_t tnfs_build_closedir(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                           uint8_t dirhandle)
{
   size_t pos = put_hdr(buf, cap, connid, seq, TNFS_CMD_CLOSEDIR);
   if (pos == 0u) return 0u;
   if (!put_u8(buf, cap, &pos, dirhandle)) return 0u;
   return pos;
}

size_t tnfs_build_stat(uint8_t *buf, size_t cap, uint16_t connid, uint8_t seq,
                       const char *path)
{
   size_t pos = put_hdr(buf, cap, connid, seq, TNFS_CMD_STAT);
   if (pos == 0u) return 0u;
   if (!put_str(buf, cap, &pos, path)) return 0u;
   return pos;
}

/* ---- reply parsing ------------------------------------------------------- */

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool tnfs_parse_reply(const uint8_t *pkt, size_t len,
                      uint8_t want_seq, uint8_t want_cmd, tnfs_reply_t *out)
{
   memset(out, 0, sizeof *out);
   if (len < TNFS_HDR_LEN + 1u) return false;      /* need header + status byte */
   if (pkt[2] != want_seq || pkt[3] != want_cmd) return false;
   out->connid   = rd_u16(pkt);
   out->seq      = pkt[2];
   out->cmd      = pkt[3];
   out->status   = pkt[4];
   out->body     = pkt + 5;
   out->body_len = len - 5u;
   if (out->status == TNFS_EAGAIN && out->body_len >= 2u)
      out->backoff_ms = rd_u16(out->body);
   out->ok = true;
   return true;
}

bool tnfs_reply_mount(const tnfs_reply_t *r, uint16_t *server_ver, uint16_t *retry_ms)
{
   if (!r->ok || r->status != TNFS_OK || r->body_len < 4u) return false;
   if (server_ver) *server_ver = rd_u16(r->body);
   if (retry_ms)   *retry_ms   = rd_u16(r->body + 2);
   return true;
}

bool tnfs_reply_open(const tnfs_reply_t *r, uint8_t *fd)
{
   if (!r->ok || r->status != TNFS_OK || r->body_len < 1u) return false;
   if (fd) *fd = r->body[0];
   return true;
}

bool tnfs_reply_write(const tnfs_reply_t *r, uint16_t *written)
{
   if (!r->ok || r->status != TNFS_OK || r->body_len < 2u) return false;
   if (written) *written = rd_u16(r->body);
   return true;
}

bool tnfs_reply_opendir(const tnfs_reply_t *r, uint8_t *dirhandle)
{
   if (!r->ok || r->status != TNFS_OK || r->body_len < 1u) return false;
   if (dirhandle) *dirhandle = r->body[0];
   return true;
}

bool tnfs_reply_read(const tnfs_reply_t *r, const uint8_t **data, uint16_t *data_len)
{
   uint16_t n;
   if (!r->ok || r->status != TNFS_OK || r->body_len < 2u) return false;
   n = rd_u16(r->body);
   if ((size_t)n + 2u > r->body_len) return false;    /* truncated datagram */
   if (data)     *data     = r->body + 2;
   if (data_len) *data_len = n;
   return true;
}

bool tnfs_reply_readdir(const tnfs_reply_t *r, const char **name)
{
   if (!r->ok || r->status != TNFS_OK || r->body_len < 1u) return false;
   if (memchr(r->body, 0, r->body_len) == NULL) return false;   /* must be terminated */
   if (name) *name = (const char *)r->body;
   return true;
}

bool tnfs_reply_stat_size(const tnfs_reply_t *r, uint32_t *size)
{
   /* body: mode(2) uid(2) gid(2) size(4) ... -> size starts at offset 6 */
   if (!r->ok || r->status != TNFS_OK || r->body_len < 10u) return false;
   if (size)
      *size = (uint32_t)r->body[6] | ((uint32_t)r->body[7] << 8)
            | ((uint32_t)r->body[8] << 16) | ((uint32_t)r->body[9] << 24);
   return true;
}
