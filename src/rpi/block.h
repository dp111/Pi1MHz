/* Copyright (C) 2013 by John Cronin <jncronin@tysos.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct fs;

struct block_device {
//	const char *driver_name;
//	const char *device_name;
//	uint32_t device_id[4];
//	size_t dev_id_len;

//	int supports_multiple_block_read;
//	int supports_multiple_block_write;

	size_t (*read)(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_num);
	size_t (*write)(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_num);
//	size_t block_size;
//	size_t num_blocks;

	struct fs *fs;
};

struct emmc_block_dev
{
	struct block_device bd;
	bool card_supports_sdhc;
	bool card_supports_18v;
	uint32_t card_rca;
	uint32_t last_interrupt;
	uint32_t last_error;

	struct sd_scr *scr;

	bool failed_voltage_switch;

	bool last_cmd_success;
	bool use_sdma;
	bool card_removal;
	uint32_t last_r0;
//	uint32_t last_r1;
//	uint32_t last_r2;
//	uint32_t last_r3;

	void *buf;
	uint32_t blocks_to_transfer;
	size_t block_size;

};


size_t block_read(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t starting_block);
size_t block_write(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t starting_block);

#endif


