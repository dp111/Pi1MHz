/* fujibus.h - the FujiNet device answering fn-rom's FujiBus packets.

   The packet layer: header and checksum checks, a bounds-checked reply
   builder, and dispatch to the devices.  No Pi dependencies - the devices
   reach storage only through fn_store.h - so all of it builds and is tested
   on the host.  See docs/dev/fujinet-device.md.

   Packet (little-endian):
      [0] device  [1] command  [2..3] length (whole packet)
      [4] checksum  [5] descriptor
   Requests from fn-rom carry descriptor 0 and a device payload from [6].
   Every reply carries descriptor 1, [6] = status, device payload from [7]. */
#ifndef FUJIBUS_H
#define FUJIBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FB_HDR_LEN        6u
#define FB_REPLY_HDR_LEN  7u

/* Wire device ids (fujinet-nio WireDeviceId). */
#define FB_DEV_HOST       0xF0u
#define FB_DEV_APPSTORE   0xF1u
#define FB_DEV_SLOTCAT    0xF2u
#define FB_DEV_DISK       0xFCu
#define FB_DEV_NETWORK    0xFDu
#define FB_DEV_FILE       0xFEu

/* Transport status (fujinet-nio StatusCode). */
#define FB_OK             0u
#define FB_DEVICE_NOT_FOUND 1u
#define FB_INVALID_REQUEST  2u
#define FB_DEVICE_BUSY    3u
#define FB_NOT_READY      4u
#define FB_IO_ERROR       5u
#define FB_TIMEOUT        6u
#define FB_INTERNAL_ERROR 7u
#define FB_UNSUPPORTED    8u

/* Every device payload starts with this protocol version. */
#define FB_VERSION        1u

/* A request's device payload, read with the fb_get_* cursor functions.  A
   read past the end sets `bad` and returns zeroes, so a handler can parse a
   whole layout and test `bad` once. */
typedef struct {
   const uint8_t *p;
   uint16_t len;
   uint16_t pos;
   bool bad;
} fb_in;

uint8_t  fb_get_u8 (fb_in *in);
uint16_t fb_get_u16(fb_in *in);
uint32_t fb_get_u32(fb_in *in);
/* A pointer to the next n bytes (NULL and `bad` if they are not there). */
const uint8_t *fb_get_bytes(fb_in *in, uint16_t n);
static inline uint16_t fb_left(const fb_in *in) { return (uint16_t)(in->len - in->pos); }

/* A reply's device payload, written with the fb_put_* functions.  A write
   past the capacity sets `full`, which the dispatcher turns into an
   InternalError reply: a device never has to check each put. */
typedef struct {
   uint8_t *p;
   uint16_t cap;
   uint16_t len;
   bool full;
} fb_out;

void fb_put_u8 (fb_out *out, uint8_t v);
void fb_put_u16(fb_out *out, uint16_t v);
void fb_put_u32(fb_out *out, uint32_t v);
void fb_put_bytes(fb_out *out, const void *src, uint16_t n);
/* Reserve n bytes and return where they go, for a length written later. */
uint8_t *fb_put_space(fb_out *out, uint16_t n);
static inline uint16_t fb_room(const fb_out *out) { return (uint16_t)(out->cap - out->len); }

/* A device: parse `in`, write its reply payload to `out`, return a status.
   A non-Ok status discards whatever was written (the reply is header and
   status only). */
typedef uint8_t (*fb_device_fn)(uint8_t command, fb_in *in, fb_out *out);

/* The checksum fn-rom and fujinet-nio use: an end-around-carry byte sum over
   the packet, with byte 4 taken as zero. */
uint8_t fb_checksum(const uint8_t *pkt, uint16_t len);

/* Answer one request packet.  Returns false - no reply - when the packet is
   not a well-formed FujiBus request (short, length field disagreeing with
   `req_len`, bad checksum) or `reply_cap` cannot hold even a header; that is
   what a device on a serial line would silently drop.  Otherwise builds the
   reply in `reply`, sets *reply_len and returns true, including for requests
   the device refuses (the status says so). */
bool fujibus_answer(const uint8_t *req, uint16_t req_len,
                    uint8_t *reply, uint16_t reply_cap, uint16_t *reply_len);

#endif
