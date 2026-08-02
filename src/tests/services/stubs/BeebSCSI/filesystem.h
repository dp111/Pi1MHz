#pragma once
/* Host-test stub of BeebSCSI filesystem.h. */
#include <stdbool.h>

#include <stdint.h>

bool filesystemMount(void);
bool filesystemDismount(void);
uint32_t filesystemReadFile(const char *filename, uint8_t **address, unsigned int max_size);
