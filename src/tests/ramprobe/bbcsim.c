/* bbcsim.c - the /bbc web API on an emulated BBC, for what runs on another computer.

   The real ramprobe.c, ramprobe_api.c, 6502 code and services_emulator.c on kc-beeb, as in
   test_ramprobe.c, served over real HTTP and paced to real time.  Not the firmware's web server:
   only its route into ramprobe_api().  Built by run_bbcsim.sh.

     bbcsim ROMS PORT

   Besides /bbc, what a person does at the machine:
     POST /sim/type?text=PRINT+1%0D   keys: capitals, digits and a little punctuation
     GET  /sim/screen                 the MODE 7 screen, as text
     POST /sim/quit */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BOARD_EXTRA
#include "test_ramprobe.c"

static const char *board_config(const char *key) { (void)key; return NULL; }
static void board_extra_init(void) {}

static bool quitting;

/* %XX and + in a query value. */
static void decode(const char *in, size_t len, char *out, size_t n)
{
   size_t o = 0;
   for (size_t i = 0; i < len && o + 1 < n; i++) {
      if (in[i] == '+') out[o++] = ' ';
      else if (in[i] == '%' && i + 2 < len) {
         char h[3] = { in[i + 1], in[i + 2], 0 };
         out[o++] = (char)strtol(h, NULL, 16);
         i += 2;
      } else out[o++] = in[i];
   }
   out[o] = 0;
}

static bool sim_value(const char *query, const char *key, char *out, size_t n)
{
   size_t klen = strlen(key);
   for (const char *p = query; p && *p; ) {
      const char *end = strchr(p, '&');
      size_t len = end ? (size_t)(end - p) : strlen(p);
      if (len > klen && p[klen] == '=' && !strncmp(p, key, klen)) {
         decode(p + klen + 1, len - klen - 1, out, n);
         return true;
      }
      p = end ? end + 1 : NULL;
   }
   return false;
}

static bool typeable(const char *s)
{
   for (; *s; s++)
      if (!((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr(" \r:/?%&=*+", *s)))
         return false;
   return true;
}

static void screen_text(char *out, size_t n)
{
   uint16_t a = m->ram[0x350] | (m->ram[0x351] << 8);
   size_t o = 0;
   for (int r = 0; r < 25; r++) {
      for (int c = 0; c < 40 && o + 2 < n; c++) {
         uint8_t v = m->ram[0x7C00 + ((a - 0x7C00 + r * 40 + c) & 0x3FF)] & 0x7F;
         out[o++] = v >= 32 && v < 127 ? (char)v : ' ';
      }
      if (o + 2 < n) out[o++] = '\n';
   }
   out[o] = 0;
}

static int sim_route(bool post, const char *path, const char *query, char *out, size_t n)
{
   static char text[256];
   if (!post && !strcmp(path, "/sim/screen")) { screen_text(out, n); return 200; }
   if (post && !strcmp(path, "/sim/type")) {
      if (!sim_value(query, "text", text, sizeof text) || !typeable(text)) {
         snprintf(out, n, "{\"error\":\"not typeable\"}\n");
         return 400;
      }
      type(text);
      run_ms(100);
      snprintf(out, n, "{}\n");
      return 200;
   }
   if (post && !strcmp(path, "/sim/quit")) { quitting = true; snprintf(out, n, "{}\n"); return 200; }
   return 0;
}

static void answer(int fd)
{
   static char req[4096], out[RAMPROBE_API_OUT + 2048];
   size_t got = 0;
   req[0] = 0;
   while (got + 1 < sizeof req && !strstr(req, "\r\n\r\n")) {
      ssize_t r = read(fd, req + got, sizeof req - 1 - got);
      if (r <= 0) return;
      got += (size_t)r;
      req[got] = 0;
   }
   bool post = !strncmp(req, "POST ", 5);
   static uint8_t body[RAMPROBE_API_BODY_MAX];
   long blen = post ? board_read_body(fd, req, got, body, sizeof body) : -1;   /* before req is cut up */
   char *sp1 = strchr(req, ' ');
   char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
   if (!sp2) return;
   *sp2 = 0;
   char *path = sp1 + 1;
   char *query = strchr(path, '?');
   if (query) *query++ = 0;

   int status = !strncmp(path, "/sim/", 5) ? sim_route(post, path, query, out, sizeof out)
              : blen > 0 ? ramprobe_api_body(path, query, body, (size_t)blen, out, sizeof out)
                         : ramprobe_api(post, path, query, out, sizeof out);
   if (status == 0) { snprintf(out, sizeof out, "No such page.\n"); status = 404; }
   if (ramprobe_api_hold(post, query, status)) {      /* wait=1: answer when it's over */
      while (!ramprobe_api_done()) run_ms(1);
      status = ramprobe_api_status(out, sizeof out);
   }

   char head[128];
   int h = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                    status, status == 200 ? "OK" : "Error", strlen(out));
   if (write(fd, head, (size_t)h) < 0 || write(fd, out, strlen(out)) < 0) perror("write");
}

static double wall_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
   if (argc < 3) { fprintf(stderr, "usage: bbcsim ROMS PORT\n"); return 2; }
   roms = argv[1];

   int s = socket(AF_INET, SOCK_STREAM, 0);
   int yes = 1;
   setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
   struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2])), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
   if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) { perror("bind"); return 2; }
   fcntl(s, F_SETFL, O_NONBLOCK);

   boot();
   to_prompt();
   puts("ready");
   fflush(stdout);

   double start = wall_ms(), emulated = 0;
   while (!quitting) {
      run_ms(1);
      emulated += 1;
      int fd = accept(s, NULL, NULL);
      if (fd >= 0) {
         int flags = fcntl(fd, F_GETFL);
         fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
         answer(fd);
         close(fd);
      }
      double ahead = emulated - (wall_ms() - start);
      if (ahead > 2) {
         struct timespec nap = { 0, (long)(ahead * 1e6) };
         nanosleep(&nap, NULL);
      } else if (ahead < -1000) {       /* typing runs unpaced: don't race to catch up */
         start = wall_ms() - emulated;
      }
   }
   return 0;
}
