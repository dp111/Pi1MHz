#ifndef STUB_FILESYSTEM_H
#define STUB_FILESYSTEM_H
#include <stdint.h>
#include <stdbool.h>
uint32_t filesystemWriteFile(const char *filename, const uint8_t *address, uint32_t max_size);
bool filesystemWriteFileSafe(const char *filename, const uint8_t *address, uint32_t length);
#endif
