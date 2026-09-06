/* Drive the real src/uef_stream.c over real UEF files and compare the bytes
   it emits with what gunzip produces.  Also exercises rewind, and reports the
   window size the tape was served through - the point being that it is served
   in windows at all, and that the cost does not grow with the tape. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uef_stream.h"

static uint8_t  file_data[8u * 1024u * 1024u];   /* the test harness may slurp; the module may not */
static uint32_t file_size;

/* Stand-in for the Beeb's upload buffer or an SD file: random access reads,
   which is all uef_stream asks of its source. */
static size_t file_source(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
{
   (void)ctx;
   if (offset >= file_size) return 0;
   if (len > file_size - offset) len = file_size - offset;
   memcpy(buf, file_data + offset, len);
   return len;
}

static uef_stream_t stream;
static uint8_t window[4096];

static long drain(FILE *out)
{
   long total = 0;
   for (;;) {
      size_t n = uef_stream_read(&stream, window, sizeof window);
      if (n == 0) break;
      if (out) fwrite(window, 1, n, out);
      total += (long)n;
   }
   return total;
}

int main(int argc, char **argv)
{
   FILE *fp, *out;
   long first, second;
   const char *name;
   uef_format_t fmt;
   static const char *fmtname[] = { "RAW", "GZIP", "ZIP", "INVALID" };

   if (argc < 3) { fprintf(stderr, "usage: test_uef <in.uef> <out.bin>\n"); return 2; }
   name = argv[1];
   fp = fopen(name, "rb");
   if (!fp) { perror(name); return 2; }
   file_size = (uint32_t)fread(file_data, 1, sizeof file_data, fp);
   fclose(fp);

   fmt = uef_stream_open(&stream, file_source, NULL, file_size);
   if (fmt == UEF_FORMAT_INVALID) { printf("  %-44s INVALID\n", name); return 1; }

   out = fopen(argv[2], "wb");
   if (!out) { perror(argv[2]); return 2; }
   first = drain(out);
   fclose(out);

   if (uef_stream_failed(&stream)) { printf("  %-44s FAILED mid-stream\n", name); return 1; }

   /* Rewind and drain again: the second pass must produce the same length,
      which is what the Beeb does when it rewinds the tape. */
   if (!uef_stream_rewind(&stream)) { printf("  %-44s rewind refused\n", name); return 1; }
   second = drain(NULL);

   printf("  %-44s %-5s %7ld bytes  rewind=%s  verified=%s  resident=%u\n",
          name, fmtname[fmt], first,
          second == first ? "same" : "DIFFERENT",
          uef_stream_verified(&stream) ? "yes" : "no",
          (unsigned)sizeof(uef_stream_t));
   return (second == first) ? 0 : 1;
}
