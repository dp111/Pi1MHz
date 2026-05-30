#include "framebuffer_export.h"

#include <stddef.h>

#include "../BeebSCSI/filesystem.h"
#include "../framebuffer/framebuffer.h"

bool framebuffer_export_get_info(framebuffer_export_info_t *info)
{
   const screen_mode_t *mode;
   uint32_t       log2bpp;
   uint32_t       width;
   uint32_t       height;
   uint32_t       pitch;

   if (info == NULL)
      return false;

   mode = fb_get_current_screen_mode();
   if (mode == NULL)
      return false;

   /* mode->log2bpp is the log2 of bits-per-pixel.  Anything >= 5 means
      bits_per_pixel >= 32, which is the largest format the framebuffer
      pipeline supports (ARGB8888).  Reject silly values explicitly so
      the 1u << shift below can't trigger undefined behaviour on a
      corrupt screen_mode_t. */
   log2bpp = (uint32_t)mode->log2bpp;
   if (log2bpp > 5u)
      return false;

   width  = (uint32_t)mode->width;
   height = (uint32_t)mode->height;
   pitch  = (uint32_t)mode->pitch;

   /* Guard against pitch * height overflowing a uint32_t (the size
      field is uint32, and the caller will use it as both a write length
      and a buffer-extent check).  Real framebuffers max out around
      30 MB so this is a defensive check rather than a real path, but
      it removes a latent overflow class. */
   if (pitch != 0u && height > (UINT32_C(0xFFFFFFFF) / pitch))
      return false;

   info->address        = fb_get_address();
   info->width          = width;
   info->height         = height;
   info->pitch          = pitch;
   info->bits_per_pixel = 1u << log2bpp;
   info->size           = height * pitch;

   return true;
}

bool framebuffer_export_save_raw(const char *path, framebuffer_export_info_t *info)
{
   framebuffer_export_info_t local_info;

   if (path == NULL)
      return false;

   if (info == NULL)
      info = &local_info;

   if (!framebuffer_export_get_info(info))
      return false;

   return filesystemWriteFile(path, (const uint8_t *)(uintptr_t)info->address, info->size) == info->size;
}