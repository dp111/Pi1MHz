#include "framebuffer_export.h"

#include <stddef.h>

#include "../BeebSCSI/filesystem.h"
#include "../framebuffer/framebuffer.h"

bool framebuffer_export_get_info(framebuffer_export_info_t *info)
{
   screen_mode_t *mode;

   if (info == NULL)
      return false;

   mode = fb_get_current_screen_mode();
   if (mode == NULL)
      return false;

   info->address = fb_get_address();
   info->width = (uint32_t)mode->width;
   info->height = (uint32_t)mode->height;
   info->pitch = (uint32_t)mode->pitch;
   info->bits_per_pixel = 1u << (uint32_t)mode->log2bpp;
   info->size = info->height * info->pitch;

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