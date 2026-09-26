/* fn_tnfs.c - disk images and directories on TNFS servers.  See fn_tnfs.h.

   One job runs at a time.  A storage call names an operation (a key: its
   kind and arguments); if the operation already finished during this
   request its remembered result is returned, if it is the job in flight
   the call is "not yet", and otherwise it becomes the job.  The job walks
   the TNFS requests it needs - resolve and mount the server once per
   session, then OPEN, STAT, LSEEK+READ, LSEEK+WRITE, or OPENDIR/READDIR/
   CLOSEDIR plus a STAT per entry - driven by replies (fn_tnfs_input) and
   time (fn_tnfs_poll), and stores its result when it ends. */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fn_tnfs.h"
#include "../net_tnfs.h"

#define SESSIONS      2u
#define HOST_MAX      64u
#define DONE_MAX      16u           /* operations one request can remember */
#define IO_MAX        512u          /* the largest read or write in one call */
#define DIR_ENTRIES   256u
#define DIR_NAMES     (16u * 1024u)
#define DIR_TTL_MS    10000u        /* a listing serves its pages this long */
#define URI_MAX       FN_STORE_MAX_PATH

/* ---- servers and open files ---------------------------------------------- */

typedef struct {
   bool     used;
   char     host[HOST_MAX];
   uint16_t port;
   uint32_t ip;
   bool     resolved;
   bool     mounted;
   uint16_t connid;
   uint16_t retry_ms;
   uint8_t  seq;
} session_t;

typedef struct {
   bool     used;
   unsigned sess;
   uint8_t  fd;
   char     path[URI_MAX];          /* on the server */
   uint32_t pos;                    /* the server's file position, if known */
   bool     pos_known;
} tfile_t;

static session_t s_sess[SESSIONS];
static tfile_t   s_file[FN_TNFS_HANDLES];

/* "tnfs://host[:port][/path]" -> host, port, path ("/" if none). */
static bool parse_uri(const char *uri, char *host, uint16_t *port, char *path)
{
   if (strncasecmp(uri, "tnfs://", 7) != 0)
      return false;                 /* tnfs+tcp:// etc.: no TCP transport here */
   const char *h = uri + 7;
   const char *slash = strchr(h, '/');
   size_t hl = slash ? (size_t)(slash - h) : strlen(h);
   const char *colon = memchr(h, ':', hl);
   size_t name = colon ? (size_t)(colon - h) : hl;
   if (name == 0 || name >= HOST_MAX)
      return false;
   memcpy(host, h, name);
   host[name] = '\0';
   *port = TNFS_PORT;
   if (colon) {
      unsigned long v = 0;
      for (const char *p = colon + 1; p < h + hl; p++) {
         if (*p < '0' || *p > '9') return false;
         v = v * 10u + (unsigned long)(*p - '0');
         if (v > 65535u) return false;
      }
      if (v == 0) return false;
      *port = (uint16_t)v;
   }
   snprintf(path, URI_MAX, "%s", slash ? slash : "/");
   return true;
}

static int session_for(const char *host, uint16_t port)
{
   int free_slot = -1;
   for (unsigned i = 0; i < SESSIONS; i++) {
      if (s_sess[i].used && s_sess[i].port == port && strcasecmp(s_sess[i].host, host) == 0)
         return (int)i;
      if (!s_sess[i].used && free_slot < 0)
         free_slot = (int)i;
   }
   if (free_slot < 0) {
      /* Recycle the one with no open files. */
      for (unsigned i = 0; i < SESSIONS && free_slot < 0; i++) {
         bool busy = false;
         for (unsigned f = 0; f < FN_TNFS_HANDLES; f++)
            busy |= s_file[f].used && s_file[f].sess == i;
         if (!busy) free_slot = (int)i;
      }
      if (free_slot < 0)
         return -1;
   }
   session_t *s = &s_sess[free_slot];
   memset(s, 0, sizeof *s);
   s->used = true;
   snprintf(s->host, sizeof s->host, "%s", host);
   s->port = port;
   s->retry_ms = TNFS_TIMEOUT_MS;
   return free_slot;
}

static tfile_t *file_of(fn_handle h)
{
   int i = h - FN_TNFS_HANDLE_BASE;
   return (i >= 0 && i < (int)FN_TNFS_HANDLES && s_file[i].used) ? &s_file[i] : NULL;
}

/* ---- operations, their results, and the job ------------------------------ */

typedef enum { OP_OPEN = 1, OP_SIZE, OP_READ, OP_WRITE, OP_ISDIR, OP_LIST } op_kind;

typedef struct {
   op_kind  kind;
   int      h;
   uint32_t off, len, sum;
   uint8_t  mode;
   char     uri[URI_MAX];
} op_key;

typedef struct {
   bool     used;
   op_key   key;
   bool     ok;
   int      handle;                 /* OPEN */
   uint32_t value;                  /* SIZE, ISDIR */
   uint8_t  data[IO_MAX];           /* READ */
} op_done;

typedef enum {
   ST_RESOLVE, ST_MOUNT, ST_OPEN, ST_STAT, ST_LSEEK, ST_READ, ST_WRITE,
   ST_OPENDIR, ST_READDIR, ST_CLOSEDIR, ST_STAT_ENTRY
} step_t;

static op_done s_done[DONE_MAX];

static struct {
   bool     active;
   op_key   key;
   step_t   step;
   unsigned sess;
   char     path[URI_MAX];          /* server path for OPEN/STAT/LIST */
   uint8_t  fd;                     /* file or directory handle on the server */
   uint32_t n;                      /* bytes moved / entries statted so far */
   uint8_t  buf[IO_MAX];            /* READ accumulates here, WRITE sends from here */
} J;

static tnfs_xfer_t X;
static bool s_pending;
static uint32_t s_now;

/* The last directory listed, serving its pages. */
static struct {
   bool     valid;
   char     uri[URI_MAX];
   uint32_t when;
   unsigned n;
   struct { uint32_t name; bool is_dir; uint32_t size, mtime; } e[DIR_ENTRIES];
   char     names[DIR_NAMES];
   uint32_t pool;
} D;

static uint32_t fnv(const void *p, uint32_t n)
{
   const uint8_t *b = p;
   uint32_t h = 2166136261u;
   while (n--) h = (h ^ *b++) * 16777619u;
   return h;
}

static bool key_eq(const op_key *a, const op_key *b)
{
   return a->kind == b->kind && a->h == b->h && a->off == b->off && a->len == b->len &&
          a->sum == b->sum && a->mode == b->mode && strcmp(a->uri, b->uri) == 0;
}

static op_done *find_done(const op_key *k)
{
   for (unsigned i = 0; i < DONE_MAX; i++)
      if (s_done[i].used && key_eq(&s_done[i].key, k))
         return &s_done[i];
   return NULL;
}

static op_done *free_done(void)
{
   for (unsigned i = 0; i < DONE_MAX; i++)
      if (!s_done[i].used) return &s_done[i];
   return NULL;
}

static void finish(bool ok, int handle, uint32_t value)
{
   op_done *d = free_done();
   J.active = false;
   if (!d)
      return;                       /* run() never starts a job without room */
   memset(d, 0, sizeof *d);
   d->used = true;
   d->key = J.key;
   d->ok = ok;
   d->handle = handle;
   d->value = value;
   if (J.key.kind == OP_READ && ok)
      memcpy(d->data, J.buf, J.key.len);
}

static void fail_session(void)
{
   /* A lost or refused exchange may mean the server forgot us: mount again
      next time.  Open files on it are kept - their fds may still be good,
      and a stale one fails on its own. */
   s_sess[J.sess].mounted = false;
   finish(false, -1, 0);
}

/* Build-and-send helpers: each fills X.req for the session and sends it. */
static void send_built(size_t len)
{
   session_t *s = &s_sess[J.sess];
   if (len == 0) {
      finish(false, -1, 0);
      return;
   }
   tnfs_xfer_begin(&X, (uint16_t)len, s_now);
   if (!fn_tnfs_io_send(s->ip, s->port, X.req, X.req_len))
      finish(false, -1, 0);
}

static void next_seq(void)
{
   session_t *s = &s_sess[J.sess];
   X.seq = ++s->seq;
   X.connid = s->connid;
   X.retry_ms = s->retry_ms;
}

static void send_step(step_t step)
{
   session_t *s = &s_sess[J.sess];
   tfile_t *f = file_of(J.key.h);
   size_t n = 0;
   if (!f && (step == ST_LSEEK || step == ST_READ || step == ST_WRITE)) {
      finish(false, -1, 0);
      return;
   }
   J.step = step;
   next_seq();
   switch (step) {
   case ST_MOUNT:
      X.connid = 0;
      n = tnfs_build_mount(X.req, sizeof X.req, X.seq, "/", NULL, NULL);
      break;
   case ST_OPEN: {
      static const uint16_t flags[] = {
         [FN_OPEN_READ]       = TNFS_O_RDONLY,
         [FN_OPEN_UPDATE]     = TNFS_O_RDWR,
         [FN_OPEN_CREATE]     = TNFS_O_RDWR | TNFS_O_CREAT | TNFS_O_TRUNC,
         [FN_OPEN_CREATE_NEW] = TNFS_O_RDWR | TNFS_O_CREAT | TNFS_O_EXCL,
      };
      n = tnfs_build_open(X.req, sizeof X.req, s->connid, X.seq, flags[J.key.mode],
                          0644u, J.path);
      break;
   }
   case ST_STAT:
      n = tnfs_build_stat(X.req, sizeof X.req, s->connid, X.seq, J.path);
      break;
   case ST_LSEEK:
      n = tnfs_build_lseek(X.req, sizeof X.req, s->connid, X.seq, f->fd, TNFS_SEEK_SET,
                           (int32_t)J.key.off);
      break;
   case ST_READ: {
      uint32_t want = J.key.len - J.n;
      n = tnfs_build_read(X.req, sizeof X.req, s->connid, X.seq, f->fd,
                          (uint16_t)(want > IO_MAX ? IO_MAX : want));
      break;
   }
   case ST_WRITE:
      n = tnfs_build_write(X.req, sizeof X.req, s->connid, X.seq, f->fd,
                           J.buf + J.n, (uint16_t)(J.key.len - J.n));
      break;
   case ST_OPENDIR:
      n = tnfs_build_opendir(X.req, sizeof X.req, s->connid, X.seq, J.path);
      break;
   case ST_READDIR:
      n = tnfs_build_readdir(X.req, sizeof X.req, s->connid, X.seq, J.fd);
      break;
   case ST_CLOSEDIR:
      n = tnfs_build_closedir(X.req, sizeof X.req, s->connid, X.seq, J.fd);
      break;
   case ST_STAT_ENTRY: {
      char p[URI_MAX];
      const char *name = D.names + D.e[J.n].name;
      size_t pl = strlen(J.path);
      if (snprintf(p, sizeof p, "%s%s%s", J.path, (pl && J.path[pl - 1] == '/') ? "" : "/",
                   name) >= (int)sizeof p) {
         finish(false, -1, 0);
         return;
      }
      n = tnfs_build_stat(X.req, sizeof X.req, s->connid, X.seq, p);
      break;
   }
   case ST_RESOLVE:
      return;
   }
   send_built(n);
}

/* The first request of the job once the session is mounted. */
static void first_request(void)
{
   tfile_t *f;
   switch (J.key.kind) {
   case OP_OPEN:  send_step(ST_OPEN); break;
   case OP_SIZE:
   case OP_ISDIR: send_step(ST_STAT); break;
   case OP_LIST:  D.valid = false; D.n = 0; D.pool = 0; send_step(ST_OPENDIR); break;
   case OP_READ:
   case OP_WRITE:
      f = file_of(J.key.h);
      if (!f) {                     /* closed while its job waited */
         finish(false, -1, 0);
         break;
      }
      send_step(f->pos_known && f->pos == J.key.off
                ? (J.key.kind == OP_READ ? ST_READ : ST_WRITE) : ST_LSEEK);
      break;
   }
}

/* Get the session resolved and mounted, then run the job's first request. */
static void run_session(void)
{
   session_t *s = &s_sess[J.sess];
   if (!s->resolved) {
      J.step = ST_RESOLVE;
      fn_io r = fn_tnfs_io_resolve(s->host, &s->ip);
      if (r == FN_IO_PENDING) return;
      if (r == FN_IO_FAIL) { finish(false, -1, 0); return; }
      s->resolved = true;
   }
   if (!s->mounted) {
      send_step(ST_MOUNT);
      return;
   }
   first_request();
}

static void on_reply(const tnfs_reply_t *rep)
{
   session_t *s = &s_sess[J.sess];
   tfile_t *f = file_of(J.key.h);
   uint16_t mode = 0;
   uint32_t size = 0;
   if (!f && (J.step == ST_LSEEK || J.step == ST_READ || J.step == ST_WRITE)) {
      finish(false, -1, 0);         /* its file was closed while it waited */
      return;
   }
   switch (J.step) {
   case ST_MOUNT: {
      uint16_t ver, retry;
      if (!tnfs_reply_mount(rep, &ver, &retry)) { finish(false, -1, 0); return; }
      s->connid = rep->connid;
      s->retry_ms = retry ? retry : TNFS_TIMEOUT_MS;
      s->mounted = true;
      first_request();
      return;
   }
   case ST_OPEN: {
      /* Handles are handed out in rotation, not lowest-free: a request that
         closes a handle and opens another is re-run from the top, and its
         repeated close of the old number must not hit the new file. */
      static unsigned next;
      uint8_t fd;
      int slot = -1;
      if (!tnfs_reply_open(rep, &fd)) { finish(false, -1, 0); return; }
      for (unsigned k = 0; k < FN_TNFS_HANDLES && slot < 0; k++) {
         unsigned i = (next + k) % FN_TNFS_HANDLES;
         if (!s_file[i].used) slot = (int)i;
      }
      if (slot < 0) { finish(false, -1, 0); return; }
      next = (unsigned)slot + 1u;
      s_file[slot] = (tfile_t){ .used = true, .sess = J.sess, .fd = fd, .pos = 0, .pos_known = true };
      snprintf(s_file[slot].path, sizeof s_file[slot].path, "%s", J.path);
      finish(true, FN_TNFS_HANDLE_BASE + slot, 0);
      return;
   }
   case ST_STAT:
      if (J.key.kind == OP_SIZE) {
         bool ok = tnfs_reply_stat_size(rep, &size);
         finish(ok, -1, size);
      } else {
         /* Not found is an answer ("not a directory"), not a failure. */
         bool ok = tnfs_reply_stat_mode(rep, &mode);
         finish(true, -1, ok && (mode & TNFS_S_IFMT) == TNFS_S_IFDIR);
      }
      return;
   case ST_LSEEK:
      if (rep->status != TNFS_OK) { f->pos_known = false; finish(false, -1, 0); return; }
      f->pos = J.key.off;
      f->pos_known = true;
      send_step(J.key.kind == OP_READ ? ST_READ : ST_WRITE);
      return;
   case ST_READ: {
      const uint8_t *data;
      uint16_t got;
      if (!tnfs_reply_read(rep, &data, &got) || got == 0 || got > J.key.len - J.n) {
         f->pos_known = false;
         finish(false, -1, 0);      /* EOF or error: the caller asked for bytes that exist */
         return;
      }
      memcpy(J.buf + J.n, data, got);
      J.n += got;
      f->pos += got;
      if (J.n < J.key.len) send_step(ST_READ);
      else finish(true, -1, 0);
      return;
   }
   case ST_WRITE: {
      uint16_t wrote;
      if (!tnfs_reply_write(rep, &wrote) || wrote == 0 || wrote > J.key.len - J.n) {
         f->pos_known = false;
         finish(false, -1, 0);
         return;
      }
      J.n += wrote;
      f->pos += wrote;
      if (J.n < J.key.len) send_step(ST_WRITE);
      else finish(true, -1, 0);
      return;
   }
   case ST_OPENDIR:
      if (!tnfs_reply_opendir(rep, &J.fd)) { finish(false, -1, 0); return; }
      send_step(ST_READDIR);
      return;
   case ST_READDIR: {
      const char *name;
      if (rep->status == TNFS_EOF) { send_step(ST_CLOSEDIR); return; }
      if (!tnfs_reply_readdir(rep, &name)) { finish(false, -1, 0); return; }
      size_t nl = strlen(name) + 1;
      if (strcmp(name, ".") && strcmp(name, "..") && D.n < DIR_ENTRIES &&
          D.pool + nl <= DIR_NAMES) {
         memcpy(D.names + D.pool, name, nl);
         D.e[D.n].name = D.pool;
         D.e[D.n].is_dir = false;
         D.e[D.n].size = 0;
         D.e[D.n].mtime = 0;
         D.pool += (uint32_t)nl;
         D.n++;
      }
      send_step(ST_READDIR);
      return;
   }
   case ST_CLOSEDIR:
      J.n = 0;
      if (D.n) send_step(ST_STAT_ENTRY);
      else goto listed;
      return;
   case ST_STAT_ENTRY:
      /* body: mode(2) uid(2) gid(2) size(4) atime(4) mtime(4) ctime(4) */
      if (tnfs_reply_stat_mode(rep, &mode))
         D.e[J.n].is_dir = (mode & TNFS_S_IFMT) == TNFS_S_IFDIR;
      if (tnfs_reply_stat_size(rep, &size))
         D.e[J.n].size = D.e[J.n].is_dir ? 0 : size;
      if (rep->body_len >= 18u)
         D.e[J.n].mtime = (uint32_t)rep->body[14] | ((uint32_t)rep->body[15] << 8) |
                          ((uint32_t)rep->body[16] << 16) | ((uint32_t)rep->body[17] << 24);
      if (++J.n < D.n) { send_step(ST_STAT_ENTRY); return; }
   listed:
      snprintf(D.uri, sizeof D.uri, "%s", J.key.uri);
      D.when = s_now;
      D.valid = true;
      finish(true, -1, 0);
      return;
   case ST_RESOLVE:
      return;
   }
}

/* Look the operation up; start it if nothing is running.  Returns its
   result, or NULL with the pending flag set.  A WRITE's data is copied into
   the job before anything is sent: the caller's buffer need not last. */
static op_done *run(const op_key *k, const void *data)
{
   static op_done refused;          /* ok = false */
   op_done *d = find_done(k);
   if (d)
      return d;
   /* A file operation needs its file open - checked only now: a re-run is
      handed the remembered results of calls on a handle it has since
      closed, and must get past them. */
   if ((k->kind == OP_SIZE || k->kind == OP_READ || k->kind == OP_WRITE) && !file_of(k->h))
      return &refused;
   if (J.active) {
      s_pending = true;
      return NULL;                  /* this, or a leftover, is in flight */
   }
   /* A request needing more operations than can be remembered would restart
      for ever: it fails instead. */
   if (!free_done())
      return &refused;
   s_pending = true;
   memset(&J, 0, sizeof J);
   J.active = true;
   J.key = *k;
   if (data)
      memcpy(J.buf, data, k->len);
   s_now = fn_tnfs_io_now_ms();
   char host[HOST_MAX];
   uint16_t port;
   if (k->kind == OP_SIZE || k->kind == OP_READ || k->kind == OP_WRITE) {
      tfile_t *f = file_of(k->h);   /* checked above; the compiler cannot see it */
      if (!f) {
         J.active = false;
         s_pending = false;
         return &refused;
      }
      J.sess = f->sess;
      snprintf(J.path, sizeof J.path, "%s", f->path);
   } else {
      int sess;
      if (!parse_uri(k->uri, host, &port, J.path) || (sess = session_for(host, port)) < 0) {
         finish(false, -1, 0);
         s_pending = false;
         return find_done(k);
      }
      J.sess = (unsigned)sess;
   }
   run_session();
   /* A job can end without the network (a failed build or resolve). */
   d = J.active ? NULL : find_done(k);
   if (d)
      s_pending = false;
   return d;
}

/* ---- the backend --------------------------------------------------------- */

fn_handle fn_tnfs_open(const char *uri, fn_open_mode mode)
{
   op_key k = { .kind = OP_OPEN, .h = -1, .mode = (uint8_t)mode };
   snprintf(k.uri, sizeof k.uri, "%s", uri);
   op_done *d = run(&k, NULL);
   return (d && d->ok) ? d->handle : FN_NO_HANDLE;
}

bool fn_tnfs_size(fn_handle h, uint32_t *size)
{
   op_key k = { .kind = OP_SIZE, .h = h };
   op_done *d = run(&k, NULL);
   if (!d || !d->ok)
      return false;
   *size = d->value;
   return true;
}

bool fn_tnfs_read(fn_handle h, uint32_t offset, void *buf, uint32_t len)
{
   if (len == 0 || len > IO_MAX)
      return false;
   op_key k = { .kind = OP_READ, .h = h, .off = offset, .len = len };
   op_done *d = run(&k, NULL);
   if (!d || !d->ok)
      return false;
   memcpy(buf, d->data, len);
   return true;
}

bool fn_tnfs_write(fn_handle h, uint32_t offset, const void *buf, uint32_t len)
{
   if (len == 0 || len > IO_MAX)
      return false;
   op_key k = { .kind = OP_WRITE, .h = h, .off = offset, .len = len, .sum = fnv(buf, len) };
   op_done *d = run(&k, buf);
   return d && d->ok;
}

void fn_tnfs_close(fn_handle h)
{
   tfile_t *f = file_of(h);
   if (!f)
      return;
   /* Fire and forget: nothing waits for a CLOSE, and its reply is ignored
      by sequence. */
   session_t *s = &s_sess[f->sess];
   uint8_t pkt[16];
   size_t n = tnfs_build_close(pkt, sizeof pkt, s->connid, ++s->seq, f->fd);
   if (n && s->mounted)
      (void)fn_tnfs_io_send(s->ip, s->port, pkt, (uint16_t)n);
   f->used = false;
}

bool fn_tnfs_is_dir(const char *uri)
{
   op_key k = { .kind = OP_ISDIR, .h = -1 };
   snprintf(k.uri, sizeof k.uri, "%s", uri);
   op_done *d = run(&k, NULL);
   return d && d->ok && d->value;
}

bool fn_tnfs_dir_entry(const char *uri, uint32_t index, fn_dirent *out)
{
   s_now = fn_tnfs_io_now_ms();
   bool fresh = D.valid && strcmp(D.uri, uri) == 0 && s_now - D.when < DIR_TTL_MS;
   if (!fresh) {
      op_key k = { .kind = OP_LIST, .h = -1 };
      snprintf(k.uri, sizeof k.uri, "%s", uri);
      op_done *d = run(&k, NULL);
      if (!d || !d->ok || !D.valid)
         return false;
   }
   if (index >= D.n)
      return false;
   snprintf(out->name, sizeof out->name, "%s", D.names + D.e[index].name);
   out->is_dir = D.e[index].is_dir;
   out->size = D.e[index].size;
   out->mtime = D.e[index].mtime;
   return true;
}

/* ---- lifetime, time and input ------------------------------------------- */

bool fn_tnfs_take_pending(void)
{
   bool p = s_pending;
   s_pending = false;
   return p;
}

void fn_tnfs_request_end(void)
{
   memset(s_done, 0, sizeof s_done);
}

void fn_tnfs_request_abort(void)
{
   /* The Beeb gave up and asked something else: drop the job (a late reply
      is ignored by sequence) and whatever the old request had gathered. */
   if (J.active && J.key.kind != OP_OPEN) {
      tfile_t *f = file_of(J.key.h);
      if (f) f->pos_known = false;
   }
   J.active = false;
   fn_tnfs_request_end();
}

void fn_tnfs_poll(uint32_t now_ms)
{
   s_now = now_ms;
   if (!J.active)
      return;
   if (J.step == ST_RESOLVE) {
      run_session();
      return;
   }
   session_t *s = &s_sess[J.sess];
   switch (tnfs_xfer_tick(&X, now_ms)) {
   case TNFS_X_SEND:
      (void)fn_tnfs_io_send(s->ip, s->port, X.req, X.req_len);
      break;
   case TNFS_X_FAIL:
      fail_session();
      break;
   default:
      break;
   }
}

void fn_tnfs_input(uint32_t ip, uint16_t port, const uint8_t *pkt, uint16_t len)
{
   if (!J.active || J.step == ST_RESOLVE)
      return;
   session_t *s = &s_sess[J.sess];
   if (ip != s->ip || port != s->port)
      return;
   s_now = fn_tnfs_io_now_ms();
   tnfs_reply_t rep;
   switch (tnfs_xfer_reply(&X, pkt, len, s_now, &rep)) {
   case TNFS_X_DONE: on_reply(&rep); break;
   case TNFS_X_FAIL: fail_session(); break;
   default:          break;
   }
}
