/* fn_disk.c - DiskService ($FC): 8 slots of mounted disk images.

   Protocol: fujinet-nio docs/disk_device_protocol.md; behaviour from
   src/lib/disk_device.cpp and src/lib/disk/ssd_image.cpp, which this
   follows command by command:

   - slots are 1-based on the wire; a bad slot is InvalidRequest, an empty
     one NotReady;
   - an SSD's geometry is its DFS catalogue's sector count (400 or 800), not
     its file size: a short (sparse) image reads zeros past its end and a
     write past the end zero-fills the gap;
   - a URI may be relative to the current host (fn_host_resolve);
   - Mount with flags bit 1 records a lazy mount, opened on first use.

   Image types (fujinet-nio ImageType): 0 auto, 1 ATR, 2 SSD, 3 DSD, 4 raw.
   SSD and raw are implemented; ATR and DSD are Unsupported, as DSD is in
   fujinet-nio too. */

#include <stdio.h>
#include <string.h>

#include "fn_devices.h"
#include "fn_store.h"
#include "fn_disk.h"

#define SLOTS        8u
#define URI_MAX      FN_STORE_MAX_PATH
#define SECTOR_MAX   512u

#define DISK_MOUNT         0x01u
#define DISK_UNMOUNT       0x02u
#define DISK_READ_SECTOR   0x03u
#define DISK_WRITE_SECTOR  0x04u
#define DISK_INFO          0x05u
#define DISK_CLEAR_CHANGED 0x06u
#define DISK_CREATE        0x07u
#define DISK_RESTORE_BOOT  0x0Au
#define DISK_BEGIN_SESSION 0x0Bu
#define DISK_REINITIALIZE  0x0Cu
#define DISK_LIST_MOUNTS   0x0Du

#define TYPE_AUTO 0u
#define TYPE_ATR  1u
#define TYPE_SSD  2u
#define TYPE_DSD  3u
#define TYPE_RAW  4u

typedef struct {
   bool     used;          /* inserted, or pending */
   bool     pending;       /* lazy: recorded, not yet opened */
   bool     ro_requested;
   bool     ro;            /* effective */
   bool     changed;
   bool     dirty;
   uint8_t  type;
   uint8_t  type_override;
   uint16_t hint;
   uint16_t sector_size;
   uint32_t sector_count;
   uint32_t file_size;
   fn_handle h;
   char     uri[URI_MAX];
} slot_t;

static slot_t s_slot[SLOTS];
static char s_boot_uri[URI_MAX];
static bool s_boot_ro = true;

void fn_disk_set_boot(const char *uri, bool read_only)
{
   snprintf(s_boot_uri, sizeof s_boot_uri, "%s", uri ? uri : "");
   s_boot_ro = read_only;
}

static void close_slot(slot_t *s)
{
   if (s->h != FN_NO_HANDLE) {
      (void)fn_store_sync(s->h);
      fn_store_close(s->h);
   }
   memset(s, 0, sizeof *s);
   s->h = FN_NO_HANDLE;
   s->changed = true;
}

static void ensure_init(void);

void fn_disk_reset(void)
{
   ensure_init();
   for (unsigned int i = 0; i < SLOTS; i++) {
      if (s_slot[i].used)
         close_slot(&s_slot[i]);
      else
         s_slot[i].h = FN_NO_HANDLE;
   }
}

static bool ends_with_ci(const char *s, const char *ext)
{
   size_t n = strlen(s), e = strlen(ext);
   if (n < e) return false;
   for (size_t i = 0; i < e; i++) {
      char c = s[n - e + i];
      if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
      if (c != ext[i]) return false;
   }
   return true;
}

/* Open the image named by s->uri and establish its geometry.  Returns a
   status. */
static uint8_t open_image(slot_t *s)
{
   char fs[16], path[URI_MAX];
   if (!fn_uri_split(s->uri, fs, sizeof fs, path, sizeof path))
      return FB_INVALID_REQUEST;

   uint8_t type = s->type_override;
   if (type == TYPE_AUTO)
      type = ends_with_ci(path, ".ssd") ? TYPE_SSD
           : ends_with_ci(path, ".dsd") ? TYPE_DSD
           : ends_with_ci(path, ".atr") ? TYPE_ATR : TYPE_RAW;
   if (type != TYPE_SSD && type != TYPE_RAW)
      return FB_UNSUPPORTED;

   bool ro = s->ro_requested;
   fn_handle h = ro ? FN_NO_HANDLE : fn_store_open(fs, path, FN_OPEN_UPDATE);
   if (h == FN_NO_HANDLE) {
      h = fn_store_open(fs, path, FN_OPEN_READ);
      ro = true;
   }
   if (h == FN_NO_HANDLE)
      return FB_INVALID_REQUEST;           /* FileNotFound */

   uint32_t size = 0;
   if (!fn_store_size(h, &size)) {
      fn_store_close(h);
      return FB_IO_ERROR;
   }
   uint16_t ssize;
   uint32_t count;
   if (type == TYPE_SSD) {
      uint8_t cat[0x108];
      if (size < sizeof cat || !fn_store_read(h, 0, cat, sizeof cat)) {
         fn_store_close(h);
         return size < sizeof cat ? FB_INVALID_REQUEST : FB_IO_ERROR;
      }
      ssize = 256;
      count = ((uint32_t)(cat[0x106] & 3u) << 8) | cat[0x107];
      if (count != 400u && count != 800u) {
         fn_store_close(h);
         return FB_INVALID_REQUEST;        /* BadImage */
      }
   } else {
      ssize = s->hint ? s->hint : ends_with_ci(path, ".adf") ? 512u : 256u;
      if (ssize > SECTOR_MAX || size % ssize) {
         fn_store_close(h);
         return FB_INVALID_REQUEST;        /* InvalidGeometry */
      }
      count = size / ssize;
   }
   s->h = h;
   s->ro = ro;
   s->type = type;
   s->sector_size = ssize;
   s->sector_count = count;
   s->file_size = size;
   s->pending = false;
   s->used = true;
   s->changed = true;
   return FB_OK;
}

static uint8_t slot_of(uint8_t slot1, slot_t **out)
{
   if (slot1 == 0 || slot1 > SLOTS)
      return FB_INVALID_REQUEST;
   *out = &s_slot[slot1 - 1u];
   return FB_OK;
}

/* A slot ready for sector I/O: opens a lazy mount on first use. */
static uint8_t active(slot_t *s)
{
   if (!s->used)
      return FB_NOT_READY;
   if (s->pending) {
      uint8_t st = open_image(s);
      if (st != FB_OK) {
         /* The media cannot be used: leave the unit as fujinet-nio does,
            recorded but failing. */
         return st;
      }
   }
   return FB_OK;
}

static void put_mounted(fb_out *out, uint8_t slot1, const slot_t *s)
{
   fb_put_u8(out, FB_VERSION);
   fb_put_u8(out, (uint8_t)(0x01u | (s->ro ? 0x02u : 0u)));
   fb_put_u16(out, 0);
   fb_put_u8(out, slot1);
   fb_put_u8(out, s->type);
   fb_put_u16(out, s->sector_size);
   fb_put_u32(out, s->sector_count);
}

static uint8_t read_uri(fb_in *in, char *uri, size_t cap)
{
   uint16_t n = fb_get_u16(in);
   const uint8_t *p = fb_get_bytes(in, n);
   char spec[URI_MAX];
   if (in->bad || n >= sizeof spec)
      return FB_INVALID_REQUEST;
   memcpy(spec, p, n);
   spec[n] = '\0';
   return fn_host_resolve(spec, uri, cap) ? FB_OK : FB_INVALID_REQUEST;
}

static uint8_t mount(slot_t *s, uint8_t slot1, const char *uri, bool ro,
                     uint8_t type, uint16_t hint, bool lazy, fb_out *out)
{
   if (s->used)
      close_slot(s);
   snprintf(s->uri, sizeof s->uri, "%s", uri);
   s->ro_requested = ro;
   s->type_override = type;
   s->hint = hint;
   if (lazy) {
      char fs[16], path[URI_MAX];
      if (!fn_uri_split(uri, fs, sizeof fs, path, sizeof path)) {
         close_slot(s);
         return FB_INVALID_REQUEST;
      }
      s->used = true;
      s->pending = true;
      s->ro = ro;
      static const uint8_t zeros[7] = { 0 };
      fb_put_u8(out, FB_VERSION);
      fb_put_bytes(out, zeros, 3);
      fb_put_u8(out, slot1);
      fb_put_bytes(out, zeros, 7);
      return FB_OK;
   }
   uint8_t st = open_image(s);
   if (st != FB_OK) {
      close_slot(s);
      return st;
   }
   put_mounted(out, slot1, s);
   return FB_OK;
}

/* Write a blank image: SSD gets fujinet-nio's minimal DFS catalogue. */
static uint8_t create_image(const char *uri, uint8_t type, uint16_t ssize,
                            uint32_t count, bool overwrite)
{
   char fs[16], path[URI_MAX];
   if (!fn_uri_split(uri, fs, sizeof fs, path, sizeof path))
      return FB_INVALID_REQUEST;
   if (type == TYPE_SSD) {
      if (ssize != 256u || (count != 400u && count != 800u))
         return FB_INVALID_REQUEST;
   } else if (type == TYPE_RAW) {
      if (ssize == 0 || ssize > SECTOR_MAX || count == 0 || count > 0x1000000u / ssize)
         return FB_INVALID_REQUEST;
   } else {
      return type == TYPE_AUTO ? FB_INVALID_REQUEST : FB_UNSUPPORTED;
   }
   fn_handle h = fn_store_open(fs, path, overwrite ? FN_OPEN_CREATE : FN_OPEN_CREATE_NEW);
   if (h == FN_NO_HANDLE)
      return FB_INVALID_REQUEST;          /* AlreadyExists, or no such directory */
   bool ok = true;
   uint8_t sec[256];
   if (type == TYPE_SSD) {
      memset(sec, 0, sizeof sec);
      memcpy(sec, "BLANK", 5);
      ok = fn_store_write(h, 0, sec, 256);
      memset(sec, 0, sizeof sec);
      sec[6] = (uint8_t)((count >> 8) & 3u);
      sec[7] = (uint8_t)count;
      ok = ok && fn_store_write(h, 256, sec, 256);
   }
   /* Extend to full size with one byte at the end. */
   uint8_t z = 0;
   ok = ok && fn_store_write(h, (uint32_t)ssize * count - 1u, &z, 1);
   ok = fn_store_sync(h) && ok;
   fn_store_close(h);
   return ok ? FB_OK : FB_IO_ERROR;
}

static void ensure_init(void)
{
   static bool done;
   if (done)
      return;
   for (unsigned int i = 0; i < SLOTS; i++)
      s_slot[i].h = FN_NO_HANDLE;
   done = true;
}

uint8_t fn_disk_command(uint8_t command, fb_in *in, fb_out *out)
{
   ensure_init();
   if (fb_get_u8(in) != FB_VERSION)
      return FB_INVALID_REQUEST;

   slot_t *s = NULL;
   uint8_t st;

   switch (command) {
   case DISK_MOUNT: {
      uint8_t slot1 = fb_get_u8(in), flags = fb_get_u8(in), type = fb_get_u8(in);
      uint16_t hint = fb_get_u16(in);
      char uri[URI_MAX];
      if ((st = slot_of(slot1, &s)) != FB_OK || in->bad ||
          (st = read_uri(in, uri, sizeof uri)) != FB_OK)
         return FB_INVALID_REQUEST;
      return mount(s, slot1, uri, flags & 1u, type, hint, flags & 2u, out);
   }
   case DISK_UNMOUNT: {
      uint8_t slot1 = fb_get_u8(in);
      if (in->bad || (st = slot_of(slot1, &s)) != FB_OK)
         return FB_INVALID_REQUEST;
      if (s->used)
         close_slot(s);
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, 0);
      fb_put_u16(out, 0);
      fb_put_u8(out, slot1);
      return FB_OK;
   }
   case DISK_READ_SECTOR:
   case DISK_WRITE_SECTOR: {
      uint8_t slot1 = fb_get_u8(in);
      uint32_t lba = fb_get_u32(in);
      uint16_t n = fb_get_u16(in);                    /* maxBytes / dataLen */
      const uint8_t *data = command == DISK_WRITE_SECTOR ? fb_get_bytes(in, n) : NULL;
      if (in->bad || (st = slot_of(slot1, &s)) != FB_OK)
         return FB_INVALID_REQUEST;
      if ((st = active(s)) != FB_OK)
         return st;
      if (lba >= s->sector_count)
         return FB_INVALID_REQUEST;                    /* OutOfRange */
      uint32_t off = lba * s->sector_size;
      if (command == DISK_READ_SECTOR) {
         uint8_t sec[SECTOR_MAX];
         memset(sec, 0, s->sector_size);
         uint32_t in_file = off >= s->file_size ? 0 : s->file_size - off;
         if (in_file > s->sector_size) in_file = s->sector_size;
         if (in_file && !fn_store_read(s->h, off, sec, in_file))
            return FB_IO_ERROR;
         uint16_t len = n < s->sector_size ? n : s->sector_size;
         fb_put_u8(out, FB_VERSION);
         fb_put_u8(out, len < s->sector_size ? 1u : 0u);
         fb_put_u16(out, 0);
         fb_put_u8(out, slot1);
         fb_put_u32(out, lba);
         fb_put_u16(out, len);
         fb_put_bytes(out, sec, len);
         return FB_OK;
      }
      if (s->ro || n < s->sector_size)
         return FB_INVALID_REQUEST;                    /* ReadOnly / too short */
      if (off > s->file_size) {
         static const uint8_t zeros[256] = { 0 };
         for (uint32_t pos = s->file_size; pos < off; ) {
            uint32_t chunk = off - pos < sizeof zeros ? off - pos : sizeof zeros;
            if (!fn_store_write(s->h, pos, zeros, chunk))
               return FB_IO_ERROR;
            pos += chunk;
         }
      }
      if (!fn_store_write(s->h, off, data, s->sector_size) || !fn_store_sync(s->h))
         return FB_IO_ERROR;
      if (off + s->sector_size > s->file_size)
         s->file_size = off + s->sector_size;
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, 0);
      fb_put_u16(out, 0);
      fb_put_u8(out, slot1);
      fb_put_u32(out, lba);
      fb_put_u16(out, s->sector_size);
      return FB_OK;
   }
   case DISK_INFO: {
      uint8_t slot1 = fb_get_u8(in);
      if (in->bad || (st = slot_of(slot1, &s)) != FB_OK)
         return FB_INVALID_REQUEST;
      /* As fujinet-nio's handler (not its stale doc vector): a pending mount
         is opened first and its failure is the reply; "has last error"
         (bit 5) is always set; dirty (bit 2) never is, since every write is
         synced. */
      if (s->used && s->pending && (st = active(s)) != FB_OK)
         return st;
      bool opened = s->used;
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, (uint8_t)((opened ? 0x01u : 0u) | (s->ro ? 0x02u : 0u) |
                               (s->changed ? 0x08u : 0u) | (opened ? 0x10u : 0u) | 0x20u));
      fb_put_u16(out, 0);
      fb_put_u8(out, slot1);
      fb_put_u8(out, opened ? s->type : 0u);
      fb_put_u16(out, opened ? s->sector_size : 0u);
      fb_put_u32(out, opened ? s->sector_count : 0u);
      fb_put_u8(out, 0);
      return FB_OK;
   }
   case DISK_CLEAR_CHANGED: {
      uint8_t slot1 = fb_get_u8(in);
      if (in->bad || (st = slot_of(slot1, &s)) != FB_OK)
         return FB_INVALID_REQUEST;
      s->changed = false;
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, 0);
      fb_put_u16(out, 0);
      fb_put_u8(out, slot1);
      return FB_OK;
   }
   case DISK_CREATE: {
      uint8_t flags = fb_get_u8(in), type = fb_get_u8(in);
      uint16_t ssize = fb_get_u16(in);
      uint32_t count = fb_get_u32(in);
      char uri[URI_MAX];
      if (in->bad || read_uri(in, uri, sizeof uri) != FB_OK)
         return FB_INVALID_REQUEST;
      if ((st = create_image(uri, type, ssize, count, flags & 1u)) != FB_OK)
         return st;
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, 0);
      fb_put_u16(out, 0);
      fb_put_u8(out, type);
      fb_put_u16(out, ssize);
      fb_put_u32(out, count);
      return FB_OK;
   }
   case DISK_RESTORE_BOOT:
   case DISK_BEGIN_SESSION: {
      uint8_t slot1 = fb_get_u8(in);
      if (in->bad || (st = slot_of(slot1, &s)) != FB_OK)
         return FB_INVALID_REQUEST;
      if (command == DISK_BEGIN_SESSION)
         fn_disk_reset();
      if (!s_boot_uri[0]) {
         if (command == DISK_RESTORE_BOOT)
            return FB_NOT_READY;
         fb_put_u8(out, FB_VERSION);
         fb_put_u8(out, 0);
         fb_put_u16(out, 0);
         fb_put_u8(out, slot1);
         return FB_OK;
      }
      char uri[URI_MAX];
      if (!fn_host_resolve(s_boot_uri, uri, sizeof uri))
         return FB_INVALID_REQUEST;
      return mount(s, slot1, uri, s_boot_ro, TYPE_AUTO, 0, false, out);
   }
   case DISK_REINITIALIZE: {
      uint8_t slot1 = fb_get_u8(in);
      uint16_t ssize = fb_get_u16(in);
      uint32_t count = fb_get_u32(in);
      if (in->bad || (st = slot_of(slot1, &s)) != FB_OK)
         return FB_INVALID_REQUEST;
      if ((st = active(s)) != FB_OK)
         return st;
      if (s->ro)
         return FB_INVALID_REQUEST;
      char uri[URI_MAX];
      uint8_t type = s->type;
      snprintf(uri, sizeof uri, "%s", s->uri);
      close_slot(s);
      if ((st = create_image(uri, type, ssize, count, true)) != FB_OK)
         return st;
      return mount(s, slot1, uri, false, type, type == TYPE_RAW ? ssize : 0, false, out);
   }
   case DISK_LIST_MOUNTS: {
      uint8_t flags = fb_get_u8(in);
      uint16_t first = fb_get_u16(in), last = fb_get_u16(in);
      uint16_t start = fb_get_u16(in), max = fb_get_u16(in);
      if (in->bad || fb_left(in) || !(flags & 1u) || max == 0)
         return FB_INVALID_REQUEST;
      bool all = first == 0 && last == 0;
      if (!all && (first > last || last >= SLOTS))
         return FB_INVALID_REQUEST;
      unsigned int unit[SLOTS], n = 0;
      for (unsigned int i = 0; i < SLOTS; i++)
         if (s_slot[i].used && (all || (i >= first && i <= last)))
            unit[n++] = i;
      fb_put_u8(out, FB_VERSION);
      uint8_t *resp_flags = fb_put_space(out, 1);
      fb_put_u16(out, n ? (uint16_t)unit[0] : 0u);
      fb_put_u16(out, start);
      uint8_t *count_at = fb_put_space(out, 2);
      uint8_t *len_at = fb_put_space(out, 2);
      if (out->full)
         return FB_INTERNAL_ERROR;
      uint16_t count = 0, used = 0;
      for (unsigned int k = start; k < n; k++) {
         char line[URI_MAX + 16];
         const slot_t *m = &s_slot[unit[k]];
         int ln = snprintf(line, sizeof line, "%u: %s %s\n", unit[k],
                           (m->pending ? m->ro_requested : m->ro) ? "RO" : "AUTO",
                           m->uri);
         if (ln < 0 || (size_t)ln >= sizeof line || ln > max)
            return FB_INVALID_REQUEST;
         if (used + ln > max || ln > fb_room(out))
            break;
         fb_put_bytes(out, line, (uint16_t)ln);
         used = (uint16_t)(used + ln);
         count++;
      }
      resp_flags[0] = (uint8_t)(0x02u | (start + count < n ? 0x01u : 0u));
      count_at[0] = (uint8_t)count; count_at[1] = (uint8_t)(count >> 8);
      len_at[0] = (uint8_t)used;    len_at[1] = (uint8_t)(used >> 8);
      return FB_OK;
   }
   default:
      return FB_UNSUPPORTED;
   }
}
