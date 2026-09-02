/* Host tests for filesystemWriteFileSafe().
 *
 * It stands between every .cfg rewrite, the WiFi profile and the SSH host
 * keys and the card, and its whole job is what happens when something goes
 * wrong half way: the original must survive a failed write, a failed verify
 * or a failed rename, and no .new or .bak may be left lying about.  None of
 * that is reachable on real hardware without pulling the card at the right
 * microsecond, so it is driven here against a stub FatFs with fault
 * injection.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "fatfs/ff.h"
#include "filesystem.h"

/* ---- the stub card ------------------------------------------------------ */

#define MAX_FILES 8
#define MAX_DATA  512

static struct {
   char    name[300];
   uint8_t data[MAX_DATA];
   size_t  len;
   bool    used;
} card[MAX_FILES];

enum { WRITE_NORMAL = 0, WRITE_FAILS, WRITE_SHORT, WRITE_CORRUPTS };
static int  g_write_mode;
static char g_rename_fail_to[300];   /* fail a rename whose target is this */
static int  g_rename_fail_times = 1; /* ...this many times, then let it work */

static int find(const char *name)
{
   for (int i = 0; i < MAX_FILES; i++)
      if (card[i].used && strcmp(card[i].name, name) == 0) return i;
   return -1;
}

static int put(const char *name, const uint8_t *data, size_t len)
{
   int i = find(name);
   if (i < 0)
      for (i = 0; i < MAX_FILES && card[i].used; i++) ;
   if (i >= MAX_FILES || len > MAX_DATA) return -1;
   snprintf(card[i].name, sizeof card[i].name, "%s", name);
   memcpy(card[i].data, data, len);
   card[i].len = len;
   card[i].used = true;
   return i;
}

FRESULT f_open(FIL *fp, const char *path, unsigned char mode)
{
   int i = find(path);
   (void)mode;
   if (i < 0) return FR_NO_FILE;
   fp->slot = i; fp->size = card[i].len; fp->pos = 0;
   return FR_OK;
}
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
   size_t left = card[fp->slot].len - fp->pos;
   UINT n = btr < left ? btr : (UINT)left;
   memcpy(buff, card[fp->slot].data + fp->pos, n);
   fp->pos += n;
   *br = n;
   return FR_OK;
}
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_stat(const char *path, FILINFO *fno)
{
   int i = find(path);
   if (i < 0) return FR_NO_FILE;
   if (fno) fno->fsize = card[i].len;
   return FR_OK;
}
FRESULT f_rename(const char *from, const char *to)
{
   int i = find(from);
   if (g_rename_fail_to[0] && strcmp(to, g_rename_fail_to) == 0
       && g_rename_fail_times != 0) {
      if (g_rename_fail_times > 0) g_rename_fail_times--;
      return FR_DENIED;
   }
   if (i < 0) return FR_NO_FILE;
   if (find(to) >= 0) return FR_EXIST;          /* FatFs will not overwrite */
   snprintf(card[i].name, sizeof card[i].name, "%s", to);
   return FR_OK;
}
FRESULT f_unlink(const char *path)
{
   int i = find(path);
   if (i < 0) return FR_NO_FILE;
   card[i].used = false;
   return FR_OK;
}

uint32_t filesystemWriteFile(const char *filename, const uint8_t *address, uint32_t max_size)
{
   uint8_t copy[MAX_DATA];
   uint32_t len = max_size;
   if (g_write_mode == WRITE_FAILS) return 0;
   if (max_size > MAX_DATA) return 0;
   memcpy(copy, address, max_size);
   if (g_write_mode == WRITE_SHORT && len > 0) len--;          /* truncated  */
   if (g_write_mode == WRITE_CORRUPTS && len > 0) copy[len / 2u] ^= 0xFFu;
   if (put(filename, copy, len) < 0) return 0;
   return max_size;    /* claims success either way - the card lying to us */
}

/* ---- framework ---------------------------------------------------------- */

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

#define TARGET "/Pi1MHz/thing.cfg"
#define NEWF   "/Pi1MHz/thing.cfg.new"
#define BAKF   "/Pi1MHz/thing.cfg.bak"

static void reset(const char *original)
{
   memset(card, 0, sizeof card);
   g_write_mode = WRITE_NORMAL;
   g_rename_fail_to[0] = '\0';
   g_rename_fail_times = 1;
   if (original) put(TARGET, (const uint8_t *)original, strlen(original));
}

static bool holds(const char *name, const char *text)
{
   int i = find(name);
   return i >= 0 && card[i].len == strlen(text)
       && memcmp(card[i].data, text, card[i].len) == 0;
}
static bool tidy(void) { return find(NEWF) < 0 && find(BAKF) < 0; }

int main(void)
{
   const char *newtext = "ssid=Home\n";

   puts("== the good paths ==");
   {
      reset(NULL);
      ok(filesystemWriteFileSafe(TARGET, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "writing where no file exists succeeds");
      ok(holds(TARGET, newtext), "and the target holds the new content");
      ok(tidy(), "leaving no .new or .bak behind");
   }
   {
      reset("ssid=Old\n");
      ok(filesystemWriteFileSafe(TARGET, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "replacing an existing file succeeds");
      ok(holds(TARGET, newtext), "the target holds the new content");
      ok(tidy(), "and neither temporary survives");
   }
   {
      reset("ssid=Old\n");
      put(BAKF, (const uint8_t *)"stale", 5);      /* left by an earlier crash */
      ok(filesystemWriteFileSafe(TARGET, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "a stale .bak from a previous crash does not block the swap");
      ok(holds(TARGET, newtext) && tidy(), "and is cleared away");
   }
   {
      uint8_t binary[] = { 0x00u, 0xFFu, 0x0Au, 0x00u, 0x7Fu };
      reset("x");
      ok(filesystemWriteFileSafe(TARGET, binary, sizeof binary), "binary content writes");
      ok(find(TARGET) >= 0 && card[find(TARGET)].len == sizeof binary
         && memcmp(card[find(TARGET)].data, binary, sizeof binary) == 0,
         "embedded NULs and 0xFF survive the verify");
   }
   {
      reset("x");
      ok(filesystemWriteFileSafe(TARGET, (const uint8_t *)"", 0u), "a zero-length write succeeds");
      ok(find(TARGET) >= 0 && card[find(TARGET)].len == 0 && tidy(),
         "and empties the file");
   }

   puts("== the original must survive ==");
   {
      reset("ssid=Old\n");
      g_write_mode = WRITE_FAILS;
      ok(!filesystemWriteFileSafe(TARGET, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "a failed write is reported");
      ok(holds(TARGET, "ssid=Old\n"), "the original is untouched");
      ok(tidy(), "and no temporary is left");
   }
   {
      reset("ssid=Old\n");
      g_write_mode = WRITE_SHORT;      /* card claims success, stores less */
      ok(!filesystemWriteFileSafe(TARGET, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "a short write is caught by the readback");
      ok(holds(TARGET, "ssid=Old\n") && tidy(), "the original survives it");
   }
   {
      reset("ssid=Old\n");
      g_write_mode = WRITE_CORRUPTS;   /* right length, wrong bytes */
      ok(!filesystemWriteFileSafe(TARGET, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "corrupted bytes are caught by the readback");
      ok(holds(TARGET, "ssid=Old\n") && tidy(), "the original survives that too");
   }
   {
      reset("ssid=Old\n");
      snprintf(g_rename_fail_to, sizeof g_rename_fail_to, "%s", BAKF);
      ok(!filesystemWriteFileSafe(TARGET, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "a failure moving the original aside is reported");
      ok(holds(TARGET, "ssid=Old\n") && tidy(), "and the original stays put");
   }
   {
      /* The swap fails once - a target that momentarily still exists, say -
         and the restore then works. */
      reset("ssid=Old\n");
      snprintf(g_rename_fail_to, sizeof g_rename_fail_to, "%s", TARGET);
      g_rename_fail_times = 1;
      ok(!filesystemWriteFileSafe(TARGET, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "a failure swapping the new file in is reported");
      ok(holds(TARGET, "ssid=Old\n"), "and the original is put back from .bak");
      ok(tidy(), "with no temporary left behind");
   }
   {
      /* The card is refusing renames outright, so even the restore fails.
         Nothing can be put back - but the point of moving the original aside
         rather than deleting it is that the data is still on the card, whole,
         under the .bak name. */
      reset("ssid=Old\n");
      snprintf(g_rename_fail_to, sizeof g_rename_fail_to, "%s", TARGET);
      g_rename_fail_times = -1;                /* fail every time */
      ok(!filesystemWriteFileSafe(TARGET, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "a card refusing every rename is reported");
      ok(find(TARGET) < 0 && holds(BAKF, "ssid=Old\n"),
         "and the original is still on the card, whole, as .bak");
      ok(find(NEWF) < 0, "the incomplete replacement is removed");
   }

   puts("== refusals ==");
   {
      reset("ssid=Old\n");
      ok(!filesystemWriteFileSafe(NULL, (const uint8_t *)newtext, 1u), "a null name is refused");
      ok(!filesystemWriteFileSafe(TARGET, NULL, 1u), "a null buffer is refused");
      ok(holds(TARGET, "ssid=Old\n") && tidy(), "and nothing was touched");
   }
   {
      char longname[300];
      memset(longname, 'a', sizeof longname - 1);
      longname[0] = '/';
      longname[sizeof longname - 1] = '\0';
      reset(NULL);
      ok(!filesystemWriteFileSafe(longname, (const uint8_t *)newtext, (uint32_t)strlen(newtext)),
         "a name too long for the .new suffix is refused");
      ok(find(longname) < 0, "and no file is created");
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   printf(fails ? "SAFEWRITE TESTS FAILED\n" : "SAFEWRITE TESTS PASSED\n");
   return fails != 0;
}
