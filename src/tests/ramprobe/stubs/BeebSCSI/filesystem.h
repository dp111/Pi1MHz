#pragma once
/* Host-test stub: the board (board.h) serves 6502code.bin, and no JIM_Init.bin. */
#include <stdint.h>
uint32_t filesystemReadFile(const char *filename, uint8_t **address, unsigned int max_size);
