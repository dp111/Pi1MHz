/* fn_appstore.c - AppStore service ($F1): namespaced opaque byte storage.

   Protocol: fujinet-nio docs/app_store_service_protocol.md.  Every request
   is  u8 version, u16 nsLen, ns, u16 keyLen, key  followed by the command's
   own fields.  fn-rom uses Read, Write and Delete (namespace "config-nio",
   its drive-to-slot mappings); Stat and List are here for completeness.

   Each value is one file, sd0:/FujiNet/app-store/v1/<ns>/<key>, the layout
   fujinet-nio uses.  Namespace and key are arbitrary bytes, so each is
   escaped into a file-name-safe form. */

#include <stdio.h>
#include <string.h>

#include "fn_devices.h"
#include "fn_store.h"

#define APP_FS    "sd0"
#define APP_ROOT  "/FujiNet/app-store/v1"
#define APP_NAME_MAX 255u

#define APP_STAT   0x01u
#define APP_READ   0x02u
#define APP_WRITE  0x03u

#define APP_VALUE_MAX  (64u * 1024u)   /* the largest value a Write may make */
#define APP_DELETE 0x04u
#define APP_LIST   0x05u

/* Letters, digits and ._- stay; every other byte is %XX.  False if the
   result would not fit. */
static bool escape(const uint8_t *s, uint16_t n, char *out, size_t cap)
{
   static const char hex[] = "0123456789ABCDEF";
   size_t o = 0;
   for (uint16_t i = 0; i < n; i++) {
      uint8_t c = s[i];
      bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
      if (plain) {
         if (o + 1 >= cap) return false;
         out[o++] = (char)c;
      } else {
         if (o + 3 >= cap) return false;
         out[o++] = '%';
         out[o++] = hex[c >> 4];
         out[o++] = hex[c & 15u];
      }
   }
   out[o] = '\0';
   /* "." and ".." are directory names, not keys. */
   return o != 0 && strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
}

static bool value_path(const uint8_t *ns, uint16_t ns_len,
                       const uint8_t *key, uint16_t key_len,
                       char *dir, char *path)
{
   char e_ns[APP_NAME_MAX + 1], e_key[APP_NAME_MAX + 1];
   if (!escape(ns, ns_len, e_ns, sizeof e_ns))
      return false;
   if (snprintf(dir, FN_STORE_MAX_PATH, "%s/%s", APP_ROOT, e_ns) >= (int)FN_STORE_MAX_PATH)
      return false;
   if (!key)
      return true;
   if (!escape(key, key_len, e_key, sizeof e_key))
      return false;
   return snprintf(path, FN_STORE_MAX_PATH, "%s/%s", dir, e_key) < (int)FN_STORE_MAX_PATH;
}

/* ---- the C interface the other devices use ------------------------------ */

static bool paths_c(const char *ns, const char *key, char *dir, char *path)
{
   return value_path((const uint8_t *)ns, (uint16_t)strlen(ns),
                     (const uint8_t *)key, (uint16_t)strlen(key), dir, path);
}

static bool app_read(const char *dir, const char *path, uint32_t offset,
                     uint8_t *buf, uint16_t max, uint16_t *got, bool *exists)
{
   (void)dir;
   *got = 0;
   if (exists) *exists = false;
   fn_handle h = fn_store_open(APP_FS, path, FN_OPEN_READ);
   if (h == FN_NO_HANDLE)
      return true;                         /* absent is not an error */
   if (exists) *exists = true;
   uint32_t size = 0;
   bool ok = fn_store_size(h, &size);
   if (ok && offset < size) {
      uint32_t n = size - offset;
      if (n > max) n = max;
      ok = fn_store_read(h, offset, buf, n);
      if (ok) *got = (uint16_t)n;
   }
   fn_store_close(h);
   return ok;
}

static bool app_write(const char *dir, const char *path, uint32_t offset,
                      const uint8_t *data, uint16_t len)
{
   /* As nio: a write at offset 0 replaces the value ("wb"), one further in
      updates it ("r+b"). */
   fn_handle h = offset == 0 ? FN_NO_HANDLE : fn_store_open(APP_FS, path, FN_OPEN_UPDATE);
   if (h == FN_NO_HANDLE) {
      if (!fn_store_mkdirs(APP_FS, dir))
         return false;
      h = fn_store_open(APP_FS, path, FN_OPEN_CREATE);
      if (h == FN_NO_HANDLE)
         return false;
   }
   bool ok = len == 0 || fn_store_write(h, offset, data, len);
   ok = fn_store_sync(h) && ok;
   fn_store_close(h);
   return ok;
}

static bool app_delete(const char *path, bool *existed)
{
   fn_handle h = fn_store_open(APP_FS, path, FN_OPEN_READ);
   *existed = h != FN_NO_HANDLE;
   if (!*existed)
      return true;
   fn_store_close(h);
   return fn_store_delete(APP_FS, path);
}

bool fn_app_read(const char *ns, const char *key, uint32_t offset,
                 uint8_t *buf, uint16_t max, uint16_t *got, bool *exists)
{
   char dir[FN_STORE_MAX_PATH], path[FN_STORE_MAX_PATH];
   *got = 0;
   return paths_c(ns, key, dir, path) &&
          app_read(dir, path, offset, buf, max, got, exists);
}

bool fn_app_write(const char *ns, const char *key, uint32_t offset,
                  const uint8_t *data, uint16_t len)
{
   char dir[FN_STORE_MAX_PATH], path[FN_STORE_MAX_PATH];
   return paths_c(ns, key, dir, path) && app_write(dir, path, offset, data, len);
}

bool fn_app_delete(const char *ns, const char *key, bool *existed)
{
   char dir[FN_STORE_MAX_PATH], path[FN_STORE_MAX_PATH];
   bool dummy;
   if (!existed) existed = &dummy;
   *existed = false;
   return paths_c(ns, key, dir, path) && app_delete(path, existed);
}

/* ---- the wire device ----------------------------------------------------- */

uint8_t fn_appstore_command(uint8_t command, fb_in *in, fb_out *out)
{
   if (fb_get_u8(in) != FB_VERSION)
      return FB_INVALID_REQUEST;
   uint16_t ns_len = fb_get_u16(in);
   const uint8_t *ns = fb_get_bytes(in, ns_len);
   uint16_t key_len = fb_get_u16(in);
   const uint8_t *key = fb_get_bytes(in, key_len);
   if (in->bad || ns_len == 0 || ns_len > 255u || key_len > 255u)
      return FB_INVALID_REQUEST;
   if (key_len == 0 && command != APP_LIST)
      return FB_INVALID_REQUEST;

   char dir[FN_STORE_MAX_PATH], path[FN_STORE_MAX_PATH];
   if (!value_path(ns, ns_len, command == APP_LIST ? NULL : key, key_len, dir, path))
      return FB_INVALID_REQUEST;

   switch (command) {
   case APP_STAT: {
      fn_handle h = fn_store_open(APP_FS, path, FN_OPEN_READ);
      uint32_t size = 0;
      if (h != FN_NO_HANDLE) {
         (void)fn_store_size(h, &size);
         fn_store_close(h);
      }
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, h != FN_NO_HANDLE ? 1u : 0u);
      fb_put_u16(out, 0);
      fb_put_u32(out, size); fb_put_u32(out, 0);    /* u64 size */
      fb_put_u32(out, 0);    fb_put_u32(out, 0);    /* u64 mtime: not kept */
      return FB_OK;
   }
   case APP_READ: {
      uint32_t offset = fb_get_u32(in);
      uint16_t max = fb_get_u16(in);
      if (in->bad)
         return FB_INVALID_REQUEST;
      fb_put_u8(out, FB_VERSION);
      uint8_t *flags = fb_put_space(out, 1);
      fb_put_u16(out, 0);
      fb_put_u32(out, offset);
      uint8_t *len_at = fb_put_space(out, 2);
      if (max > fb_room(out))
         max = fb_room(out);
      uint8_t *data = out->full ? NULL : out->p + out->len;
      uint16_t got = 0;
      bool exists = false;
      if (!data || !app_read(dir, path, offset, data, max, &got, &exists))
         return data ? FB_IO_ERROR : FB_INTERNAL_ERROR;
      (void)fb_put_space(out, got);
      /* EOF when nothing is left beyond what this read returned. */
      uint8_t more_probe;
      uint16_t more = 0;
      if (exists)
         (void)app_read(dir, path, offset + got, &more_probe, 1, &more, NULL);
      flags[0] = (uint8_t)((more == 0 ? 1u : 0u) | (exists ? 2u : 0u));
      len_at[0] = (uint8_t)got;
      len_at[1] = (uint8_t)(got >> 8);
      return FB_OK;
   }
   case APP_WRITE: {
      uint32_t offset = fb_get_u32(in);
      uint16_t len = fb_get_u16(in);
      const uint8_t *data = fb_get_bytes(in, len);
      if (in->bad)
         return FB_INVALID_REQUEST;
      /* Values are small (nio keeps settings here); a far offset would have
         the SD backend zero-fill up to it, synchronously, in the main loop. */
      if ((uint64_t)offset + len > APP_VALUE_MAX)
         return FB_INVALID_REQUEST;
      if (!app_write(dir, path, offset, data, len))
         return FB_IO_ERROR;
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, 0);
      fb_put_u16(out, 0);
      fb_put_u32(out, offset);
      fb_put_u16(out, len);
      return FB_OK;
   }
   case APP_DELETE: {
      bool existed = false;
      if (!app_delete(path, &existed))
         return FB_IO_ERROR;
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, existed ? 1u : 0u);
      fb_put_u16(out, 0);
      return FB_OK;
   }
   case APP_LIST: {
      uint16_t start = fb_get_u16(in);
      uint16_t max_payload = fb_get_u16(in);
      if (in->bad)
         return FB_INVALID_REQUEST;
      fb_put_u8(out, FB_VERSION);
      uint8_t *flags = fb_put_space(out, 1);
      fb_put_u16(out, 0);
      fb_put_u16(out, start);
      uint8_t *count_at = fb_put_space(out, 2);
      uint8_t *len_at = fb_put_space(out, 2);
      if (out->full)
         return FB_INTERNAL_ERROR;
      uint16_t count = 0, blob = 0;
      bool more = false;
      fn_dirent e;
      uint32_t key_index = 0;
      for (uint32_t i = 0; fn_store_dir_entry(APP_FS, dir, i, &e); i++) {
         if (e.is_dir || key_index++ < start)
            continue;
         /* Keys are listed as stored (escaped): List is not used by fn-rom
            and fujinet-nio's own key names are all plain. */
         uint16_t n = (uint16_t)strlen(e.name);
         if (blob + 2u + n > max_payload || 2u + n > fb_room(out)) {
            more = true;
            break;
         }
         fb_put_u16(out, n);
         fb_put_bytes(out, e.name, n);
         blob = (uint16_t)(blob + 2u + n);
         count++;
      }
      flags[0] = more ? 1u : 0u;
      count_at[0] = (uint8_t)count; count_at[1] = (uint8_t)(count >> 8);
      len_at[0] = (uint8_t)blob;    len_at[1] = (uint8_t)(blob >> 8);
      return FB_OK;
   }
   default:
      return FB_UNSUPPORTED;
   }
}
