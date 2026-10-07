/* Host tests for BeebSCSI/fcode.c: the VP (video overlay) F-codes.
 *
 * Plane 0 is the player's: whether it is wanted is decided by the player
 * alone - on with its first real decoded frame (reap_flip), off and on again
 * with E0/E1 (videoplayer_set_video), off on a media change.  The VP modes
 * are the mixer: they gate, key and mix the layers and must never assert
 * the player's plane themselves (review 2026-10-06 U1).  When they did, an
 * open player with nothing decoded showed whatever its buffer held, and a
 * VPx after E0 turned the disc video back on.  Real file, stub platform.
 */
#include <stdio.h>
#include <string.h>

#include "fcode_stubs.h"
#include "fcode.h"
#include "videoplayer.h"

/* ---- the stub platform -------------------------------------------------- */

static bool player_open;
static int  plane0_enables;           /* screen_plane_enable(0, ...) calls */
static int  plane0_gate = -1;         /* last screen_plane_gate(0, x), -1 none */

uint32_t RPI_GetSystemTime(void) { return 0u; }
bool config_get_bool(const char *key) { (void)key; return false; }
void hd_juke_request(uint8_t dir) { (void)dir; }

uint8_t filesystemGetLunDirectoryVFS(void) { return 1u; }
bool filesystemReadVFSCfgTextDir(uint8_t dir, enum parserkeyvalueenum key, char *out, uint32_t maxLen)
{ (void)dir; (void)key; (void)out; (void)maxLen; return false; }
uint8_t filesystemVFSDirType(uint8_t dir) { (void)dir; return 0u; }
bool filesystemVFSVolumePresent(void) { return true; }
bool filesystemVFSDatPresent(void) { return true; }
bool filesystemVFSDirPresent(uint8_t dir) { (void)dir; return false; }
void filesystemReadLunUserCode(uint8_t lunNumber, uint8_t userCode[5]) { (void)lunNumber; memset(userCode, 0, 5); }

void screen_plane_enable(uint32_t planeno, bool enable)
{
   (void)enable;
   if (planeno == 0u)
      plane0_enables++;
}
void screen_plane_gate(uint32_t planeno, bool gated)
{
   if (planeno == 0u)
      plane0_gate = gated ? 1 : 0;
}
void screen_plane_alpha(uint32_t planeno, uint32_t alpha) { (void)planeno; (void)alpha; }
void screen_set_highlight(bool on) { (void)on; }
void screen_plane_treatment(uint32_t planeno, uint32_t palette_flags, uint32_t alpha)
{ (void)planeno; (void)palette_flags; (void)alpha; }
void screen_dim_strips(bool on) { (void)on; }

bool videoplayer_active(void) { return player_open; }
bool videoplayer_seeking(void) { return false; }
bool videoplayer_audio_enabled(int channel) { (void)channel; return true; }
void videoplayer_goto(uint32_t picture, char op) { (void)picture; (void)op; }
void videoplayer_play_fwd(void) { }
void videoplayer_play_rev(void) { }
void videoplayer_halt(void) { }
void videoplayer_pause(void) { }
void videoplayer_step(int delta) { (void)delta; }
void videoplayer_speed(uint32_t value, bool fast) { (void)value; (void)fast; }
void videoplayer_slow_fwd(void) { }
void videoplayer_slow_rev(void) { }
void videoplayer_fast_fwd(void) { }
void videoplayer_fast_rev(void) { }
void videoplayer_clear(void) { }
void videoplayer_show_picture_number(bool on) { (void)on; }
void videoplayer_audio_enable(int channel, bool on) { (void)channel; (void)on; }
void videoplayer_set_video(bool on) { (void)on; }
bool videoplayer_take_stop_reached(void) { return false; }
uint32_t videoplayer_picture_number(void) { return 1u; }

/* ---- helpers ------------------------------------------------------------ */

static int failures, checks;

#define CHECK(cond, ...) do { \
      checks++; \
      if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                     printf(__VA_ARGS__); printf("\n"); } \
   } while (0)

static void send(const char *code)
{
   memset(scsiFcodeBuffer, 0, sizeof scsiFcodeBuffer);
   memcpy(scsiFcodeBuffer, code, strlen(code));
   scsiFcodeBuffer[strlen(code)] = 0x0D;
   fcodeWriteBuffer(0u);
}

/* ---- tests -------------------------------------------------------------- */

/* Every VP mode, with the player open and closed: the mixer gate is set,
   the player's plane is never asserted from here. */
static void test_vp_leaves_plane0_to_the_player(void)
{
   static const char *modes[] = { "VP1", "VP2", "VP3", "VP4", "VP5" };
   for (int open = 0; open < 2; open++) {
      player_open = open != 0;
      for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
         plane0_enables = 0;
         plane0_gate = -1;
         send(modes[i]);
         CHECK(plane0_enables == 0, "%s (player %s) set the player's plane %d time(s)",
               modes[i], open ? "open" : "closed", plane0_enables);
         int want_gate = (i == 1u) ? 1 : 0;       /* VP2: computer only */
         CHECK(plane0_gate == want_gate, "%s (player %s): plane 0 gate %d, want %d",
               modes[i], open ? "open" : "closed", plane0_gate, want_gate);
      }
   }
}

/* VPX answers the last mode set. */
static void test_vpx_reports_mode(void)
{
   send("VP4");
   send("VPX");
   CHECK(scsiFcodeBufferRX[0] == 'V' && scsiFcodeBufferRX[1] == 'P' &&
         scsiFcodeBufferRX[2] == '4' && scsiFcodeBufferRX[3] == 0x0D,
         "VPX answered %c%c%c", scsiFcodeBufferRX[0], scsiFcodeBufferRX[1], scsiFcodeBufferRX[2]);
}

int main(void)
{
   test_vp_leaves_plane0_to_the_player();
   test_vpx_reports_mode();
   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("FCODE TESTS PASSED\n");
   return failures ? 1 : 0;
}
