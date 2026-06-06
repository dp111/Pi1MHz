#!/bin/sh -e
# Host build + run of the teletext adapter tests. Builds the real
# teletext_emulator.c against minimal stub headers (mirroring the Pi1MHz
# bus / lwIP / wifi APIs) so it can run on a PC. From tests/teletext/.
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
mkdir -p "$B/rpi" "$B/wifi" "$B/lwip"

cat > "$B/Pi1MHz.h" <<'EOF'
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define LOG_DEBUG(...) printf(__VA_ARGS__)
#define LOG_INFO(...)  printf(__VA_ARGS__)
#define WRITE_FRED 0
#define READ_FRED  0x200
#define GET_DATA(g) ((unsigned)(g) & 0xffu)
#define GET_ADDR(g) (((unsigned)(g) >> 8) & 0xffu)
typedef void (*callback_func_ptr)(unsigned int);
typedef void (*func_ptr)(void);
void Pi1MHz_Register_Memory(int access, uint8_t addr, callback_func_ptr fn);
void Pi1MHz_Register_Poll(func_ptr fn);
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data);
void Pi1MHz_nIRQ_ASSERT(uint8_t src);
void Pi1MHz_nIRQ_CLEAR(uint8_t src);
EOF
echo 'char *get_cmdline_prop(const char *prop);' > "$B/rpi/info.h"
printf '#pragma once\n#include <stdint.h>\nuint32_t RPI_GetSystemTime(void);\n' > "$B/rpi/systimer.h"
echo 'void wifi_debug_printf(const char *fmt, ...);' > "$B/wifi/wifi.h"
cat > "$B/wifi/wifi_lwip.h" <<'EOF'
#pragma once
#include <stdbool.h>
typedef struct { bool address_ready; } wifi_lwip_context_t;
const wifi_lwip_context_t *wifi_lwip_get_context(void);
EOF
cat > "$B/lwip/ip_addr.h" <<'EOF'
#pragma once
#include <stdint.h>
typedef struct { uint32_t addr; } ip_addr_t;
#define ip_addr_set_ip4_u32(ipaddr, val) ((ipaddr)->addr = (uint32_t)(val))
EOF
cat > "$B/lwip/pbuf.h" <<'EOF'
#pragma once
#include <stdint.h>
struct pbuf { struct pbuf *next; void *payload; uint16_t len; uint16_t tot_len; };
uint8_t pbuf_free(struct pbuf *p);
EOF
cat > "$B/lwip/tcp.h" <<'EOF'
#pragma once
#include <stdint.h>
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
typedef int8_t err_t;
#define ERR_OK 0
struct tcp_pcb;
typedef err_t (*tcp_recv_fn)(void*, struct tcp_pcb*, struct pbuf*, err_t);
typedef err_t (*tcp_connected_fn)(void*, struct tcp_pcb*, err_t);
typedef void  (*tcp_err_fn)(void*, err_t);
struct tcp_pcb *tcp_new(void);
void tcp_arg(struct tcp_pcb*, void*);
void tcp_recv(struct tcp_pcb*, tcp_recv_fn);
void tcp_err(struct tcp_pcb*, tcp_err_fn);
err_t tcp_connect(struct tcp_pcb*, const ip_addr_t*, uint16_t, tcp_connected_fn);
void tcp_recved(struct tcp_pcb*, uint16_t);
err_t tcp_close(struct tcp_pcb*);
void tcp_abort(struct tcp_pcb*);
EOF

cp "$SRC/teletext_emulator.c" "$SRC/teletext_emulator.h" "$B/"
cp "$HERE/test_teletext.c" "$B/"

echo "== compile-check (strict) =="
gcc -std=gnu2x -I"$B" -c "$B/teletext_emulator.c" -o "$B/ttx.o" \
   -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wcast-qual \
   -Wpointer-arith -Wstrict-prototypes -Wundef -Wredundant-decls -Wnull-dereference

echo "== functional test =="
gcc -std=gnu2x -I"$B" -Wall -Wextra -o "$B/ttxtest" "$B/test_teletext.c"
"$B/ttxtest"

echo "TELETEXT TESTS PASSED"
