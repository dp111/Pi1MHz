#ifndef WIFI_FRAMEBUFFER_EXPORT_H
#define WIFI_FRAMEBUFFER_EXPORT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
   uint32_t address;
   uint32_t size;
   uint32_t width;
   uint32_t height;
   uint32_t pitch;
   uint32_t bits_per_pixel;
} framebuffer_export_info_t;

bool framebuffer_export_get_info(framebuffer_export_info_t *info);
bool framebuffer_export_save_raw(const char *path, framebuffer_export_info_t *info);

#endif