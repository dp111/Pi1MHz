#ifndef STUB_FILESYSTEM_H
#define STUB_FILESYSTEM_H
#include <stdint.h>
#include <stdbool.h>
/* An in-memory card: the suite sets the file the parser will read and
   inspects whatever it writes back. */
uint32_t filesystemReadFile(const char *filename, uint8_t **address, unsigned int max_size);
uint32_t filesystemWriteFile(const char *filename, const uint8_t *address, uint32_t max_size);
bool filesystemWriteFileSafe(const char *filename, const uint8_t *address, uint32_t length);
#endif
