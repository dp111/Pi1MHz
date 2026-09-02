/* Verified write-and-swap for files whose existing copy matters.
 *
 * Its own translation unit so the host tests can drive it against a stub
 * card: filesystem.c itself pulls in the whole SCSI and FatFs world and
 * cannot be compiled on a PC.  Nothing here knows about LUNs or discs - it
 * is FatFs plus filesystemWriteFile().
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "fatfs/ff.h"
#include "filesystem.h"
#include "../rpi/rpi.h"

/* Read a file back and compare it with what was meant to be written. */
static bool filesystemFileMatches(const char * filename, const uint8_t *address,
                                  uint32_t length)
{
   FIL fileObject;
   uint32_t offset = 0;
   bool ok;

   if (f_open(&fileObject, filename, FA_READ) != FR_OK)
      return false;
   ok = (f_size(&fileObject) == length);
   while (ok && offset < length) {
      uint8_t buffer[256];
      UINT got;
      UINT want = (length - offset) > sizeof buffer
                ? (UINT)sizeof buffer : (UINT)(length - offset);
      if (f_read(&fileObject, buffer, want, &got) != FR_OK || got != want
          || memcmp(buffer, address + offset, want) != 0)
         ok = false;
      else
         offset += got;
   }
   f_close(&fileObject);
   return ok;
}

/* Replace a file without risking the copy already on the card.
 *
 * filesystemWriteFile() opens FA_CREATE_ALWAYS, which truncates what is there
 * before the first byte of the replacement is written: a power cut, a card
 * pulled, or a short write leaves nothing behind.  Survivable for a scratch
 * file; not for a .cfg, where the file being replaced is the only copy of a
 * LUN's geometry or of the WiFi credentials, and losing it can mean a Pi that
 * no longer joins a network or a disc that no longer describes itself.
 *
 * So write "<name>.new", read it back and compare it byte for byte - this
 * card has silently short-changed a write before - and only then swap it in.
 * The previous copy is held as "<name>.bak" across the rename, so even the
 * short window where the canonical name does not exist has two complete
 * files on the card either side of it.  On any failure the original is left
 * exactly as it was.
 *
 * Main loop only, like every other FatFs call here.
 */
bool filesystemWriteFileSafe(const char * filename, const uint8_t *address, uint32_t length)
{
   char newname[256];
   char bakname[256];
   FILINFO fno;
   bool moved_old = false;

   if (filename == NULL || address == NULL)
      return false;
   if (snprintf(newname, sizeof newname, "%s.new", filename) >= (int)sizeof newname
    || snprintf(bakname, sizeof bakname, "%s.bak", filename) >= (int)sizeof bakname)
      return false;

   /* Verify before anything irreversible happens to the original. */
   if (filesystemWriteFile(newname, address, length) != length
       || !filesystemFileMatches(newname, address, length))
      goto fail;

   if (f_stat(filename, &fno) == FR_OK) {
      (void)f_unlink(bakname);                 /* may not exist - do not care */
      if (f_rename(filename, bakname) != FR_OK)
         goto fail;
      moved_old = true;
   }
   if (f_rename(newname, filename) != FR_OK)
      goto fail;
   if (moved_old)
      (void)f_unlink(bakname);
   return true;

fail:
   (void)f_unlink(newname);
   if (moved_old)
      (void)f_rename(bakname, filename);       /* put the original back */
   return false;
}
