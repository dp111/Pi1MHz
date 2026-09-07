#include "uef_service.h"

#include <stdlib.h>
#include <string.h>

#include "Pi1MHz.h"
#include "uef_stream.h"
#include "byteorder.h"
#include "uzlib/uzlib.h"
#include "wifi_service.h"

/* JIM layout.  The AP5 exposes one 64K JIM aperture and the tape is published
 * into it a window at a time; the Pi keeps the authoritative stream, which is
 * also what protects a loaded tape from filing systems and other expansions
 * sharing JIM.  Absolute JIM offset zero, not the services mailbox DISC_RAM. */
#define UEF_BASE            0u
#define UEF_FIRST_PAGE      1u   /* page 0 is the service reply buffer */
/* The flat window reserves page 0 for the reply buffer, which OSWORD &65
 * clients read in full, and the last page for the length trailer. */
#define UEF_FLAT_WINDOW     ((256u - UEF_FIRST_PAGE - 1u) * 256u)
/* A published guard occupies the top of every page, so the stream gives those
 * bytes up and is laid out in short runs instead.  The guard ends exactly at
 * the page top, which is what leaves the length trailer intact. */
#define UEF_GUARD_OFFSET    0x97u
#define UEF_GUARD_LENGTH    (256u - UEF_GUARD_OFFSET)
#define UEF_GUARD_WINDOW    ((256u - UEF_FIRST_PAGE - 1u) * UEF_GUARD_OFFSET)
#define UEF_TRAILER         (UEF_BASE + 0xfffeu)
#define UEF_APPEND_MAX      0xff00u   /* most the host sends in one APPEND */
#define UEF_LEGACY_CAPACITY 0xfffeu   /* the one-shot path's JIM window     */
#define UEF_STREAM_VERSION  1u

/* An upload cap has to exist because APPEND is open ended.  Real tapes are
 * tiny - the largest in the b-em and beebjit corpora is 84 KB decompressed,
 * 69 KB compressed - so a megabyte is twelve times the worst real case and
 * still a thirty-second of what the original static buffers cost. */
#define UEF_UPLOAD_MAX      (1024u * 1024u)
#define UEF_UPLOAD_GROW     (64u * 1024u)

#define UEF_OP_PROBE        0u
#define UEF_OP_BEGIN        1u
#define UEF_OP_APPEND       2u
#define UEF_OP_FINALIZE     3u
#define UEF_OP_REWIND       4u
#define UEF_OP_REFILL       5u
#define UEF_OP_CLOSE        6u
#define UEF_OP_REPUBLISH    7u

/* Everything an open tape owns.  Allocated on BEGIN, freed on CLOSE, so the
 * idle cost of the whole feature is this one pointer. */
typedef struct {
   uef_stream_t stream;              /* the resumable inflater, 34 KB      */
   uint8_t     *upload;              /* the compressed image as uploaded   */
   uint32_t     upload_length;
   uint32_t     upload_capacity;
   /* The bytes currently published.  Kept because REPUBLISH and a guard
    * change both have to lay the same window down again, and a stream
    * cannot seek backwards to recover them. */
   uint8_t     *window;
   uint16_t     window_length;
   bool         window_final;
   bool         ready;               /* FINALIZE done, serving windows     */
   bool         uploading;           /* between BEGIN and FINALIZE         */
   uint8_t      format;              /* 'R', 'G' or 'Z' for the host       */
   uint32_t     token;
   uint32_t     generation;
   uint32_t     last_append_offset;
   uint32_t     last_append_crc;
   uint16_t     last_append_length;
} uef_tape_t;

static uef_tape_t *tape;
static uint8_t guard_image[UEF_GUARD_LENGTH];
static bool guard_image_valid;
/* The token survives a tape being closed so a host that reopens gets a fresh
 * one and cannot mistake a stale window for its own. */
static uint32_t next_token;

extern void response_string(uint32_t cp, const char *value);



/* The last two bytes of the aperture tell the host how much of the window is
 * live.  The host also writes it, to declare an APPEND length. */
static void public_length_set(size_t length)
{
   Pi1MHz->JIM_ram[UEF_TRAILER] = (uint8_t)length;
   Pi1MHz->JIM_ram[UEF_TRAILER + 1u] = (uint8_t)(length >> 8);
}

static size_t public_length_get(void)
{
   return (size_t)Pi1MHz->JIM_ram[UEF_TRAILER]
        | ((size_t)Pi1MHz->JIM_ram[UEF_TRAILER + 1u] << 8);
}

static void tape_free(void)
{
   if (tape == NULL)
      return;
   free(tape->upload);
   free(tape->window);
   free(tape);
   tape = NULL;
}

static bool tape_alloc(void)
{
   tape_free();
   tape = calloc(1u, sizeof *tape);
   if (tape == NULL)
      return false;
   /* The window doubles as the one-shot path's scratch, so size it to the
    * larger of the two uses. */
   tape->window = malloc(UEF_LEGACY_CAPACITY);
   if (tape->window == NULL) {
      tape_free();
      return false;
   }
   tape->last_append_offset = UINT32_MAX;
   if (++next_token == 0u)
      ++next_token;
   tape->token = next_token;
   return true;
}

/* Source callback for uef_stream: the uploaded image, or the JIM aperture on
 * the one-shot path. */
static size_t upload_source(void *context, uint32_t offset,
                            uint8_t *buffer, size_t length)
{
   const uef_tape_t *t = context;
   if (offset >= t->upload_length)
      return 0u;
   if (length > t->upload_length - offset)
      length = t->upload_length - offset;
   memcpy(buffer, t->upload + offset, length);
   return length;
}

static size_t jim_source(void *context, uint32_t offset,
                         uint8_t *buffer, size_t length)
{
   uint32_t available = *(const uint32_t *)context;
   if (offset >= available)
      return 0u;
   if (length > available - offset)
      length = available - offset;
   memcpy(buffer, &Pi1MHz->JIM_ram[UEF_BASE + offset], length);
   return length;
}

static void guard_stamp(void)
{
   unsigned int page;
   if (!guard_image_valid)
      return;
   /* Page 0 is the service command and reply buffer, which OSWORD &65
    * clients read as 241 contiguous bytes, so it cannot also hold the guard.
    * The host keeps the selector off page 0 outside service calls.
    *
    * Page 255 is excluded too: the guard run is UEF_GUARD_OFFSET..255 of each
    * page, so on page 255 it covers 0xFF97..0xFFFF - which CONTAINS the length
    * trailer at UEF_TRAILER (0xFFFE).  Both window sizes already reserve the
    * last page for exactly that reason.  Stamping it left the trailer holding
    * two arbitrary bytes of the ROM's own guard image, and an APPEND with
    * length == 0 - the documented "read my length from the trailer" form -
    * then appended an arbitrary number of bytes from JIM page 0. */
   for (page = UEF_FIRST_PAGE; page < 255u; page++)
      memcpy(&Pi1MHz->JIM_ram[UEF_BASE + (page << 8) + UEF_GUARD_OFFSET],
             guard_image, UEF_GUARD_LENGTH);
}

/* Lay the window into the aperture in UEF_GUARD_OFFSET runs, one per page, so
 * it never covers the guard. */
static void window_scatter(const uint8_t *source, size_t count)
{
   size_t page = 0u;
   while (count != 0u) {
      size_t chunk = count < UEF_GUARD_OFFSET ? count : UEF_GUARD_OFFSET;
      memcpy(&Pi1MHz->JIM_ram[UEF_BASE + ((page + UEF_FIRST_PAGE) << 8)],
             source, chunk);
      source += chunk;
      count -= chunk;
      page++;
   }
   guard_stamp();
}

/* Put the window we are already holding into the aperture, under whichever
 * layout the guard currently dictates. */
static void window_lay_out(void)
{
   if (tape->window_length != 0u) {
      if (guard_image_valid)
         window_scatter(tape->window, tape->window_length);
      else
         memcpy(&Pi1MHz->JIM_ram[UEF_BASE + (UEF_FIRST_PAGE << 8)],
                tape->window, tape->window_length);
   }
   public_length_set(tape->window_length);
}

/* Pull the next window out of the stream and publish it. */
static void window_advance(void)
{
   size_t limit = guard_image_valid ? UEF_GUARD_WINDOW : UEF_FLAT_WINDOW;
   size_t count = uef_stream_read(&tape->stream, tape->window, limit);
   tape->window_length = (uint16_t)count;
   /* A short window means the stream ended inside it; a zero-length one that
    * the host asked for means the tape is simply over. */
   tape->window_final = count < limit;
   window_lay_out();
}

static void incremental_response(uint32_t cp)
{
   uint8_t *p = &Pi1MHz->JIM_ram[cp + 1u];
   memcpy(p, "IUEF", 4u);
   p[4] = UEF_STREAM_VERSION;
   put_le32(p + 5u, tape != NULL ? tape->token : 0u);
   put_le32(p + 9u, tape != NULL ? tape->generation : 0u);
   put_le16(p + 13u, tape != NULL ? tape->window_length : 0u);
   p[15] = (tape != NULL && tape->window_final) ? 1u : 0u;
   p[16] = tape != NULL ? tape->format : 0u;
   p[17] = 0u;
}

static bool incremental_request(uint32_t cp)
{
   const uint8_t *p = &Pi1MHz->JIM_ram[cp + 1u];
   return p[0] == 'I' && p[1] == 'U' && p[2] == 'E' && p[3] == 'F'
       && p[4] == UEF_STREAM_VERSION;
}

static uint8_t format_letter(uef_format_t format)
{
   return format == UEF_FORMAT_GZIP ? 'G'
        : format == UEF_FORMAT_ZIP  ? 'Z' : 'R';
}

/* Grow the upload buffer to hold `extra` more bytes.  Grows in steps rather
 * than exactly, so a tape uploaded in 255-byte APPENDs does not realloc on
 * every one. */
static bool upload_reserve(uint32_t extra)
{
   uint32_t needed = tape->upload_length + extra;
   uint32_t capacity;
   uint8_t *grown;
   if (extra > UEF_UPLOAD_MAX || needed > UEF_UPLOAD_MAX)
      return false;
   if (needed <= tape->upload_capacity)
      return true;
   capacity = tape->upload_capacity + UEF_UPLOAD_GROW;
   while (capacity < needed)
      capacity += UEF_UPLOAD_GROW;
   if (capacity > UEF_UPLOAD_MAX)
      capacity = UEF_UPLOAD_MAX;
   grown = realloc(tape->upload, capacity);
   if (grown == NULL)
      return false;
   tape->upload = grown;
   tape->upload_capacity = capacity;
   return true;
}

static uint8_t stream_operation(uint32_t cp)
{
   const uint8_t *request = &Pi1MHz->JIM_ram[cp + 1u];
   uint8_t operation = request[5];
   uint32_t token = get_le32(request + 6u);
   uint32_t value = get_le32(request + 10u);
   uint16_t length = get_le16(request + 14u);
   uint32_t crc = get_le32(request + 16u);

   switch (operation) {
      case UEF_OP_PROBE:
         incremental_response(cp);
         return WIFI_SVC_OK;

      case UEF_OP_BEGIN:
         if (!tape_alloc())
            return WIFI_SVC_ERR_IO;
         tape->uploading = true;
         incremental_response(cp);
         return WIFI_SVC_OK;

      case UEF_OP_APPEND:
      {
         uint32_t actual_crc;
         if (tape == NULL)
            return WIFI_SVC_ERR_PARAM;
         if (token == 0u)
            token = tape->token;
         if (length == 0u)
            length = (uint16_t)public_length_get();
         if (!tape->uploading || token != tape->token
             || length == 0u || length > UEF_APPEND_MAX)
            return WIFI_SVC_ERR_PARAM;
         /* uzlib's table CRC, already linked for the tape stream: seed ~0, invert
            the running value for the final CRC-32 */
         actual_crc = ~uzlib_crc32(&Pi1MHz->JIM_ram[UEF_BASE], (unsigned int)length, 0xffffffffu);
         if (value == tape->generation) {
            if (!upload_reserve(length))
               return WIFI_SVC_ERR_PARAM;
            if (crc != 0u && actual_crc != crc)
               return WIFI_SVC_ERR_PARAM;
            memcpy(tape->upload + tape->upload_length,
                   &Pi1MHz->JIM_ram[UEF_BASE], length);
            tape->upload_length += length;
            tape->last_append_offset = value;
            tape->last_append_length = length;
            tape->last_append_crc = actual_crc;
            tape->generation++;
         } else if (value + 1u != tape->generation
                    || value != tape->last_append_offset
                    || length != tape->last_append_length
                    || actual_crc != tape->last_append_crc
                    || (crc != 0u && crc != actual_crc)) {
            /* Not the next window and not an exact repeat of the last one:
             * the host and the Pi disagree about where the upload is. */
            return WIFI_SVC_ERR_PARAM;
         }
         incremental_response(cp);
         return WIFI_SVC_OK;
      }

      case UEF_OP_FINALIZE:
      {
         uef_format_t format;
         if (tape == NULL)
            return WIFI_SVC_ERR_PARAM;
         if (token == 0u)
            token = tape->token;
         if (!tape->uploading || token != tape->token
             || tape->upload_length == 0u)
            return WIFI_SVC_ERR_PARAM;
         format = uef_stream_open(&tape->stream, upload_source, tape,
                                  tape->upload_length);
         if (format == UEF_FORMAT_INVALID) {
            response_string(cp, "INVALID\r\n");
            return WIFI_SVC_OK;
         }
         tape->format = format_letter(format);
         tape->ready = true;
         tape->uploading = false;
         tape->generation++;
         window_advance();
         incremental_response(cp);
         return WIFI_SVC_OK;
      }

      case UEF_OP_REWIND:
         if (tape == NULL || !tape->ready
             || (token != 0u && token != tape->token))
            return WIFI_SVC_ERR_PARAM;
         if (!uef_stream_rewind(&tape->stream))
            return WIFI_SVC_ERR_IO;
         tape->generation++;
         window_advance();
         incremental_response(cp);
         return WIFI_SVC_OK;

      case UEF_OP_REFILL:
         if (tape == NULL || !tape->ready
             || (token != 0u && token != tape->token))
            return WIFI_SVC_ERR_PARAM;
         if ((token == 0u && value == 0u) || value == 0xffffffffu
             || value == tape->generation) {
            tape->generation++;
            window_advance();
         } else if (value + 1u == tape->generation) {
            /* The host is asking for the window it already has, because
             * something trod on it - lay the same bytes down again. */
            window_lay_out();
         } else {
            return WIFI_SVC_ERR_PARAM;
         }
         incremental_response(cp);
         return WIFI_SVC_OK;

      case UEF_OP_REPUBLISH:
         /* The host's service reply buffer shares the aperture with the
          * published window, so any command that copies a reply while a
          * stream is open treads on part of it.  Lay the same bytes down
          * again without moving the stream or the generation: the host has
          * not consumed anything, it has only had the window damaged. */
         if (tape == NULL || !tape->ready
             || (token != 0u && token != tape->token))
            return WIFI_SVC_ERR_PARAM;
         window_lay_out();
         incremental_response(cp);
         return WIFI_SVC_OK;

      case UEF_OP_CLOSE:
         if (tape != NULL && token != 0u && token != tape->token)
            return WIFI_SVC_ERR_PARAM;
         tape_free();
         public_length_set(0u);
         incremental_response(cp);
         return WIFI_SVC_OK;

      default:
         return WIFI_SVC_ERR_PARAM;
   }
}

/* The one-shot path: the host has put a whole UEF in the aperture and wants
 * it normalised in place.  Kept for hosts that predate the incremental
 * protocol; it is bounded by the aperture, so it needs no upload buffer. */
static uint8_t legacy_normalize(uint32_t cp)
{
   uint32_t available = (uint32_t)public_length_get();
   uef_format_t format;
   size_t produced = 0u;

   if (available == 0u || available > UEF_LEGACY_CAPACITY)
      return WIFI_SVC_ERR_PARAM;
   if (!tape_alloc())
      return WIFI_SVC_ERR_IO;

   format = uef_stream_open(&tape->stream, jim_source, &available, available);
   if (format == UEF_FORMAT_INVALID) {
      tape_free();
      response_string(cp, "INVALID\r\n");
      return WIFI_SVC_OK;
   }
   /* Decompress into our own buffer first: the source is the aperture we are
    * about to overwrite. */
   for (;;) {
      size_t got = uef_stream_read(&tape->stream, tape->window + produced,
                                   UEF_LEGACY_CAPACITY - produced);
      if (got == 0u)
         break;
      produced += got;
      if (produced == UEF_LEGACY_CAPACITY) {
         /* Still more to come than the aperture can hold. */
         if (uef_stream_read(&tape->stream, tape->window, 1u) != 0u) {
            tape_free();
            response_string(cp, "TOO LARGE\r\n");
            return WIFI_SVC_OK;
         }
         break;
      }
   }
   if (uef_stream_failed(&tape->stream)) {
      tape_free();
      response_string(cp, "INVALID\r\n");
      return WIFI_SVC_OK;
   }
   memcpy(&Pi1MHz->JIM_ram[UEF_BASE], tape->window, produced);
   public_length_set(produced);
   /* The one-shot path hands the tape over whole and keeps no stream. */
   tape_free();
   response_string(cp, format == UEF_FORMAT_RAW  ? "RAW\r\n"
                     : format == UEF_FORMAT_GZIP ? "GZIP\r\n" : "ZIP\r\n");
   return WIFI_SVC_OK;
}

uint8_t uef_service_stream_command(uint32_t cp)
{
   if (incremental_request(cp))
      return stream_operation(cp);
   return legacy_normalize(cp);
}

uint8_t uef_service_guard_command(uint32_t cp)
{
   /* The host sends its assembled filing-vector guard and the Pi only
    * replicates it into the top of every JIM page.  A zero length withdraws
    * it and hands the pages back to the stream.  The Pi never interprets the
    * image; only the host knows what it does. */
   const uint8_t *p = &Pi1MHz->JIM_ram[cp + 1u];
   if (p[0] == 0u) {
      guard_image_valid = false;
      memset(guard_image, 0, sizeof guard_image);
      if (tape != NULL && tape->ready)
         window_lay_out();
      response_string(cp, "OFF\r\n");
      return WIFI_SVC_OK;
   }
   if (p[0] != UEF_GUARD_LENGTH)
      return WIFI_SVC_ERR_PARAM;
   memcpy(guard_image, p + 1, UEF_GUARD_LENGTH);
   guard_image_valid = true;
   guard_stamp();
   /* Publishing or withdrawing the guard changes the shape of the window, so
    * anything already published was laid out for the old shape and the host
    * would read it misaligned.  Lay the same bytes down again under the new
    * rule - the window we are holding makes that exact. */
   if (tape != NULL && tape->ready)
      window_lay_out();
   response_string(cp, "OK\r\n");
   return WIFI_SVC_OK;
}

void uef_service_reset(void)
{
   tape_free();
   guard_image_valid = false;
}
