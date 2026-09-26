/* fujibus.c - FujiBus packet layer and device dispatch.  See fujibus.h. */

#include <string.h>

#include "fujibus.h"
#include "fn_devices.h"
#include "fn_store.h"

uint8_t fb_get_u8(fb_in *in)
{
   if (in->pos >= in->len) {
      in->bad = true;
      return 0;
   }
   return in->p[in->pos++];
}

uint16_t fb_get_u16(fb_in *in)
{
   uint16_t lo = fb_get_u8(in);
   return (uint16_t)(lo | (fb_get_u8(in) << 8));
}

uint32_t fb_get_u32(fb_in *in)
{
   uint32_t lo = fb_get_u16(in);
   return lo | ((uint32_t)fb_get_u16(in) << 16);
}

const uint8_t *fb_get_bytes(fb_in *in, uint16_t n)
{
   if (n > fb_left(in)) {
      in->bad = true;
      return NULL;
   }
   const uint8_t *p = in->p + in->pos;
   in->pos = (uint16_t)(in->pos + n);
   return p;
}

uint8_t *fb_put_space(fb_out *out, uint16_t n)
{
   if (out->full || n > fb_room(out)) {
      out->full = true;
      return NULL;
   }
   uint8_t *p = out->p + out->len;
   out->len = (uint16_t)(out->len + n);
   return p;
}

void fb_put_u8(fb_out *out, uint8_t v)
{
   uint8_t *p = fb_put_space(out, 1);
   if (p)
      p[0] = v;
}

void fb_put_u16(fb_out *out, uint16_t v)
{
   fb_put_u8(out, (uint8_t)v);
   fb_put_u8(out, (uint8_t)(v >> 8));
}

void fb_put_u32(fb_out *out, uint32_t v)
{
   fb_put_u16(out, (uint16_t)v);
   fb_put_u16(out, (uint16_t)(v >> 16));
}

void fb_put_bytes(fb_out *out, const void *src, uint16_t n)
{
   uint8_t *p = fb_put_space(out, n);
   if (p && n)
      memcpy(p, src, n);
}

uint8_t fb_checksum(const uint8_t *pkt, uint16_t len)
{
   unsigned int c = 0;
   for (uint16_t i = 0; i < len; i++) {
      c += (i == 4u) ? 0u : pkt[i];
      c = (c >> 8) + (c & 0xFFu);
   }
   return (uint8_t)c;
}

static fb_device_fn device_for(uint8_t device)
{
   switch (device) {
   case FB_DEV_DISK:     return fn_disk_command;
   case FB_DEV_SLOTCAT:  return fn_slotcat_command;
   case FB_DEV_APPSTORE: return fn_appstore_command;
   case FB_DEV_HOST:     return fn_host_command;
   case FB_DEV_FILE:     return fn_file_command;
   default:              return NULL;
   }
}

fb_answer fujibus_answer(const uint8_t *req, uint16_t req_len,
                         uint8_t *reply, uint16_t reply_cap, uint16_t *reply_len)
{
   if (req_len < FB_HDR_LEN || reply_cap < FB_REPLY_HDR_LEN ||
       (uint16_t)(req[2] | (req[3] << 8)) != req_len ||
       fb_checksum(req, req_len) != req[4]) {
      fn_store_request_end();
      return FB_ANSWER_NONE;
   }

   uint8_t device = req[0];
   uint8_t command = req[1];
   fb_in in = { .p = req + FB_HDR_LEN, .len = (uint16_t)(req_len - FB_HDR_LEN) };
   fb_out out = { .p = reply + FB_REPLY_HDR_LEN,
                  .cap = (uint16_t)(reply_cap - FB_REPLY_HDR_LEN) };

   uint8_t status;
   fb_device_fn fn = device_for(device);
   if (!fn)
      status = FB_DEVICE_NOT_FOUND;
   else if (req[5] != 0u)
      /* fn-rom only ever sends descriptor 0; anything else would need the
         general parameter encoding, which no device here implements. */
      status = FB_INVALID_REQUEST;
   else {
      fn_store_clear_pending();
      status = fn(command, &in, &out);
      if (fn_store_pending())
         return FB_ANSWER_PENDING;   /* whatever it wrote is incomplete */
      if (status == FB_OK && out.full)
         status = FB_INTERNAL_ERROR;
   }
   fn_store_request_end();
   if (status != FB_OK)
      out.len = 0;

   uint16_t total = (uint16_t)(FB_REPLY_HDR_LEN + out.len);
   reply[0] = device;
   reply[1] = command;
   reply[2] = (uint8_t)total;
   reply[3] = (uint8_t)(total >> 8);
   reply[4] = 0;
   reply[5] = 1;        /* one parameter: the status */
   reply[6] = status;
   reply[4] = fb_checksum(reply, total);
   *reply_len = total;
   return FB_ANSWER_REPLY;
}
