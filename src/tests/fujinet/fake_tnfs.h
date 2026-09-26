#ifndef FAKE_TNFS_H
#define FAKE_TNFS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { int sent, dropped, opens, closes, reads, writes, seeks; } fake_tnfs_stats_t;

void fake_tnfs_reset(void);
void fake_tnfs_mkdir(const char *path);
void fake_tnfs_put(const char *path, const void *data, uint32_t size);
const uint8_t *fake_tnfs_get(const char *path, uint32_t *size);
int  fake_tnfs_open_fds(void);
void fake_tnfs_step(void);          /* answer everything queued */
void fake_tnfs_advance(uint32_t ms);
void fake_tnfs_drop(int n);         /* lose the next n requests */
void fake_tnfs_eagain(int n);       /* answer the next n with EAGAIN (50 ms) */
fake_tnfs_stats_t *fake_tnfs_stats(void);

#endif
