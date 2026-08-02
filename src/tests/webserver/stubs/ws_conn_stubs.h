#pragma once
/* Host-test stand-ins for the firmware types that ws_conn_t embeds but
   the chunked-transfer parser under test never dereferences: FatFs file
   handles and the framebuffer export descriptor.  struct tcp_pcb stays
   an incomplete type - ws_conn_t only holds a pointer to it. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FF_LFN_BUF 255

typedef struct { uint32_t fsize; } FIL;
typedef struct { int dummy; } framebuffer_export_info_t;
struct tcp_pcb;
