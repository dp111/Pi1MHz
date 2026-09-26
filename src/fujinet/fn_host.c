/* fn_host.c - HostService ($F0) and URI resolution.

   Protocol: fujinet-nio docs/host_service_protocol.md.  Resolution follows
   fujinet-nio src/lib/host_state.cpp and src/lib/path_resolvers/ rule
   for rule (HTTP resolvers omitted: this device has no HTTP filesystem):

     1. tnfs://host[:port]/path (and tnfs+tcp:// etc.) - itself, normalised
     2. tnfs://... written as tnfs:<//host...>   - kept as given
     3. relative to a current tnfs host           - joined and normalised
     4. fs:path                                    - fs:/normalised-path
     5. anything else, relative to the current host

   A spec that is empty or starts with '/' is always taken relative to the
   current host; anything else is first tried on its own.

   State lives in the app store, namespace "fujinet-nio", under fujinet-nio's
   own key names: current-host, current-display-path, host-history (one URI
   per line, most recent first, at most 32). */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "fn_devices.h"
#include "fn_store.h"

#define HOST_NS        "fujinet-nio"
#define KEY_CURRENT    "current-host"
#define KEY_DISPLAY    "current-display-path"
#define KEY_HISTORY    "host-history"
#define HISTORY_MAX    32u
#define URI_MAX        FN_STORE_MAX_PATH
#define HISTORY_BYTES  (HISTORY_MAX * URI_MAX)

#define HOST_GET_CURRENT    0x01u
#define HOST_SET_CURRENT    0x02u
#define HOST_LIST_HISTORY   0x03u
#define HOST_SELECT_HISTORY 0x04u
#define HOST_DELETE_HISTORY 0x05u

/* ---- path utilities (path_resolver_utils.cpp, tnfs_path_utils.cpp) ------ */

static bool cat(char *dst, size_t cap, const char *a, size_t an, const char *b, size_t bn)
{
   if (an + bn + 1 > cap) return false;
   memmove(dst, a, an);
   memcpy(dst + an, b, bn);
   dst[an + bn] = '\0';
   return true;
}

static bool fs_join(char *out, size_t cap, const char *base, const char *rel)
{
   size_t bn = strlen(base), rn = strlen(rel);
   if (bn == 0) return cat(out, cap, "", 0, rel, rn);
   if (base[bn - 1] == '/') {
      if (rn && rel[0] == '/') return cat(out, cap, base, bn, rel + 1, rn - 1);
      return cat(out, cap, base, bn, rel, rn);
   }
   if (rn && rel[0] == '/') return cat(out, cap, base, bn, rel, rn);
   char tmp[URI_MAX];
   if (!cat(tmp, sizeof tmp, base, bn, "/", 1)) return false;
   return cat(out, cap, tmp, bn + 1, rel, rn);
}

/* Collapse "//", "." and ".."; the result always starts with '/' and never
   ends with one (except "/" itself). */
static bool fs_norm(char *out, size_t cap, const char *in)
{
   const char *seg[64];
   size_t len[64];
   unsigned int n = 0;
   const char *s = in;
   while (*s) {
      while (*s == '/') s++;
      const char *start = s;
      while (*s && *s != '/') s++;
      size_t l = (size_t)(s - start);
      if (l == 0) break;
      if (l == 1 && start[0] == '.') continue;
      if (l == 2 && start[0] == '.' && start[1] == '.') {
         if (n) n--;
         continue;
      }
      if (n == 64) return false;
      seg[n] = start;
      len[n++] = l;
   }
   size_t o = 0;
   if (cap < 2) return false;
   out[o++] = '/';
   for (unsigned int k = 0; k < n; k++) {
      if (k) {
         if (o + 1 >= cap) return false;
         out[o++] = '/';
      }
      if (o + len[k] >= cap) return false;
      memcpy(out + o, seg[k], len[k]);
      o += len[k];
   }
   out[o] = '\0';
   return true;
}

static bool starts_ci(const char *s, const char *prefix)
{
   for (; *prefix; s++, prefix++)
      if (tolower((unsigned char)*s) != *prefix) return false;
   return true;
}

static bool is_tnfs_uri(const char *s)
{
   return starts_ci(s, "tnfs://") || starts_ci(s, "tnfs+tcp://") ||
          starts_ci(s, "tnfstcp://") || starts_ci(s, "tnfs-tcp://");
}

static bool is_tnfs_endpoint(const char *p)
{
   return (p[0] == '/' && p[1] == '/') || is_tnfs_uri(p);
}

/* Join `rel` onto `base` keeping a URI's scheme and authority intact. */
static bool tnfs_join_relative(char *out, size_t cap, const char *base, const char *rel)
{
   char joined[URI_MAX], norm[URI_MAX];
   const char *scheme = strstr(base, "://");
   size_t path_start;
   if (scheme) {
      const char *slash = strchr(scheme + 3, '/');
      if (!slash) {
         /* No path yet: "tnfs://host" + "x" -> "tnfs://host/x"; ".." -> "/". */
         if (strcmp(rel, "..") == 0) rel = "";
         return snprintf(out, cap, "%s/%s", base, rel) < (int)cap;
      }
      path_start = (size_t)(slash - base);
   } else if (base[0] == '/' && base[1] == '/') {
      const char *slash = strchr(base + 2, '/');
      if (!slash)
         return snprintf(out, cap, "%s/%s", base, rel) < (int)cap;
      path_start = (size_t)(slash - base);
   } else {
      return fs_join(joined, sizeof joined, base, rel) &&
             fs_norm(out, cap, joined);
   }
   if (!fs_join(joined, sizeof joined, base + path_start, rel) ||
       !fs_norm(norm, sizeof norm, joined))
      return false;
   /* fs_norm yields "/" for the root, so "tnfs://h" + "/" keeps its slash. */
   return cat(out, cap, base, path_start, norm, strlen(norm));
}

/* ---- the resolver chain (path_resolver.cpp) ------------------------------ */

typedef struct {
   char fs[16];
   char path[URI_MAX];
} target_t;

static bool set_fs(target_t *t, const char *fs, size_t n)
{
   if (n == 0 || n >= sizeof t->fs) return false;
   memcpy(t->fs, fs, n);
   t->fs[n] = '\0';
   return true;
}

static bool resolve(const char *spec, const target_t *ctx, target_t *out)
{
   /* 1. tnfs:// URI */
   if (is_tnfs_uri(spec))
      return set_fs(out, "tnfs", 4) &&
             tnfs_join_relative(out->path, sizeof out->path, spec, "");
   /* 2. tnfs: + endpoint */
   if (strncmp(spec, "tnfs:", 5) == 0 && is_tnfs_endpoint(spec + 5))
      return set_fs(out, "tnfs", 4) &&
             snprintf(out->path, sizeof out->path, "%s", spec) < (int)sizeof out->path;
   /* 3. relative to a tnfs current host */
   if (ctx && strcmp(ctx->fs, "tnfs") == 0 &&
       (is_tnfs_endpoint(ctx->path) || (spec[0] && is_tnfs_endpoint(spec)))) {
      if (!set_fs(out, "tnfs", 4)) return false;
      if (spec[0] == '/' && is_tnfs_endpoint(spec))
         return snprintf(out->path, sizeof out->path, "%s", spec) < (int)sizeof out->path;
      if (spec[0] == '/') {
         /* DIVERGES from fujinet-nio, which yields "tnfs:/top" here - the
            server is lost and the result fails its own directory check.  A
            rooted path on a tnfs host means that host's root. */
         const char *scheme = strstr(ctx->path, "://");
         const char *slash = scheme ? strchr(scheme + 3, '/') : NULL;
         size_t keep = scheme ? (slash ? (size_t)(slash - ctx->path) : strlen(ctx->path)) : 0;
         char norm[URI_MAX];
         return fs_norm(norm, sizeof norm, spec) &&
                cat(out->path, sizeof out->path, ctx->path, keep, norm, strlen(norm));
      }
      return tnfs_join_relative(out->path, sizeof out->path, ctx->path, spec);
   }
   /* 4. fs:path */
   const char *colon = strchr(spec, ':');
   if (colon) {
      if (!set_fs(out, spec, (size_t)(colon - spec))) return false;
      const char *p = colon + 1;
      char tmp[URI_MAX];
      if (*p == '\0') p = "/";
      if (*p != '/') {
         if (snprintf(tmp, sizeof tmp, "/%s", p) >= (int)sizeof tmp) return false;
         p = tmp;
      }
      return fs_norm(out->path, sizeof out->path, p);
   }
   /* 5. relative to the current host */
   if (!ctx || !ctx->fs[0])
      return false;
   memcpy(out->fs, ctx->fs, sizeof out->fs);
   if (spec[0] == '/')
      return fs_norm(out->path, sizeof out->path, spec);
   char joined[URI_MAX];
   return fs_join(joined, sizeof joined, ctx->path, spec) &&
          fs_norm(out->path, sizeof out->path, joined);
}

static bool build_uri(const target_t *t, char *uri, size_t cap)
{
   if (strstr(t->path, "://"))
      return snprintf(uri, cap, "%s", t->path) < (int)cap;
   return snprintf(uri, cap, "%s:%s", t->fs, t->path) < (int)cap;
}

static void build_display(const target_t *t, char *out, size_t cap)
{
   const char *p = t->path;
   const char *scheme = strstr(p, "://");
   if (scheme) {
      const char *slash = strchr(scheme + 3, '/');
      p = slash ? slash : "/";
   } else {
      const char *colon = strchr(p, ':');
      if (colon) p = colon + 1;
   }
   if (!*p) p = "/";
   /* Display only: a path too long for the buffer is shown cut short. */
   if (snprintf(out, cap, "%s%s", p[0] == '/' ? "" : "/", p) >= (int)cap)
      out[cap - 1] = '\0';
}

/* ---- state ---------------------------------------------------------------- */

static bool read_text(const char *key, char *buf, size_t cap)
{
   uint16_t got = 0;
   bool exists = false;
   if (!fn_app_read(HOST_NS, key, 0, (uint8_t *)buf, (uint16_t)(cap - 1), &got, &exists))
      return false;
   buf[got] = '\0';
   return exists && got > 0;
}

static bool write_text(const char *key, const char *text)
{
   (void)fn_app_delete(HOST_NS, key, NULL);
   return fn_app_write(HOST_NS, key, 0, (const uint8_t *)text, (uint16_t)strlen(text));
}

static bool current_ctx(target_t *ctx)
{
   char current[URI_MAX];
   return read_text(KEY_CURRENT, current, sizeof current) &&
          resolve(current, NULL, ctx);
}

static bool resolve_target(const char *spec, char *uri, size_t cap, char *display, size_t dcap)
{
   target_t t, ctx;
   bool must_use_current = spec[0] == '\0' || spec[0] == '/';
   if (!must_use_current && resolve(spec, NULL, &t)) {
      /* resolved on its own */
   } else if (!current_ctx(&ctx) || !resolve(spec, &ctx, &t)) {
      return false;
   }
   if (!build_uri(&t, uri, cap))
      return false;
   if (display)
      build_display(&t, display, dcap);
   return true;
}

bool fn_host_resolve(const char *spec, char *uri, size_t cap)
{
   return resolve_target(spec, uri, cap, NULL, 0);
}

bool fn_uri_split(const char *uri, char *fs, size_t fs_cap, char *path, size_t path_cap)
{
   const char *canon;
   char name[16];
   const char *scheme = strstr(uri, "://");
   const char *colon = strchr(uri, ':');
   if (!colon) return false;
   size_t n = (size_t)((scheme ? scheme : colon) - uri);
   if (n == 0 || n >= sizeof name) return false;
   memcpy(name, uri, n);
   name[n] = '\0';
   if (!fn_store_known_fs(name, &canon)) return false;
   /* A URI filesystem (tnfs) takes the whole URI as its path. */
   const char *p = scheme ? uri : colon + 1;
   return snprintf(fs, fs_cap, "%s", canon) < (int)fs_cap &&
          snprintf(path, path_cap, "%s", p) < (int)path_cap;
}

static char s_history[HISTORY_BYTES + 1];

/* The history, as an array of pointers into s_history (lines split in
   place). */
static unsigned int read_history(const char **entries)
{
   unsigned int n = 0;
   if (!read_text(KEY_HISTORY, s_history, sizeof s_history))
      return 0;
   char *s = s_history;
   while (*s && n < HISTORY_MAX) {
      char *nl = strchr(s, '\n');
      if (nl) *nl = '\0';
      if (*s) entries[n++] = s;
      if (!nl) break;
      s = nl + 1;
   }
   return n;
}

static bool write_history(const char **entries, unsigned int n)
{
   static char text[HISTORY_BYTES + 1];
   size_t o = 0;
   for (unsigned int i = 0; i < n; i++) {
      size_t l = strlen(entries[i]);
      if (o + l + 1 > HISTORY_BYTES) break;
      memcpy(text + o, entries[i], l);
      o += l;
      text[o++] = '\n';
   }
   text[o] = '\0';
   return write_text(KEY_HISTORY, text);
}

static bool set_current(const char *spec)
{
   char uri[URI_MAX], display[URI_MAX], fs[16], path[URI_MAX];
   if (!resolve_target(spec, uri, sizeof uri, display, sizeof display) ||
       !fn_uri_split(uri, fs, sizeof fs, path, sizeof path) ||
       !fn_store_is_dir(fs, path))
      return false;
   if (!write_text(KEY_CURRENT, uri) || !write_text(KEY_DISPLAY, display))
      return false;
   /* Move (or insert) to the top, capped at HISTORY_MAX. */
   const char *entries[HISTORY_MAX + 1];
   unsigned int n = read_history(entries + 1), k = 1;
   entries[0] = uri;
   for (unsigned int i = 1; i <= n; i++)
      if (strcmp(entries[i], uri) != 0)
         entries[k++] = entries[i];
   return write_history(entries, k > HISTORY_MAX ? HISTORY_MAX : k);
}

/* ---- the wire device ------------------------------------------------------ */

uint8_t fn_host_command(uint8_t command, fb_in *in, fb_out *out)
{
   if (fb_get_u8(in) != FB_VERSION)
      return FB_INVALID_REQUEST;

   switch (command) {
   case HOST_GET_CURRENT: {
      char uri[URI_MAX], display[URI_MAX];
      if (in->bad)
         return FB_INVALID_REQUEST;
      if (!read_text(KEY_CURRENT, uri, sizeof uri))
         return FB_DEVICE_NOT_FOUND;
      if (!read_text(KEY_DISPLAY, display, sizeof display))
         display[0] = '\0';
      uint16_t hl = (uint16_t)strlen(uri), dl = (uint16_t)strlen(display);
      fb_put_u8(out, FB_VERSION);
      fb_put_u16(out, hl);
      fb_put_u16(out, dl);
      fb_put_bytes(out, uri, hl);
      fb_put_bytes(out, display, dl);
      return FB_OK;
   }
   case HOST_SET_CURRENT: {
      uint16_t n = fb_get_u16(in);
      const uint8_t *spec = fb_get_bytes(in, n);
      char buf[URI_MAX];
      if (in->bad || n >= sizeof buf)
         return FB_INVALID_REQUEST;
      memcpy(buf, spec, n);
      buf[n] = '\0';
      if (!set_current(buf))
         return FB_IO_ERROR;
      fb_put_u8(out, FB_VERSION);
      return FB_OK;
   }
   case HOST_LIST_HISTORY: {
      uint16_t offset = fb_get_u16(in);
      uint16_t max = fb_get_u16(in);
      if (in->bad || max == 0)
         return FB_INVALID_REQUEST;
      const char *entries[HISTORY_MAX];
      static char text[HISTORY_BYTES + HISTORY_MAX * 4u];
      unsigned int n = read_history(entries);
      size_t o = 0;
      for (unsigned int i = 0; i < n; i++)
         o += (size_t)snprintf(text + o, sizeof text - o, "%u %s\n", i, entries[i]);
      uint16_t len = 0;
      if (offset < o) {
         size_t rest = o - offset;
         len = (uint16_t)(rest < max ? rest : max);
      }
      fb_put_u8(out, FB_VERSION);
      fb_put_u8(out, (uint8_t)(offset + len < o ? 1u : 0u));
      fb_put_u16(out, offset);
      fb_put_u16(out, len);
      fb_put_bytes(out, text + offset, len);
      return FB_OK;
   }
   case HOST_SELECT_HISTORY:
   case HOST_DELETE_HISTORY: {
      uint8_t index = fb_get_u8(in);
      if (in->bad)
         return FB_INVALID_REQUEST;
      const char *entries[HISTORY_MAX];
      unsigned int n = read_history(entries);
      if (index >= HISTORY_MAX || index >= n)
         return FB_IO_ERROR;
      if (command == HOST_SELECT_HISTORY) {
         char uri[URI_MAX];
         snprintf(uri, sizeof uri, "%s", entries[index]);
         if (!set_current(uri))
            return FB_IO_ERROR;
      } else {
         for (unsigned int i = index; i + 1 < n; i++)
            entries[i] = entries[i + 1];
         if (!write_history(entries, n - 1))
            return FB_IO_ERROR;
      }
      fb_put_u8(out, FB_VERSION);
      return FB_OK;
   }
   default:
      return FB_UNSUPPORTED;
   }
}
