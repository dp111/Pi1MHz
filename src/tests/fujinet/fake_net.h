#ifndef FAKE_NET_H
#define FAKE_NET_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { char url[256]; uint8_t mode, method; uint32_t body_len; char ctype[64]; } fake_net_open_t;

void fake_net_reset(void);
void fake_net_enable(bool on);
void fake_net_open_delay(int calls);     /* "not yet"s before an open is ready */
void fake_net_body(const void *d, uint32_t n);
void fake_net_chunk(uint32_t n);         /* most bytes one read delivers */
void fake_net_stall(int reads);          /* the next n reads deliver nothing */
void fake_net_accept(uint32_t n);        /* most bytes one write takes; 0 = all */
void fake_net_write_block(bool on);      /* writes take nothing */
const fake_net_open_t *fake_net_last_open(void);
const uint8_t *fake_net_sink(uint32_t *n);
int  fake_net_opens(void);
int  fake_net_handles_taken(void);

#endif
