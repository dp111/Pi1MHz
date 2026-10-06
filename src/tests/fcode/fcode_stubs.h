/* Host stub for what BeebSCSI/fcode.c includes besides fcode.h and
   videoplayer.h (both real): one header, which run_tests.sh puts at each of
   the paths it includes. */
#ifndef FCODE_STUBS_H
#define FCODE_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* debug.h, uart.h, cpuspecific.h: debug output off, no UART */
#define PSTR(s) (s)
#define debugFlag_scsiFcodes false
#define FCdebugString_P(...) {}
#define FCdebugString(...) {}
#define FCdebugStringInt8Hex_P(...) {}
#define FCdebugStringInt16_P(...) {}
#define FCdebugStringInt32_P(...) {}
#define debugString_P(...) {}
#define debugStringInt8Hex_P(...) {}
#define debugStringInt16_P(...) {}
#define UART_NO_DATA 0x0100
#define uartRead() UART_NO_DATA
#define uartPeekForString() false
#define uartAvailable() 0
#define uartFlush()

/* rpi/systimer.h, config.h, harddisc_emulator.h */
uint32_t RPI_GetSystemTime(void);
bool config_get_bool(const char *key);
void hd_juke_request(uint8_t dir);

/* BeebSCSI/filesystem.h */
enum parserkeyvalueenum { TITLE, DESCRIPTION };
uint8_t filesystemGetLunDirectoryVFS(void);
bool filesystemReadVFSCfgTextDir(uint8_t dir, enum parserkeyvalueenum key, char *out, uint32_t maxLen);
uint8_t filesystemVFSDirType(uint8_t dir);
bool filesystemVFSVolumePresent(void);
bool filesystemVFSDatPresent(void);
bool filesystemVFSDirPresent(uint8_t dir);
void filesystemReadLunUserCode(uint8_t lunNumber, uint8_t userCode[5]);

/* rpi/screen.h */
void screen_plane_enable(uint32_t planeno, bool enable);
void screen_plane_alpha(uint32_t planeno, uint32_t alpha);
void screen_set_highlight(bool on);
void screen_plane_treatment(uint32_t planeno, uint32_t palette_flags, uint32_t alpha);
void screen_plane_gate(uint32_t planeno, bool gated);
void screen_dim_strips(bool on);

#endif
