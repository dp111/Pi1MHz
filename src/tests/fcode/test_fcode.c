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
static uint32_t goto_picture;         /* last videoplayer_goto, 0 none */
static char     goto_op;
static uint32_t stop_register;        /* last videoplayer_goto(x, 'S') */
void videoplayer_goto(uint32_t picture, char op)
{
   if (op == 'S') { stop_register = picture; return; }
   goto_picture = picture; goto_op = op;
}
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
static bool stop_reached;              /* the player reached the stop register */
bool videoplayer_take_stop_reached(void) { bool r = stop_reached; stop_reached = false; return r; }
static uint32_t cur_picture = 1u;
uint32_t videoplayer_picture_number(void) { return cur_picture; }

/* A BGB side 2 shaped table: chapters 05 and 06, none before picture 100. */
static bool has_chapters = true;
static int  chapter_display = -1;     /* last C0/C1, -1 none */
bool videoplayer_has_chapters(void) { return has_chapters; }
bool videoplayer_chapter_first(uint32_t chapter, uint32_t *picture)
{
   if (!has_chapters) return false;
   if (chapter == 5u) { *picture = 100u; return true; }
   if (chapter == 6u) { *picture = 18327u; return true; }
   return false;
}
bool videoplayer_chapter_last(uint32_t chapter, uint32_t *picture)
{
   if (!has_chapters) return false;
   if (chapter == 5u) { *picture = 18326u; return true; }
   if (chapter == 6u) { *picture = UINT32_MAX; return true; }   /* the last */
   return false;
}
int videoplayer_chapter_at(uint32_t picture)
{
   if (!has_chapters || picture < 100u) return -1;
   return picture < 18327u ? 5 : 6;
}
void videoplayer_show_chapter_number(bool on) { chapter_display = on ? 1 : 0; }

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

static bool reply_is(const char *want)
{
   size_t n = strlen(want);
   return memcmp(scsiFcodeBufferRX, want, n) == 0 && scsiFcodeBufferRX[n] == 0x0D;
}

/* QxxR / QxxN: a known chapter goes to its first picture with the same op
   and answers A6; anything else answers AN and moves nothing. */
static void test_chapter_goto(void)
{
   static const struct { const char *code; uint32_t pic; char op; const char *reply; } c[] = {
      { "Q05R",  100u,   'R', "A6" },
      { "Q06N",  18327u, 'N', "A6" },
      { "Q5R",   100u,   'R', "A6" },      /* one digit, as the handset */
      { "Q07R",  0u,     0,   "AN" },      /* not on this side */
      { "Q05",   0u,     0,   "AN" },      /* no op */
      { "Q05X",  0u,     0,   "AN" },
      { "QR",    0u,     0,   "AN" },      /* no digits */
      { "Q0506S",0u,     0,   "AN" },      /* list: not supported */
      { "Q07S",  0u,     0,   "AN" },      /* not on this side */
      { "Q005R", 0u,     0,   "AN" },      /* three digits */
   };
   player_open = true;
   for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
      goto_picture = 0u;
      goto_op = 0;
      send(c[i].code);
      CHECK(goto_picture == c[i].pic && goto_op == c[i].op,
            "%s went to %u %c, want %u %c", c[i].code, goto_picture,
            goto_op ? goto_op : '-', c[i].pic, c[i].op ? c[i].op : '-');
      CHECK(reply_is(c[i].reply), "%s answered %c%c, want %s", c[i].code,
            scsiFcodeBufferRX[0], scsiFcodeBufferRX[1], c[i].reply);
   }
   has_chapters = false;
   send("Q05R");
   CHECK(reply_is("AN"), "Q05R with no table answered %c%c", scsiFcodeBufferRX[0], scsiFcodeBufferRX[1]);
   has_chapters = true;
}

/* QxxS, as Domesday National A's FILM drives it: VP1, a read to drop any
   stale reply, QxxS, *AUDIO n (A then B, never read), E1, then a read every
   tick until one starts with A - "film over".  No A may be waiting until the
   player reaches the chapter's last picture.  Also VFS *CHAPTER n (QnS). */
static void test_chapter_play(void)
{
   static const struct { const char *code; uint32_t first, stop; } c[] = {
      { "Q05S", 100u,   18326u },          /* ends before chapter 06 */
      { "Q6S",  18327u, UINT32_MAX },      /* the last: to the disc's end */
   };
   static const char *audio[][2] = { { "A0", "B0" }, { "A1", "B0" }, { "A0", "B1" }, { "A1", "B1" } };
   player_open = true;
   for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
      for (unsigned a = 0; a < 4; a++) {
         goto_picture = 0u; goto_op = 0; stop_register = 0u; stop_reached = false;
         send("VP1");
         fcodeReadBuffer(); fcodeClearBuffer();
         send(c[i].code);
         send(audio[a][0]);
         send(audio[a][1]);
         send("E1");
         CHECK(goto_picture == c[i].first && goto_op == 'N' && stop_register == c[i].stop,
               "%s went to %u %c stop %u, want %u N stop %u", c[i].code, goto_picture,
               goto_op ? goto_op : '-', stop_register, c[i].first, c[i].stop);
         for (int tick = 0; tick < 3; tick++) {
            fcodePoll();
            fcodeReadBuffer();
            CHECK(scsiFcodeBufferRX[0] != 'A', "%s + *AUDIO %u: tick %d read %c before the chapter ended",
                  c[i].code, a, tick, scsiFcodeBufferRX[0]);
            fcodeClearBuffer();
         }
         stop_reached = true;              /* the player reaches the stop */
         fcodePoll();
         fcodeReadBuffer();
         CHECK(reply_is("A2"), "%s + *AUDIO %u: at the chapter end read %.2s, want A2",
               c[i].code, a, (const char *)scsiFcodeBufferRX);
         fcodeClearBuffer();
      }
   }
}

/* ?C: the chapter of the picture on screen, X when there is none. */
static void test_chapter_request(void)
{
   player_open = true;
   cur_picture = 18400u;
   send("?C");
   CHECK(reply_is("C06"), "?C at 18400 answered %.3s", (const char *)scsiFcodeBufferRX);
   cur_picture = 100u;
   send("?C");
   CHECK(reply_is("C05"), "?C at 100 answered %.3s", (const char *)scsiFcodeBufferRX);
   cur_picture = 50u;
   send("?C");
   CHECK(reply_is("X"), "?C before the first chapter answered %.3s", (const char *)scsiFcodeBufferRX);
   player_open = false;
   cur_picture = 18400u;
   send("?C");
   CHECK(reply_is("X"), "?C with the player closed answered %.3s", (const char *)scsiFcodeBufferRX);
   cur_picture = 1u;
}

/* ?P: the second status character carries "chapter numbers exist". */
static void test_status_reports_chapters(void)
{
   has_chapters = true;
   send("?P");
   CHECK(scsiFcodeBufferRX[0] == 'P' && (scsiFcodeBufferRX[2] & 0x04u) != 0u,
         "?P with chapters: %.6s", (const char *)scsiFcodeBufferRX);
   has_chapters = false;
   send("?P");
   CHECK(scsiFcodeBufferRX[0] == 'P' && (scsiFcodeBufferRX[2] & 0x04u) == 0u,
         "?P without chapters: %.6s", (const char *)scsiFcodeBufferRX);
   has_chapters = true;
}

static void test_chapter_display(void)
{
   send("C1");
   CHECK(chapter_display == 1, "C1 -> %d", chapter_display);
   send("C0");
   CHECK(chapter_display == 0, "C0 -> %d", chapter_display);
}

int main(void)
{
   test_chapter_goto();
   test_chapter_play();
   test_chapter_request();
   test_status_reports_chapters();
   test_chapter_display();
   test_vp_leaves_plane0_to_the_player();
   test_vpx_reports_mode();
   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("FCODE TESTS PASSED\n");
   return failures ? 1 : 0;
}
