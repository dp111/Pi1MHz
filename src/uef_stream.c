#include "uef_stream.h"
#include "rpi/byteorder.h"

#include <string.h>

static const uint8_t uef_magic[] = { 'U', 'E', 'F', ' ', 'F', 'i', 'l', 'e', '!', 0 };


/* Read straight from the source, bypassing the inflater's own buffering.
 * Only used while sniffing the container header. */
static size_t source_at(uef_stream_t *stream, uint32_t offset,
                        uint8_t *buffer, size_t length)
{
   if (offset >= stream->source_length)
      return 0u;
   if (length > stream->source_length - offset)
      length = stream->source_length - offset;
   return stream->source(stream->source_context, offset, buffer, length);
}

/* uzlib pulls compressed input through here.  It has no context pointer, so
 * recover the stream from the address of its embedded state - which is why
 * `inflate` is the first member of uef_stream_t. */
static int inflate_source_read(struct uzlib_uncomp *uncomp)
{
   uef_stream_t *stream = (uef_stream_t *)(void *)uncomp;
   size_t got;
   if (stream->source_position >= stream->source_length)
      return -1;
   got = source_at(stream, stream->source_position, stream->chunk,
                   UEF_SRC_CHUNK);
   if (got == 0u)
      return -1;
   stream->source_position += (uint32_t)got;
   uncomp->source = stream->chunk + 1;
   uncomp->source_limit = stream->chunk + got;
   return stream->chunk[0];
}

/* Walk the gzip header to the first DEFLATE byte.  Returns false if the
 * header is malformed or runs off the end. */
static bool gzip_data_start(uef_stream_t *stream, uint32_t *start)
{
   uint8_t header[12];
   uint32_t pos = 10u;
   uint8_t flags;

   /* A gzip UEF needs a 10 byte header, a trailer, and something between. */
   if (stream->source_length < 20u)
      return false;
   if (source_at(stream, 0u, header, 4u) != 4u)
      return false;
   if (header[2] != 8u || (header[3] & 0xe0u) != 0u)
      return false;
   flags = header[3];

   if ((flags & 4u) != 0u) {           /* FEXTRA */
      uint8_t len[2];
      if (source_at(stream, pos, len, 2u) != 2u)
         return false;
      pos += 2u + (uint32_t)get_le16(len);
   }
   if ((flags & 8u) != 0u) {           /* FNAME, NUL terminated */
      uint8_t c;
      do {
         if (source_at(stream, pos, &c, 1u) != 1u)
            return false;
         pos++;
      } while (c != 0u);
   }
   if ((flags & 16u) != 0u) {          /* FCOMMENT, NUL terminated */
      uint8_t c;
      do {
         if (source_at(stream, pos, &c, 1u) != 1u)
            return false;
         pos++;
      } while (c != 0u);
   }
   if ((flags & 2u) != 0u)             /* FHCRC */
      pos += 2u;

   /* Must leave at least the 8 byte trailer. */
   if (pos >= stream->source_length - 8u)
      return false;
   *start = pos;
   return true;
}

/* The zip case we accept is the one UEF archives actually use: a single
 * stored-or-deflated entry.  Anything else is refused rather than guessed
 * at, since a wrong guess would feed the Beeb noise as if it were tape. */
static bool zip_data_start(uef_stream_t *stream, uint32_t *start,
                           bool *stored)
{
   uint8_t header[30];
   uint16_t flags, method, name_length, extra_length;
   uint32_t data;

   if (stream->source_length < 30u)
      return false;
   if (source_at(stream, 0u, header, sizeof header) != sizeof header)
      return false;
   if (get_le32(header) != 0x04034b50u)
      return false;
   flags = get_le16(header + 6u);
   method = get_le16(header + 8u);
   /* Bit 0 is encryption, bit 3 puts the sizes in a trailing descriptor we
    * would have to hunt for - refuse both. */
   if ((flags & 9u) != 0u || (method != 0u && method != 8u))
      return false;
   stream->expected_crc = get_le32(header + 14u);
   stream->expected_length = get_le32(header + 22u);
   name_length = get_le16(header + 26u);
   extra_length = get_le16(header + 28u);
   data = 30u + (uint32_t)name_length + (uint32_t)extra_length;
   if (data >= stream->source_length)
      return false;
   *start = data;
   *stored = method == 0u;
   return true;
}

static void inflate_restart(uef_stream_t *stream)
{
   uzlib_uncompress_init(&stream->inflate, stream->dict, UEF_DICT_SIZE);
   stream->inflate.source = NULL;
   stream->inflate.source_limit = NULL;
   stream->inflate.source_read_cb = inflate_source_read;
   stream->source_position = stream->data_start;
   stream->produced = 0u;
   stream->running_crc = ~0u;
   stream->finished = false;
   stream->failed = false;
}

uef_format_t uef_stream_open(uef_stream_t *stream, uef_source_fn source,
                             void *context, uint32_t length)
{
   uint8_t magic[sizeof uef_magic];
   bool stored = false;

   memset(stream, 0, sizeof *stream);
   stream->source = source;
   stream->source_context = context;
   stream->source_length = length;
   stream->format = UEF_FORMAT_INVALID;

   if (source == NULL || length == 0u)
      return UEF_FORMAT_INVALID;

   if (source_at(stream, 0u, magic, sizeof magic) == sizeof magic
       && memcmp(magic, uef_magic, sizeof uef_magic) == 0) {
      /* Already a tape - hand the bytes straight through. */
      stream->format = UEF_FORMAT_RAW;
      stream->data_start = 0u;
      stream->source_position = 0u;
      stream->expected_length = length;
      return UEF_FORMAT_RAW;
   }

   if (magic[0] == 0x1fu && magic[1] == 0x8bu) {
      uint8_t trailer[8];
      if (!gzip_data_start(stream, &stream->data_start))
         return UEF_FORMAT_INVALID;
      /* gzip states the decompressed length and CRC in its trailer, so we
       * can check both without a second pass. */
      if (source_at(stream, length - 8u, trailer, 8u) == 8u) {
         stream->expected_crc = get_le32(trailer);
         stream->expected_length = get_le32(trailer + 4u);
      }
      stream->format = UEF_FORMAT_GZIP;
      inflate_restart(stream);
      return UEF_FORMAT_GZIP;
   }

   if (get_le32(magic) == 0x04034b50u) {
      if (!zip_data_start(stream, &stream->data_start, &stored))
         return UEF_FORMAT_INVALID;
      stream->format = stored ? UEF_FORMAT_RAW : UEF_FORMAT_ZIP;
      if (stored) {
         /* Stored entry: the tape is already plain inside the archive. */
         stream->source_position = stream->data_start;
         return UEF_FORMAT_RAW;
      }
      inflate_restart(stream);
      return UEF_FORMAT_ZIP;
   }

   return UEF_FORMAT_INVALID;
}

size_t uef_stream_read(uef_stream_t *stream, uint8_t *buffer, size_t length)
{
   size_t produced = 0u;

   if (stream->failed || stream->finished || length == 0u)
      return 0u;

   if (stream->format == UEF_FORMAT_RAW) {
      uint32_t remaining = stream->expected_length
                         - (stream->source_position - stream->data_start);
      if (length > remaining)
         length = remaining;
      if (length != 0u)
         produced = source_at(stream, stream->source_position, buffer, length);
      stream->source_position += (uint32_t)produced;
      if (produced == 0u)
         stream->finished = true;
   } else {
      int result;
      stream->inflate.dest_start = buffer;
      stream->inflate.dest = buffer;
      stream->inflate.dest_limit = buffer + length;
      result = uzlib_uncompress(&stream->inflate);
      produced = (size_t)(stream->inflate.dest - buffer);
      if (result == TINF_DONE)
         stream->finished = true;
      else if (result != TINF_OK)
         stream->failed = true;
   }

   if (produced != 0u) {
      stream->running_crc = uzlib_crc32(buffer, (unsigned int)produced,
                                        stream->running_crc);
      stream->produced += (uint32_t)produced;
   }
   return produced;
}

bool uef_stream_rewind(uef_stream_t *stream)
{
   if (stream->format == UEF_FORMAT_INVALID)
      return false;
   if (stream->format == UEF_FORMAT_RAW) {
      stream->source_position = stream->data_start;
      stream->produced = 0u;
      stream->running_crc = ~0u;
      stream->finished = false;
      stream->failed = false;
      return true;
   }
   inflate_restart(stream);
   return true;
}

bool uef_stream_verified(const uef_stream_t *stream)
{
   if (!stream->finished || stream->failed)
      return false;
   if (stream->expected_length != 0u && stream->produced != stream->expected_length)
      return false;
   /* A raw tape has no checksum of its own to check against. */
   if (stream->format == UEF_FORMAT_RAW)
      return true;
   return stream->expected_crc == ~stream->running_crc;
}

bool uef_stream_failed(const uef_stream_t *stream)
{
   return stream->failed;
}
