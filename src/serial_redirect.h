#ifndef SERIAL_REDIRECT_H
#define SERIAL_REDIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the Beeb writes to the stub's control port (+&28): helpers 19 and 20, and the TelnetD ROM
   (beeb/telnetd-rom/telnetd.asm uses the same names). */
enum {
   SER_CTL_UNHOOK     = 0x00,   /* unhooked: back to the modem's RS423 */
   SER_CTL_HOOKED     = 0x01,   /* hooked (helper 19, RS423) */
   SER_CTL_TELNET     = 0x10,   /* + MODE: telnet mode, the end of a hook report */
   SER_CTL_RESET      = 0x20,   /* the ROM: a reset (the reset line may not have moved), REMV not claimed */
   SER_CTL_ROMFS      = 0x22,   /* a ROM filing system operation: output RS423 did not carry */
   SER_CTL_KEY_TOLD   = 0x24,   /* a program took a cursor key, COPY or RETURN */
   SER_CTL_KEY_OTHER  = 0x25,   /* ... or any other byte */
   SER_CTL_RESET_LIT  = 0x26,   /* a reset, and REMV claimed: the screen can be asked for (SCANKEY), every key
                                   taken is told, and a byte after LITKEY is read as it is */
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

/* What helper 20 and the auto-start ROM report through the control port, kept by the FIQ: each
   count changes with every report of its kind.  Main loop: read the fields, never write them. */
typedef struct {
   uint32_t mode_gen;          /* hook reports (&10 + MODE, after the cursor's column and row) */
   uint32_t reset_gen;         /* the BBC's resets (serial_redirect_init) */
   uint32_t key_gen;           /* a program took a cursor key, COPY or RETURN (&24) */
   uint32_t key_other_gen;     /* ... or any other byte (&25) */
   uint32_t unseen_gen;        /* ROM filing system operations, whose output RS423 never sees (&22) */
   uint8_t  mode, col, row;    /* the Beeb's screen MODE and text cursor at the last hook report */
   bool     rom;               /* the ROM claimed REMV (&26): the screen can be asked for, every key taken
                                  is told, and a byte after LITKEY is read as it is; false after a reset */
} ser_beeb_report_t;
extern volatile const ser_beeb_report_t *const serial_redirect_beeb;

bool   serial_redirect_telnet_attached(void);   /* hooked by helper 20 or the ROM: the stream is telnetd's */
uint32_t serial_redirect_rx_count(void);    /* bytes put into the Beeb's buffer */
size_t serial_redirect_write(const uint8_t *src, size_t len);

/* Telnet mode (helper 20) only, main loop only: raise the Beeb's ESCAPE
   condition and discard what is queued for its keyboard.  A no-op otherwise. */
void serial_redirect_escape(void);
void serial_redirect_drop_keys(uint8_t keep_after);   /* the keys not yet given to the Beeb go (a new session), but
                                                         the one after keep_after if that was the last given */
void serial_redirect_telnetd_refresh(void); /* telnetd_enabled() changed: the ROM's TELNET byte says so */
void serial_redirect_shift(void);           /* SHIFT pressed once (paged mode), read by the auto-start ROM's KEYV */
void serial_redirect_break(void);       /* a BREAK, as the BREAK key: see the .c */

#endif
