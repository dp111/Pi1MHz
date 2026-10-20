#ifndef SERIAL_REDIRECT_H
#define SERIAL_REDIRECT_H

#include <stddef.h>
#include <stdint.h>

/* What the Beeb writes to the stub's control port (+&28): helpers 19 and 20, and the TelnetD ROM
   (beeb/telnetd-rom/telnetd.asm uses the same names). */
enum {
   SER_CTL_UNHOOK     = 0x00,   /* unhooked: back to the modem's RS423 */
   SER_CTL_HOOKED     = 0x01,   /* hooked (helper 19, RS423) */
   SER_CTL_TELNET_OLD = 0x02,   /* telnet mode (helper 20 before it reported the MODE) */
   SER_CTL_TELNET     = 0x10,   /* + MODE: telnet mode, the end of a hook report */
   SER_CTL_RESET      = 0x20,   /* the ROM: a reset (the reset line may not have moved) */
   SER_CTL_RESET_SCAN = 0x21,   /* ... and the screen can be asked for (SCANKEY) */
   SER_CTL_ROMFS      = 0x22,   /* a ROM filing system operation: output RS423 did not carry */
   SER_CTL_RESET_TELL = 0x23,   /* a reset, the screen can be asked for, and cursor keys, COPY, RETURN are told */
   SER_CTL_KEY_TOLD   = 0x24,   /* a program took a cursor key, COPY or RETURN */
   SER_CTL_KEY_OTHER  = 0x25,   /* ... or any other byte (ROM build 13) */
   SER_CTL_RESET_LIT  = 0x26,   /* a reset, as RESET_TELL, and every key told and LITKEY read as it is (build 13) */
   SER_CTL_COLUMN     = 0x80,   /* + the cursor's column, ahead of TELNET */
   SER_CTL_ROW        = 0xE0    /* + the cursor's row, ahead of TELNET */
};

void serial_redirect_init(uint8_t instance, uint8_t address);
uint8_t serial_redirect_address(void);      /* stub's FRED offset, 0 = disabled */
void serial_redirect_status(char *buf, size_t len);
#ifdef DEBUG
void serial_redirect_log(char *buf, size_t len);   /* the stub's recent events */
#endif

/* Main loop only: the bytes the Beeb has sent, and room for / bytes to it. */
size_t serial_redirect_read(uint8_t *dst, size_t max);
bool   serial_redirect_pending(void);       /* the Beeb has printed something not read yet */
size_t serial_redirect_room(void);

/* The Beeb's screen MODE (0-7), reported by helper 20 each time it hooks in, and
   a count that changes with every report. */
uint32_t serial_redirect_mode_gen(void);
bool serial_redirect_scan_ok(void);        /* the ROM can send the Beeb's screen when a byte &FF is put in the keyboard buffer */
bool serial_redirect_keys_told(void);       /* the ROM (build 11) tells each cursor key, COPY or RETURN a program takes */
uint32_t serial_redirect_key_gen(void);     /* ... counted */
uint32_t serial_redirect_key_other_gen(void); /* every other byte taken (build 13) */
bool serial_redirect_lit_ok(void);          /* the ROM reads a byte after LITKEY (&FB) as it is (build 13) */
uint32_t serial_redirect_rx_count(void);    /* bytes put into the Beeb's buffer */
uint32_t serial_redirect_tx_count(void);    /* bytes the Beeb has sent */
uint32_t serial_redirect_reset_gen(void);   /* ... and one that counts the BBC's resets */
uint32_t serial_redirect_unseen_gen(void);  /* ... and the ROM filing system operations, whose output RS423 never sees */
uint8_t  serial_redirect_beeb_mode(void);
void     serial_redirect_beeb_cursor(uint8_t *col, uint8_t *row);   /* ... and its text cursor */
size_t serial_redirect_write(const uint8_t *src, size_t len);

/* Telnet mode (helper 20) only, main loop only: raise the Beeb's ESCAPE
   condition and discard what is queued for its keyboard.  A no-op otherwise. */
void serial_redirect_escape(void);
void serial_redirect_drop_keys(void);      /* the keys not yet given to the Beeb go (a new session) */
void serial_redirect_shift(void);           /* SHIFT pressed once (paged mode), read by the auto-start ROM's KEYV */
void serial_redirect_break(void);       /* a BREAK, as the BREAK key: see the .c */

#endif
