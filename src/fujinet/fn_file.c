/* fn_file.c - FileService ($FE): ListDirectory, the one command fn-rom sends
   (*FLS).

   Protocol: fujinet-nio docs/file_device_protocol.md; the formatted line and
   the sort follow src/lib/file_device.cpp and list_directory_format.cpp.
   fn-rom asks for sorted, formatted text (list flags $06) and prints it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fn_devices.h"
#include "fn_store.h"

#define FILE_LIST_DIRECTORY 0x02u

#define LIST_COMPACT   0x01u
#define LIST_SORT      0x02u
#define LIST_FORMATTED 0x04u

#define MAX_ENTRIES    1024u
#define NAME_POOL      (64u * 1024u)
#define URI_MAX        FN_STORE_MAX_PATH

typedef struct {
   uint32_t name;         /* offset into s_names */
   uint32_t size;
   uint32_t mtime;
   bool is_dir;
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
static char s_names[NAME_POOL];

static int by_name(const void *a, const void *b)
{
   return strcmp(s_names + ((const entry_t *)a)->name,
                 s_names + ((const entry_t *)b)->name);
}

/* "1.5K"-style, as ls -h. */
static void human_size(uint32_t bytes, char *out, size_t cap)
{
   /* nio's format_size_readable, in integers: the release printf has no
      float support.  bytes / 1024^u is exact in binary, so rounding it to
      tenths here - a tie to even, as printf does - gives printf's digits. */
   static const char units[] = "BKMGT";
   if (bytes == 0) {
      snprintf(out, cap, "0");
      return;
   }
   uint64_t den = 1;
   unsigned u = 0;
   while (u < 4 && bytes >= 1000u * den) {
      den *= 1024u;
      u++;
   }
   if (u == 0) {
      snprintf(out, cap, "%luB", (unsigned long)bytes);
      return;
   }
   uint64_t num = (uint64_t)bytes * 10u;
   uint64_t tenths = num / den, rem = num % den;
   if (2u * rem > den || (2u * rem == den && (tenths & 1u)))
      tenths++;
   snprintf(out, cap, "%lu.%lu%c", (unsigned long)(tenths / 10u),
            (unsigned long)(tenths % 10u), units[u]);
}

/* "Mon dd  YYYY" (the Pi has no trusted "now" to choose ls's HH:MM form for
   recent files), or "??? ?? ??:??" when the time is unknown. */
static void ls_date(uint32_t t, char *out, size_t cap)
{
   static const char mon[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
   if (t == 0) {
      snprintf(out, cap, "??? ?? ??:??");
      return;
   }
   /* Civil date from days since 1970 (Howard Hinnant's algorithm). */
   int64_t z = (int64_t)(t / 86400u) + 719468;
   int64_t era = z / 146097;
   int64_t doe = z - era * 146097;
   int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
   int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
   int64_t mp = (5 * doy + 2) / 153;
   int d = (int)(doy - (153 * mp + 2) / 5 + 1);
   int m = (int)(mp < 10 ? mp + 3 : mp - 9);
   int64_t y = yoe + era * 400 + (m <= 2);
   snprintf(out, cap, "%s %2d  %4d", mon[m - 1], d, (int)y);
}

static int format_line(const entry_t *e, char *line, size_t cap)
{
   char size[16], date[16];
   human_size(e->size, size, sizeof size);
   ls_date(e->mtime, date, sizeof date);
   return snprintf(line, cap, "%c %7s %12s %s%s\n", e->is_dir ? 'd' : '-',
                   e->is_dir ? "0" : size, date, s_names + e->name,
                   e->is_dir ? "/" : "");
}

uint8_t fn_file_command(uint8_t command, fb_in *in, fb_out *out)
{
   if (command != FILE_LIST_DIRECTORY)
      return FB_UNSUPPORTED;
   if (fb_get_u8(in) != FB_VERSION)
      return FB_INVALID_REQUEST;
   uint16_t ulen = fb_get_u16(in);
   const uint8_t *u = fb_get_bytes(in, ulen);
   uint16_t start = fb_get_u16(in);
   uint16_t max = fb_get_u16(in);
   uint8_t flags = fb_left(in) ? fb_get_u8(in) : 0u;
   if ((flags & LIST_FORMATTED) && fb_left(in))
      (void)fb_get_u8(in);              /* lineWidth, optional (fn-rom sends
                                           none): not applied, as upstream */
   char spec[URI_MAX], uri[URI_MAX], fs[16], path[URI_MAX];
   /* An empty path is the current directory (fn-rom's plain *FLS). */
   if (in->bad || ulen >= sizeof spec || max == 0 ||
       ((flags & LIST_COMPACT) && (flags & LIST_FORMATTED)))
      return FB_INVALID_REQUEST;
   if (ulen)
      memcpy(spec, u, ulen);
   spec[ulen] = '\0';
   if (!fn_host_resolve(spec, uri, sizeof uri) ||
       !fn_uri_split(uri, fs, sizeof fs, path, sizeof path))
      return FB_DEVICE_NOT_FOUND;
   if (!fn_store_is_dir(fs, path))
      return FB_IO_ERROR;

   /* The whole directory, so it can be sorted before paging. */
   unsigned int total = 0;
   uint32_t pool = 0;
   fn_dirent d;
   for (uint32_t i = 0; total < MAX_ENTRIES && fn_store_dir_entry(fs, path, i, &d); i++) {
      size_t n = strlen(d.name) + 1;
      if (pool + n > NAME_POOL)
         break;
      memcpy(s_names + pool, d.name, n);
      s_entries[total] = (entry_t){ .name = pool, .size = d.size,
                                    .mtime = d.mtime, .is_dir = d.is_dir };
      pool += (uint32_t)n;
      total++;
   }
   if (flags & LIST_SORT)
      qsort(s_entries, total, sizeof s_entries[0], by_name);

   fb_put_u8(out, FB_VERSION);
   uint8_t *resp_flags = fb_put_space(out, 1);
   fb_put_u16(out, 0);
   fb_put_u16(out, start);
   uint8_t *count_at = fb_put_space(out, 2);
   uint8_t *len_at = fb_put_space(out, 2);
   if (out->full)
      return FB_INTERNAL_ERROR;

   uint16_t count = 0, used = 0;
   for (unsigned int i = start; i < total; i++) {
      const entry_t *e = &s_entries[i];
      char buf[FN_STORE_NAME_MAX + 48];
      uint16_t n;
      if (flags & LIST_FORMATTED) {
         int ln = format_line(e, buf, sizeof buf);
         if (ln < 0 || (size_t)ln >= sizeof buf) continue;
         n = (uint16_t)ln;
      } else {
         const char *name = s_names + e->name;
         size_t nl = strlen(name);
         if (nl > 255u) nl = 255u;
         buf[0] = e->is_dir ? 1 : 0;
         buf[1] = (char)nl;
         memcpy(buf + 2, name, nl);
         n = (uint16_t)(2u + nl);
         if (!(flags & LIST_COMPACT)) {
            memset(buf + n, 0, 16);
            for (int k = 0; k < 4; k++) {
               buf[n + k] = (char)(e->size >> (8 * k));
               buf[n + 8 + k] = (char)(e->mtime >> (8 * k));
            }
            n = (uint16_t)(n + 16u);
         }
      }
      if (used + n > max || n > fb_room(out))
         break;
      fb_put_bytes(out, buf, n);
      used = (uint16_t)(used + n);
      count++;
   }
   bool more = start + count < total;
   resp_flags[0] = (uint8_t)((more ? 1u : 0u) | ((flags & LIST_COMPACT) ? 2u : 0u) |
                             ((flags & LIST_FORMATTED) ? 4u : 0u));
   count_at[0] = (uint8_t)count; count_at[1] = (uint8_t)(count >> 8);
   len_at[0] = (uint8_t)used;    len_at[1] = (uint8_t)(used >> 8);
   return FB_OK;
}
