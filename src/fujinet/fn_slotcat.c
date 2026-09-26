/* fn_slotcat.c - SlotCatalogService ($F2): the user's 256 disk-image choices.

   Protocol: fujinet-nio docs/slot_catalog_service_protocol.md; behaviour
   from src/lib/slot_catalog_service.cpp and slot_catalog.cpp.  fn-rom:
   *FIN puts, *FOUT deletes, *FSLOTS ranges, *FMOUNT gets.

   Each entry is one app-store record, namespace "fujinet-slots", key the
   decimal index, holding [flags][canonical URI].  An occupancy bitmap is
   built from one directory scan on first use and kept up to date by Put and
   Delete, so Range does not open 256 records. */

#include <stdio.h>
#include <string.h>

#include "fn_devices.h"
#include "fn_store.h"

#define SLOT_NS      "fujinet-slots"
#define SLOT_DIR     "/FujiNet/app-store/v1/" SLOT_NS
#define URI_MAX      FN_STORE_MAX_PATH

#define SC_GET       0x01u
#define SC_PUT       0x02u
#define SC_DELETE    0x03u
#define SC_RANGE     0x04u

#define ENTRY_VALID      0x01u
#define ENTRY_READONLY   0x02u
#define ENTRY_TRUNCATED  0x04u
#define REQ_TAIL_URI     0x01u
#define REQ_FORMATTED    0x02u
#define RESP_MORE        0x01u
#define RESP_FORMATTED   0x02u
#define DELETE_REMOVED   0x01u

static uint8_t s_occupied[32];
static bool s_occupied_valid;

static void key_of(uint8_t index, char *key) { sprintf(key, "%u", index); }

static bool occupied(uint8_t i) { return ((unsigned)s_occupied[i >> 3] >> (i & 7u)) & 1u; }

static void set_occupied(uint8_t i, bool on)
{
   if (on) s_occupied[i >> 3] = (uint8_t)(s_occupied[i >> 3] | (1u << (i & 7u)));
   else    s_occupied[i >> 3] = (uint8_t)(s_occupied[i >> 3] & ~(1u << (i & 7u)));
}

static void ensure_index(void)
{
   if (s_occupied_valid)
      return;
   memset(s_occupied, 0, sizeof s_occupied);
   fn_dirent e;
   for (uint32_t i = 0; fn_store_dir_entry("sd0", SLOT_DIR, i, &e); i++) {
      unsigned int v = 0;
      const char *s = e.name;
      if (e.is_dir || !*s) continue;
      while (*s >= '0' && *s <= '9' && v < 256u) v = v * 10u + (unsigned)(*s++ - '0');
      if (*s == '\0' && v < 256u)
         set_occupied((uint8_t)v, true);
   }
   s_occupied_valid = true;
}

/* An entry: flags (ENTRY_VALID set if present) and its URI. */
static bool get_entry(uint8_t index, uint8_t *flags, char *uri)
{
   uint8_t rec[1 + URI_MAX];
   uint16_t got = 0;
   bool exists = false;
   char key[4];
   key_of(index, key);
   *flags = 0;
   uri[0] = '\0';
   if (!fn_app_read(SLOT_NS, key, 0, rec, sizeof rec - 1, &got, &exists))
      return false;
   if (!exists || got < 2)
      return true;
   *flags = (uint8_t)(ENTRY_VALID | ((rec[0] & 1u) ? ENTRY_READONLY : 0u));
   memcpy(uri, rec + 1, got - 1u);
   uri[got - 1u] = '\0';
   return true;
}

static void put_entry_reply(fb_out *out, uint8_t index, uint8_t flags, const char *uri)
{
   uint16_t n = (uint16_t)strlen(uri);
   fb_put_u8(out, FB_VERSION);
   fb_put_u8(out, flags);
   fb_put_u8(out, index);
   fb_put_u16(out, n);
   fb_put_bytes(out, uri, n);
}

static unsigned int digits(unsigned int v) { return v >= 100u ? 3u : v >= 10u ? 2u : 1u; }

uint8_t fn_slotcat_command(uint8_t command, fb_in *in, fb_out *out)
{
   if (fb_get_u8(in) != FB_VERSION)
      return FB_INVALID_REQUEST;

   switch (command) {
   case SC_GET: {
      uint8_t index = fb_get_u8(in), flags;
      char uri[URI_MAX];
      if (in->bad || fb_left(in))
         return FB_INVALID_REQUEST;
      if (!get_entry(index, &flags, uri))
         return FB_IO_ERROR;
      if (!(flags & ENTRY_VALID))
         return FB_DEVICE_NOT_FOUND;
      put_entry_reply(out, index, flags, uri);
      return FB_OK;
   }
   case SC_PUT: {
      uint8_t index = fb_get_u8(in);
      uint8_t flags = fb_get_u8(in);
      uint16_t n = fb_get_u16(in);
      const uint8_t *target = fb_get_bytes(in, n);
      char spec[URI_MAX], uri[URI_MAX];
      if (in->bad || fb_left(in) || n == 0 || n >= sizeof spec ||
          (flags & ~ENTRY_READONLY))
         return FB_INVALID_REQUEST;
      memcpy(spec, target, n);
      spec[n] = '\0';
      /* A relative target resolves against the current host; a canonical URI
         stays as it is. */
      if (!fn_host_resolve(spec, uri, sizeof uri))
         return FB_DEVICE_NOT_FOUND;
      uint8_t rec[1 + URI_MAX];
      uint16_t len = (uint16_t)strlen(uri);
      rec[0] = (flags & ENTRY_READONLY) ? 1u : 0u;
      memcpy(rec + 1, uri, len);
      char key[4];
      key_of(index, key);
      (void)fn_app_delete(SLOT_NS, key, NULL);
      if (!fn_app_write(SLOT_NS, key, 0, rec, (uint16_t)(len + 1u)))
         return FB_IO_ERROR;
      ensure_index();
      set_occupied(index, true);
      put_entry_reply(out, index, (uint8_t)(ENTRY_VALID | (flags & ENTRY_READONLY)), uri);
      return FB_OK;
   }
   case SC_DELETE: {
      uint8_t index = fb_get_u8(in);
      bool existed = false;
      char key[4];
      if (in->bad || fb_left(in))
         return FB_INVALID_REQUEST;
      key_of(index, key);
      if (!fn_app_delete(SLOT_NS, key, &existed))
         return FB_IO_ERROR;
      ensure_index();
      set_occupied(index, false);
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, existed ? DELETE_REMOVED : 0u);
      fb_put_u8(out, index);
      return FB_OK;
   }
   case SC_RANGE: {
      uint8_t lower = fb_get_u8(in), upper = fb_get_u8(in), cursor = fb_get_u8(in);
      uint8_t flags = fb_get_u8(in), max_uri = fb_get_u8(in);
      uint16_t max_payload = fb_get_u16(in);
      if (in->bad || fb_left(in) || (flags & ~(REQ_TAIL_URI | REQ_FORMATTED)) ||
          lower > upper || cursor < lower || cursor > upper || max_uri == 0)
         return FB_INVALID_REQUEST;
      unsigned int presence = ((unsigned)upper - lower + 8u) / 8u;
      if (max_payload < presence + 3u)
         return FB_INVALID_REQUEST;
      ensure_index();

      bool formatted = flags & REQ_FORMATTED;
      fb_put_u8(out, FB_VERSION);
      uint8_t *resp_flags = fb_put_space(out, 1);
      uint8_t *next_at = fb_put_space(out, 1);
      fb_put_u8(out, (uint8_t)presence);
      uint8_t *count_at = fb_put_space(out, 1);
      uint8_t *len_at = fb_put_space(out, 2);
      uint8_t *bitmap = fb_put_space(out, (uint16_t)presence);
      if (out->full)
         return FB_INTERNAL_ERROR;
      memset(bitmap, 0, presence);
      for (unsigned int i = lower; i <= upper; i++)
         if (occupied((uint8_t)i))
            bitmap[(i - lower) >> 3] = (uint8_t)(bitmap[(i - lower) >> 3] | (1u << ((i - lower) & 7u)));

      unsigned int used = presence, count = 0;
      uint16_t blob_start = out->len;
      uint8_t more = 0, next = 0;
      for (unsigned int i = cursor; i <= upper; i++) {
         uint8_t idx = (uint8_t)i, ef;
         char uri[URI_MAX];
         if (!occupied(idx)) continue;
         if (!get_entry(idx, &ef, uri))
            return FB_IO_ERROR;
         const char *u = uri;
         size_t ul = strlen(uri);
         if (ef & ENTRY_VALID) {
            if (ul > max_uri) {
               ef |= ENTRY_TRUNCATED;
               if (flags & REQ_TAIL_URI) u = uri + (ul - max_uri);
               ul = max_uri;
            }
         } else {
            /* Occupied in the bitmap but gone from the card: shown as
               "<invalid>", as fujinet-nio does. */
            u = "<invalid>";
            ul = formatted ? 9u : 0u;
         }
         unsigned int bytes = formatted ? digits(idx) + 2u + (unsigned)ul + 1u
                                        : 3u + (unsigned)ul;
         if (used + bytes > max_payload || bytes > fb_room(out)) {
            more = RESP_MORE;
            next = idx;
            break;
         }
         used += bytes;
         count++;
         if (formatted) {
            char head[8];
            int hn = snprintf(head, sizeof head, "%u: ", idx);
            fb_put_bytes(out, head, (uint16_t)hn);
            fb_put_bytes(out, u, (uint16_t)ul);
            fb_put_u8(out, '\n');
         } else {
            fb_put_u8(out, idx);
            fb_put_u8(out, ef);
            fb_put_u8(out, (uint8_t)ul);
            fb_put_bytes(out, u, (uint16_t)ul);
         }
      }
      uint16_t blob = (uint16_t)(out->len - blob_start);
      resp_flags[0] = (uint8_t)(more | (formatted ? RESP_FORMATTED : 0u));
      next_at[0] = next;
      count_at[0] = (uint8_t)count;
      len_at[0] = (uint8_t)blob;
      len_at[1] = (uint8_t)(blob >> 8);
      return FB_OK;
   }
   default:
      return FB_UNSUPPORTED;
   }
}
