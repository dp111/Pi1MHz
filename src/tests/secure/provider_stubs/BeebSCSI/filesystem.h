#pragma once
/* Host-test stub of BeebSCSI filesystem.h - the one call the provider uses. */
#include <stdbool.h>
#include <stdint.h>
bool filesystemWriteFileSafe(const char *filename, const uint8_t *address, uint32_t length);
