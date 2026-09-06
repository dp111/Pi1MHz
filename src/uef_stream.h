#ifndef UEF_STREAM_H
#define UEF_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uzlib/uzlib.h"

/* A UEF tape image served to the Beeb a window at a time.
 *
 * The Beeb reads a tape sequentially through one JIM aperture, so this never
 * holds the decompressed image: it keeps DEFLATE's 32 KB history and pulls
 * compressed bytes from the caller on demand.  Cost is flat in the size of
 * the tape - a 5 MB UEF costs the same RAM as a 20 KB one - which is what
 * lets the feature exist at all.  (PR #20's one-shot inflater needed the
 * whole thing in RAM twice, at 16 MB each.)
 *
 * Raw UEFs skip the inflater entirely and are passed straight through.
 */

#define UEF_DICT_SIZE   32768u  /* DEFLATE's maximum back-reference distance */
#define UEF_SRC_CHUNK     512u  /* compressed bytes held at once             */

typedef enum {
   UEF_FORMAT_RAW = 0,
   UEF_FORMAT_GZIP,
   UEF_FORMAT_ZIP,
   UEF_FORMAT_INVALID
} uef_format_t;

/* Pull `length` compressed bytes from `offset` into `buffer`; return how many
 * were actually produced.  The stream only ever reads forwards, except that
 * uef_stream_rewind restarts from the beginning. */
typedef size_t (*uef_source_fn)(void *context, uint32_t offset,
                                uint8_t *buffer, size_t length);

typedef struct {
   /* uzlib's read callback carries no context pointer of its own, so its
    * state must be the first member: the callback recovers this object from
    * the address uzlib hands back. */
   struct uzlib_uncomp  inflate;
   uef_source_fn        source;
   void                *source_context;
   uint32_t             source_length;   /* compressed bytes available     */
   uint32_t             source_position; /* how far the pull has got       */
   uint32_t             data_start;      /* first DEFLATE byte in source   */
   uint32_t             produced;        /* decompressed bytes emitted     */
   uint32_t             expected_length; /* from the container, 0 if unknown */
   uint32_t             expected_crc;
   uint32_t             running_crc;
   uef_format_t         format;
   bool                 finished;
   bool                 failed;
   uint8_t              chunk[UEF_SRC_CHUNK];
   uint8_t              dict[UEF_DICT_SIZE];
} uef_stream_t;

/* Identify the container and prepare to stream.  Returns the format, or
 * UEF_FORMAT_INVALID if this is not a UEF (nothing is allocated either way). */
uef_format_t uef_stream_open(uef_stream_t *stream, uef_source_fn source,
                             void *context, uint32_t length);

/* Emit up to `length` more bytes.  Returns how many; 0 means the tape has
 * ended (or failed - see uef_stream_failed). */
size_t uef_stream_read(uef_stream_t *stream, uint8_t *buffer, size_t length);

/* Back to the start of the tape.  Re-runs the inflater from the beginning,
 * which is why the source must still be readable from offset 0. */
bool uef_stream_rewind(uef_stream_t *stream);

/* True once every byte has been emitted and the container's own length and
 * CRC agreed with what we produced.  Only meaningful after the last read. */
bool uef_stream_verified(const uef_stream_t *stream);

bool uef_stream_failed(const uef_stream_t *stream);

#endif
