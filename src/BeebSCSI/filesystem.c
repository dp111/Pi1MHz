/************************************************************************
   filesystem.c

   BeebSCSI filing system functions
    BeebSCSI - BBC Micro SCSI Drive Emulator
    Copyright (C) 2018 Simon Inns

   This file is part of BeebSCSI.

    BeebSCSI is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

   Email: simon.inns@gmail.com

************************************************************************/

/*
 The code has been changed to support all Luns to be open at the same time.
 This actually has simplified code in places.

 The code has be refactored in places to simplify it.

 A number of global variable have become local.

 Unreachable code has been removed.

 Some duplicate code has been removed

 Write buffering has been introduced to increase performance

 Buffers have been increased to 16K to reduced SDCARD access and make use of
 multiblock accesses

 Bugs around testing for errors on dsc file lengths being 22 bytes fixed.

 Starting ADFS with F break now doesn't start any LUNs so SCSIJUKE can be the first command

 Fall back to slow seeking if the LUN is too fragmented instead of just failing over.

 Add using BeebVFS for the VFS LUNs

 altered to allow variable sizes of Sectors per Track. This supports drives that have been
 connected on a ACB4070 which uses RLL encoding and allows a larger sector per track size.
 The default is 33 sectors per track (MFM)
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "debug.h"
#include "scsi.h"
#include "fatfs/ff.h"
#include "filesystem.h"
#include "../videoplayer.h"
#include "../config.h"			/* Beeb_write_protect */
#include "../rpi/rpi.h"
#include "../rpi/systimer.h"
#include "../rpi/fileparser.h"


static const parserkey scsiattributes[] = {
   { "Title"           , 0 , 39 , STRING},
   { "Description"     , 0 ,255 , STRING },
   { "Inquiry"         , 0 ,101 , NUMSTRING },
   { "ModeParamHeader" , 0 ,  4 , NUMSTRING },
   { "LBADescriptor"   , 0 ,  8 , NUMSTRING },
   { "ModePage0"       , 0 ,  10 , NUMSTRING},
   { "ModePage1"       , 0 ,  5 , NUMSTRING},
   { "ModePage3"       , 0 , 23 , NUMSTRING },
   { "WritPage3"       , 0 , 23 , NUMSTRING },
   { "ModePage4"       , 0 ,  6 , NUMSTRING},
   { "ModePage32"      , 0 , 10 , NUMSTRING},
   { "ModePage33"      , 0 ,  9 , NUMSTRING },
   { "ModePage35"      , 0 ,  3 , NUMSTRING},
   { "ModePage36"      , 0 ,  4 , NUMSTRING},
   { "ModePage37"      , 0 ,  6 , NUMSTRING},
   { "ModePage38"      , 0 ,  6 , NUMSTRING},
   { "LDUserCode"      , 0 ,  4 , STRING },
   { "LDVideoXoffset"  , -768 , 768 , INTEGER },
   { NULL , 0 ,0, 0} // end of list
};

#define NUM_KEYS (sizeof(scsiattributes)/sizeof(parserkey))

/* The public enum (filesystem.h) indexes this table: pin the ordering */
_Static_assert(LDVIDEOXOFFSET + 2 == NUM_KEYS,
               "parserkeyvalueenum out of step with scsiattributes[]");

/* One-side cache of a scsi0.cfg's text, so the menu's ?T and ?Y for the
   same side share a single parse - see filesystemReadVFSCfgTextDir(). */
static int16_t vfs_cfg_dir = -1;
static char    vfs_cfg_title[128];
static char    vfs_cfg_desc[240];

/* VFS volume-present cache - see filesystemVFSVolumePresent() */
static int16_t vfs_vol_cached_dir = -1;
static bool    vfs_vol_present;

// File system state structure
//NOINIT_SECTION
static struct filesystemStateStruct
{
   FATFS fsObject;                     // FAT FS file system object
   FIL fileObject[MAX_LUNS];           // FAT FS file objects
   DWORD *clmt[MAX_LUNS];              // fast-seek link maps, grown to fit by filesystemAttachLinkMap
   uint32_t clmtEntries[MAX_LUNS];

   bool fsMountState;                  // File system mount state (true = mounted, false = dismounted)

   uint8_t lunDirectory;               // Current LUN directory ID
   uint8_t lunDirectoryVFS;            // Current LUN directory ID for VFS
   bool fsLunStatus[MAX_LUNS];         // LUN image availability flags for the currently selected LUN directory (true = started, false = stopped)
	struct HDGeometry fsLunGeometry[MAX_LUNS];   // Keep the geometry details for each LUN
   parserkeyvalue keyvalues[MAX_LUNS][NUM_KEYS];   // keys from .cfg file for each LUN
} filesystemState;

NOINIT_SECTION static char fileName[256];       // String for storing LFN filename
NOINIT_SECTION static char fatDirectory[256];      // String for storing FAT directory (for FAT transfer operations)

NOINIT_SECTION static uint8_t sectorBuffer[SECTOR_BUFFER_SIZE];   // Buffer for reading sectors

// Globals for multi-sector reading
static uint8_t sectorsInBuffer = 0;
static uint8_t currentBufferSector = 0;
static uint32_t sectorsRemaining = 0;

#ifdef DEBUG
/* Bad-FS-map hunt: when a LUN read starts at sector 0, verify the ADFS map
   checksums of the bytes we are about to serve and report on serial - lets a
   headless session see whether the Pi served a valid map on each mount. */
static bool map_check_pending;
static uint8_t adfs_map_cksum(const uint8_t *sec)
{
   uint32_t a = 255;
   for (int i = 254; i >= 0; i--) {
      if (a > 255) a = (a + 1u) & 0xFFu;
      a += sec[i];
   }
   return (uint8_t)(a & 0xFFu);
}
#endif

NOINIT_SECTION static FIL fileObjectFAT;

static bool filesystemCheckLunDirectory(uint8_t lunDirectory, uint8_t lunNumber);

static void filesystemPrintfserror(FRESULT fsResult)
{
   switch(fsResult) {
      case FR_DISK_ERR:
      debugString_P(PSTR("FR_DISK_ERR\r\n"));
      break;

      case FR_INT_ERR:
      debugString_P(PSTR("FR_INT_ERR\r\n"));
      break;

      case FR_INVALID_NAME:
      debugString_P(PSTR("FR_INVALID_NAME\r\n"));
      break;

      case FR_INVALID_OBJECT:
      debugString_P(PSTR("FR_INVALID_OBJECT\r\n"));
      break;

      case FR_INVALID_DRIVE:
      debugString_P(PSTR("FR_INVALID_DRIVE\r\n"));
      break;

      case FR_NOT_ENABLED:
      debugString_P(PSTR("FR_NOT_ENABLED\r\n"));
      break;

      case FR_NO_FILESYSTEM:
      debugString_P(PSTR("FR_NO_FILESYSTEM\r\n"));
      break;

      case FR_TIMEOUT:
      debugString_P(PSTR("FR_TIMEOUT\r\n"));
      break;

      case FR_NOT_ENOUGH_CORE:
      debugString_P(PSTR("FR_NOT_ENOUGH_CORE\r\n"));
      break;

      case FR_TOO_MANY_OPEN_FILES:
      debugString_P(PSTR("FR_TOO_MANY_OPEN_FILES\r\n"));
      break;

      case FR_NOT_READY:
      debugString_P(PSTR("FR_NOT_READY\r\n"));
      break;

      case FR_NO_PATH:
      debugString_P(PSTR("FR_NO_PATH\r\n"));
      break;

      case FR_DENIED:
      debugString_P(PSTR("FR_DENIED\r\n"));
      break;

      case FR_EXIST:
      debugString_P(PSTR("FR_EXIST\r\n"));
      break;

      case FR_WRITE_PROTECTED:
      debugString_P(PSTR("FR_WRITE_PROTECTED\r\n"));
      break;

      case FR_LOCKED:
      debugString_P(PSTR("FR_LOCKED\r\n"));
      break;

      case FR_NO_FILE:
      debugString_P(PSTR("FR_NO_FILE\r\n"));
      break;

      case FR_OK: // This can't happen as only errors are passed in
      case FR_MKFS_ABORTED:
      case FR_INVALID_PARAMETER:
      default:
      debugString_P(PSTR("unknown error\r\n"));
      break;
   }
}

// Function to initialise the file system control functions (called on a cold-start of the AVR)
void filesystemInitialise(uint8_t scsijuke)
{
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemInitialise(): Initialising file system\r\n"));
   filesystemState.lunDirectory = scsijuke;      // Default to LUN directory 0
   filesystemState.fsMountState = false;  // FS default state is unmounted
}

// Function to initialise the file system control functions (called on a cold-start of the AVR)
void filesystemInitialiseVFS(uint8_t vfsjuke)
{
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemInitialiseVFS(): Initialising file system\r\n"));
   filesystemState.lunDirectoryVFS = vfsjuke;      // Default to LUN directory 0
}

// Reset the file system (called when the host signals reset)
void filesystemReset(void)
{
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReset(): Resetting file system\r\n"));

   // Reset the default FAT transfer directory
   snprintf(fatDirectory, sizeof(fatDirectory), "/Transfer");
   vfs_vol_cached_dir = -1;          /* re-stat the VFS volume marker */

   // ensure the file-system is closed on reset
   filesystemDismount();
   // Now Mount the filesystem
   filesystemMount();
}

// File system mount and dismount functions --------------------------------------------------------------------

// Function to mount the file system
 bool filesystemMount(void)
{
   FRESULT fsResult;

   // Already mounted?  Do nothing.
   //
   // f_mount() on an already-registered volume clears fs_type, which defeats
   // mount_volume()'s "already valid, reuse" fast path and forces a full
   // remount ending in "fs->id = ++Fsid".  Every FIL/DIR opened earlier then
   // fails validate() with FR_INVALID_OBJECT - including the per-LUN
   // fileObject[] handles BeebSCSI opens once and holds for the life of the
   // LUN.  fs_ensure_ready() in usb/mtp_fs.c calls this on essentially every
   // MTP operation, so without this guard plugging the Pi into a USB host
   // kills the emulated hard disc until the LUN is stopped and restarted -
   // which ADFS never does on its own.  It also re-read the MBR/BPB on every
   // MTP request.
   if (filesystemState.fsMountState) return true;

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemMount(): Mounting file system\r\n"));

   // Mount the SD card
   fsResult = f_mount(&filesystemState.fsObject, "", 1);

   // Check the result
   if (fsResult != FR_OK) {
      if (debugFlag_filesystem) {
         debugString_P(PSTR("File system: filesystemMount(): ERROR: "));
         filesystemPrintfserror(fsResult);
      }

      // Exit with error status
      filesystemState.fsMountState = false;
      return false;
   }

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemMount(): Successful\r\n"));
   filesystemState.fsMountState = true;

   // Every FIL held across the (dis)mount is invalid, and this is the
   // first moment a reopen can succeed - notify the video player here,
   // never at dismount (a reopen attempted while unmounted fails and the
   // one-shot flag would be spent).
   videoplayer_media_changed();

   // Note: ADFS does not send a SCSI STARTSTOP command on reboot... it assumes that LUN 0 is already started.
   // This is theoretically incorrect... the host should not assume anything about the state of a SCSI LUN.
   // However, in order to support this buggy implementation we have to start LUN 0 here.
  // filesystemSetLunStatus(0, true);

   return true;
}

// Function to dismount the file system
bool filesystemDismount(void)
{

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemDismount(): Dismounting file system\r\n"));

   // Is the file system mounted?
   if (filesystemState.fsMountState == false) {
      // Nothing to do...
      debugString_P(PSTR("File system: filesystemDismount(): No file system to dismount\r\n"));
      return false;
   }

   // Set all LUNs to stopped
   for( uint8_t i=0 ; i < MAX_LUNS; i++ )
   {
      filesystemSetLunStatus(i, false);
      parse_releasekeyvalues(filesystemState.keyvalues[i], NUM_KEYS);
   }
   // Dismount the SD card
     FRESULT fsResult;
   fsResult = f_mount(&filesystemState.fsObject, "", 0);

   // Check the result
   if (fsResult != FR_OK) {
      if (debugFlag_filesystem) {
         debugString_P(PSTR("File system: filesystemDismount(): ERROR: "));
         filesystemPrintfserror(fsResult);
      }

      // Exit with error status
      filesystemState.fsMountState = false;
      return false;
   }

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemDismount(): Successful\r\n"));
   filesystemState.fsMountState = false;
   return true;
}

// Host-side write interlock -------------------------------------------------
//
// A started LUN's image is held open for the life of the LUN, with a cluster
// link map (see filesystemOpenLunForRead()). FatFs does not reference-count:
// unlinking, renaming or truncating a file another FIL still has open leaves
// that FIL addressing clusters something else now owns, and with a cltbl the
// writes bypass the FAT entirely, so nothing detects it. A host-side writer
// (MTP, WebDAV) must therefore refuse to touch the image of a started LUN.
// *BYE on the Beeb issues SCSI STARTSTOP (0x1B), which stops the LUN, closes
// the handle and makes the image writable again.
//
// hostLockMask is the same interlock in the other direction: while a host
// transfer is part way through rewriting an image, the Beeb must not auto-
// start that LUN back onto the half-written file (scsi.c auto-starts any
// stopped LUN on its first access).

static uint16_t hostLockMask;    // bit n set = LUN n is being written by the host
static uint16_t hostRevokeMask;  // bit n set = the Beeb took LUN n back; abort that transfer

// Compare up to `len` bytes case-insensitively; FAT names are not case
// sensitive, and the host hands us whatever case the directory entry has.
static bool fsHostNameEqual(const char *a, const char *b, size_t len)
{
   for (size_t i = 0; i < len; i++) {
      char ca = a[i];
      char cb = b[i];
      if (ca >= 'a' && ca <= 'z') ca = (char)(ca - ('a' - 'A'));
      if (cb >= 'a' && cb <= 'z') cb = (char)(cb - ('a' - 'A'));
      if (ca != cb) return false;
      if (ca == '\0') return true;
   }
   return true;
}

// Assemble a LUN's directory, and the stem its files share, exactly as
// filesystemCheckLunImage() assembles the .dat name.
static void fsHostLunNames(uint8_t lunNumber, char *dir, size_t dirSize,
                           char *stem, size_t stemSize)
{
   if (lunNumber < 8) {
      snprintf(dir, dirSize, "/BeebSCSI%d", filesystemState.lunDirectory);
      snprintf(stem, stemSize, "/BeebSCSI%d/scsi%d.", filesystemState.lunDirectory, lunNumber);
   } else {
      snprintf(dir, dirSize, "/BeebVFS%d", filesystemState.lunDirectoryVFS);
      snprintf(stem, stemSize, "/BeebVFS%d/scsi%d.", filesystemState.lunDirectoryVFS, lunNumber & 7);
   }
}

// True if `path` is a file of a started (or host-locked) LUN, or a directory
// containing one. Deliberately conservative: it matches the whole "scsi<n>."
// stem, so the descriptor and config alongside the image are covered too -
// they are read when the LUN starts, so replacing them under a running LUN is
// the same class of problem.
bool filesystemHostPathBusy(const char *path)
{
   if (path == NULL || path[0] == '\0') return false;

   size_t pathLen = strlen(path);
   while (pathLen > 1u && path[pathLen - 1u] == '/') pathLen--;   // ignore a trailing /

   for (uint8_t lunNumber = 0; lunNumber < MAX_LUNS; lunNumber++) {
      if (!filesystemState.fsLunStatus[lunNumber] && !filesystemLunHostLocked(lunNumber))
         continue;

      char dir[24];
      char stem[40];
      fsHostLunNames(lunNumber, dir, sizeof(dir), stem, sizeof(stem));

      // path names one of this LUN's own files
      size_t stemLen = strlen(stem);
      if (pathLen >= stemLen && fsHostNameEqual(path, stem, stemLen)) return true;

      // path is a directory at or above the one holding them
      size_t dirLen = strlen(dir);
      if (pathLen <= dirLen && fsHostNameEqual(path, dir, pathLen) &&
          (dir[pathLen] == '\0' || dir[pathLen] == '/')) return true;
   }

   return false;
}

// Which LUN's image does `path` name, if any? Unlike filesystemHostPathBusy()
// this ignores LUN status - the caller is about to write the file and wants to
// lock it whether or not the LUN happens to be started.
int8_t filesystemLunFromHostPath(const char *path)
{
   if (path == NULL) return -1;

   for (uint8_t lunNumber = 0; lunNumber < MAX_LUNS; lunNumber++) {
      char dir[24];
      char stem[40];
      fsHostLunNames(lunNumber, dir, sizeof(dir), stem, sizeof(stem));
      if (fsHostNameEqual(path, stem, strlen(stem))) return (int8_t)lunNumber;
   }

   return -1;
}

// Claim/release a LUN for the duration of a host transfer. Every path that
// tears a transfer down must release it, including the aborted ones: a lock
// left set by a host that vanished mid-upload would keep the LUN unstartable
// until the next reboot.
//
// The lock never blocks the Beeb. When the Beeb wants a locked LUN it calls
// filesystemHostRevokeLun(), which hands the LUN straight back and leaves a
// revoked flag behind; the host transfer notices that in its own loop and
// aborts itself. Refusing the Beeb instead is what hung the machine in
// 8d8389e - ADFS's handshake spins on the status register with no timeout, so
// every refusal became a FIQ storm that starved the main loop (see 9177fef).
void filesystemHostLockLun(int8_t lunNumber, bool lock)
{
   if (lunNumber < 0 || lunNumber >= MAX_LUNS) return;

   // Either way the revocation is stale: a new transfer has not been revoked
   // yet, and a finished one has nothing left to abort.
   hostRevokeMask &= (uint16_t)~(1u << (uint8_t)lunNumber);

   if (lock) hostLockMask |= (uint16_t)(1u << (uint8_t)lunNumber);
   else      hostLockMask &= (uint16_t)~(1u << (uint8_t)lunNumber);
}

bool filesystemLunHostLocked(uint8_t lunNumber)
{
   return (lunNumber < MAX_LUNS) && ((hostLockMask & (uint16_t)(1u << lunNumber)) != 0u);
}

// The Beeb wants this LUN. Hand it back immediately and record that the host
// transfer holding it must stop. Called from the SCSI start paths, so it must
// stay trivial: no FatFs, no lwIP, no callbacks into the transfer's own
// module - just two bits. The holder polls filesystemHostLunRevoked() from
// its own loop, where closing files and answering the client is safe.
void filesystemHostRevokeLun(uint8_t lunNumber)
{
   if (lunNumber >= MAX_LUNS) return;
   if ((hostLockMask & (uint16_t)(1u << lunNumber)) == 0u) return;

   hostLockMask   &= (uint16_t)~(1u << lunNumber);
   hostRevokeMask |= (uint16_t)(1u << lunNumber);
}

bool filesystemHostLunRevoked(uint8_t lunNumber)
{
   return (lunNumber < MAX_LUNS) && ((hostRevokeMask & (uint16_t)(1u << lunNumber)) != 0u);
}

// LUN status control functions ---------------------------------------------

// Function to set the status of a LUN image
bool filesystemSetLunStatus(uint8_t lunNumber, bool lunStatus)
{
   // Is the requested status the same as the current status?
   if (filesystemState.fsLunStatus[lunNumber] == lunStatus) {
      if (debugFlag_filesystem) {
         debugStringInt16_P(PSTR("File system: filesystemSetLunStatus(): LUN number "), (uint16_t)lunNumber, false);
         if (filesystemState.fsLunStatus[lunNumber]) {debugString_P(PSTR(" is started\r\n"));}
         else debugString_P(PSTR(" is stopped\r\n"));
      }
      return true;
   }

   // Transitioning from stopped to started?
   if (lunStatus == true) {
      // Is the file system mounted?
      if (filesystemState.fsMountState == false) {
         // Nothing to do...
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemSetLunStatus(): ERROR: No file system mounted - cannot set LUNs to started!\r\n"));
         return false;
      }

      // If the LUN image is starting the file system needs to recheck the LUN and LUN
      // descriptor to ensure everything is up to date

      // Check that the currently selected LUN directory exists (and, if not, create it)
      if (!filesystemCheckLunDirectory(filesystemState.lunDirectory, lunNumber)) {
         // Failed!
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemSetLunStatus(): ERROR: Could not access LUN image directory!\r\n"));
         return false;
      }

      // Check that the LUN image exists
      if (!filesystemCheckLunImage(lunNumber)) {
         // Failed!
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemSetLunStatus(): ERROR: Could not access LUN image file!\r\n"));
         return false;
      }

      // Exit with success
      filesystemState.fsLunStatus[lunNumber] = true;

      if (debugFlag_filesystem) {
         debugStringInt16_P(PSTR("File system: filesystemSetLunStatus(): LUN number "), (uint16_t)lunNumber, false);
         debugString_P(PSTR(" is started\r\n"));
      }

      return true;
   }

   // Transitioning from started to stopped
   f_close(&filesystemState.fileObject[lunNumber]);
   parse_releasekeyvalues(filesystemState.keyvalues[lunNumber], NUM_KEYS);
   filesystemState.fsLunStatus[lunNumber] = false;

   if (debugFlag_filesystem) {
      debugStringInt16_P(PSTR("File system: filesystemSetLunStatus(): LUN number "), (uint16_t)lunNumber, false);
      debugString_P(PSTR(" is stopped\r\n"));
   }

   // Exit with success
   return true;
}

// Function to read the status of a LUN image
bool filesystemReadLunStatus(uint8_t lunNumber)
{
   return filesystemState.fsLunStatus[lunNumber];
}

// Function to confirm that a LUN image is still available
// cppcheck-suppress unusedFunction
bool filesystemTestLunStatus(uint8_t lunNumber)
{
   if (filesystemState.fsLunStatus[lunNumber] == true) {
      // Check that the LUN image exists
      if (!filesystemCheckLunImage(lunNumber)) {
         // Failed!
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemTestLunStatus(): ERROR: Could not access LUN image file!\r\n"));
         return false;
      }
   } else {
      // LUN is not marked as available!
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemTestLunStatus(): LUN status is marked as stopped - cannot test\r\n"));
      return false;
   }

   // LUN tested OK
   return true;
}

// Function to read the user code for the specified LUN image
void filesystemReadLunUserCode(uint8_t lunNumber, uint8_t userCode[5])
{
   int index = LDUSERCODE;
   if (filesystemState.keyvalues[lunNumber][index].v.string)
   {
      userCode[0] = filesystemState.keyvalues[lunNumber][index].v.string[0];
      userCode[1] = filesystemState.keyvalues[lunNumber][index].v.string[1];
      userCode[2] = filesystemState.keyvalues[lunNumber][index].v.string[2];
      userCode[3] = filesystemState.keyvalues[lunNumber][index].v.string[3];
      userCode[4] = filesystemState.keyvalues[lunNumber][index].v.string[4];
      return;
   }
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadLunUserCode(): ERROR: Unable to find LDUserCode in attributes\r\n"));
   userCode[0] = 0;
   userCode[1] = 0;
   userCode[2] = 0;
   userCode[3] = 0;
   userCode[4] = 0;
}

// Check that the currently selected LUN directory exists (and, if not, create it)
static bool filesystemCheckLunDirectory(uint8_t lunDirectory, uint8_t lunNumber)
{
   FRESULT fsResult;
   DIR dirObject;
   // Is the file system mounted?
   if (filesystemState.fsMountState == false) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): ERROR: No file system mounted\r\n"));
      return false;
   }

   // Does a directory exist for the currently selected LUN directory - if not, create it
   // (VFS is a read-only filesystem: check its directory but never create it)
   if (lunNumber < 8 )
      snprintf(fileName, sizeof(fileName), "/BeebSCSI%d", lunDirectory);
   else
      // VFS LUNs live under the VFS jukebox directory (this must match
      // where filesystemCheckLunImage/CheckExtAttributes open the images)
      snprintf(fileName, sizeof(fileName), "/BeebVFS%d", filesystemState.lunDirectoryVFS);

   fsResult = f_opendir(&dirObject, fileName);

   // Did a directory exist?
   if (fsResult == FR_NO_PATH) {

      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): f_opendir returned FR_NO_PATH - Directory does not exist\r\n"));
      if (lunNumber >= 8 || config_beeb_write_protected())
         return false;                        // no LUN-dir auto-create under write-protect
      // Create the LUN image directory - it's not present on the SD card
      // Check the result
      if (f_mkdir(fileName) != FR_OK) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): ERROR: Unable to create LUN directory\r\n"));
         return false;
      }

      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): Created LUN directory entry\r\n"));
      return true;

   }

   f_closedir(&dirObject);
   // Check the result from f_opendir
   if (fsResult != FR_OK) {
      if (debugFlag_filesystem) {
         debugString_P(PSTR("File system: filesystemCheckLunDirectory(): ERROR: f_opendir returned "));
         filesystemPrintfserror(fsResult);
      }
      return false;
   }
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): LUN directory found\r\n"));

   return true;
}

// Function to scan for SCSI LUN image file on the mounted file system
// and check the image is valid.
/* Is the current /BeebVFS<n> a real LaserDisc volume? The marker is its
   video.pvf: no directory, or a directory without one, means "drive
   empty". One f_stat, cached per jukebox directory, so a failing VFS
   *MOUNT gets its CHECK CONDITION in microseconds - the old path re-ran
   the whole cfg/defscsi fallback churn on every retried command and the
   accumulated SD stalls could wedge VFS mid error handshake. */
bool filesystemVFSVolumePresent(void)
{
   bool stale = (int16_t)filesystemState.lunDirectoryVFS != vfs_vol_cached_dir;
   /* An "absent" verdict also expires after a second, so a video.pvf
      uploaded over WebDAV/USB into the CURRENT directory is seen without
      a jukebox or reboot. A "present" verdict never re-stats (zero cost
      on the hot path; deleting the video mid-session is not a case worth
      an SD access per command). */
   if (!stale && !vfs_vol_present) {
      static uint32_t last_stat_us;
      uint32_t now = RPI_GetSystemTime();
      if (now - last_stat_us > 1000000u) {
         last_stat_us = now;
         stale = true;
      }
   }
   if (stale) {
      FILINFO fno;
      snprintf(fileName, sizeof(fileName), "/BeebVFS%d/video.pvf",
               filesystemState.lunDirectoryVFS);
      vfs_vol_present = (f_stat(fileName, &fno) == FR_OK);
      vfs_vol_cached_dir = (int16_t)filesystemState.lunDirectoryVFS;
   }
   return vfs_vol_present;
}

/* Does the current /BeebVFS<n> hold a scsi0.dat? Distinguishes a full
   VFS disc from a video-only volume for the menu's ?V F-code. Uncached -
   only the menu scan asks, once per directory. */
bool filesystemVFSDatPresent(void)
{
   FILINFO fno;
   snprintf(fileName, sizeof(fileName), "/BeebVFS%d/scsi0.dat",
            filesystemState.lunDirectoryVFS);
   return f_stat(fileName, &fno) == FR_OK;
}

/* What /BeebVFS<dir> holds, without jukeboxing to it: 2 = full disc
   (scsi0.dat), 1 = video-only volume (video.pvf), 0 = nothing. The menu's
   rescan asks this for every side; making it take the directory is what
   lets it stop remounting each one just to look. */
uint8_t filesystemVFSDirType(uint8_t dir)
{
   FILINFO fno;
   snprintf(fileName, sizeof(fileName), "/BeebVFS%u/scsi0.dat", dir);
   if (f_stat(fileName, &fno) == FR_OK)
      return 2;
   snprintf(fileName, sizeof(fileName), "/BeebVFS%u/video.pvf", dir);
   if (f_stat(fileName, &fno) == FR_OK)
      return 1;
   return 0;
}

/* The mounted FatFs object, or NULL when no card is mounted.  Read-only
   access to FAT geometry for the webserver's incremental free-space scan. */
FATFS *filesystemGetFsObject(void)
{
   return filesystemState.fsMountState ? &filesystemState.fsObject : NULL;
}

/* Does /BeebVFS<dir> hold mountable media (video.pvf or scsi0.dat)?
   Used to validate an eject/disc-flip target BEFORE latching the jukebox:
   a single-sided disc must not flip into an empty drive. Main loop only
   (f_stat) - never callable from the FIQ poke path. */
bool filesystemVFSDirPresent(uint8_t dir)
{
   FILINFO fno;
   snprintf(fileName, sizeof(fileName), "/BeebVFS%u/video.pvf", dir);
   if (f_stat(fileName, &fno) == FR_OK)
      return true;
   snprintf(fileName, sizeof(fileName), "/BeebVFS%u/scsi0.dat", dir);
   return f_stat(fileName, &fno) == FR_OK;
}

bool filesystemCheckLunImage(uint8_t lunNumber)
{
   uint32_t lunFileSize;
   FRESULT fsResult;
   if (filesystemState.fsLunStatus[lunNumber])
   {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): Lun already open\r\n"));
      return true;
   }

   // A VFS drive is present when its directory holds a video.pvf OR a
   // scsi0.dat - real AIV data sides (e.g. Community South) have no video
   // file, only the data image. Refuse only when neither exists, so an
   // empty/absent directory still reads as "drive empty" instantly.
   if (lunNumber >= 8 && !filesystemVFSVolumePresent() && !filesystemVFSDatPresent())
      return false;

   // Attempt to open the LUN image
   if (lunNumber < 8 )
      snprintf(fileName, sizeof(fileName), "/BeebSCSI%d/scsi%d.dat", filesystemState.lunDirectory, lunNumber);
   else
      snprintf(fileName, sizeof(fileName), "/BeebVFS%d/scsi%d.dat", filesystemState.lunDirectoryVFS, lunNumber & 7);

   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemCheckLunImage(): Checking for (.dat) LUN image "), (uint16_t)lunNumber, 1);
   fsResult = f_open(&filesystemState.fileObject[lunNumber], fileName, FA_READ | FA_WRITE);

   if (fsResult != FR_OK) {
      if (debugFlag_filesystem) {
         if (fsResult == FR_NO_FILE)
         {
            debugString_P(PSTR("File system: filesystemCheckLunImage(): LUN image not found\r\n"));
         } else
         {
            debugString_P(PSTR("File system: filesystemCheckLunImage(): ERROR: f_open on LUN image returned "));
            filesystemPrintfserror(fsResult);
         }
      }

      // Exit with error
      return false;
   }

#if FF_USE_FASTSEEK
   if (!filesystemAttachLinkMap(&filesystemState.fileObject[lunNumber],
                                &filesystemState.clmt[lunNumber],
                                &filesystemState.clmtEntries[lunNumber])) {
          if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): LUN very fragmented falling back to slow seek "));
   }
#endif

   // Opening the LUN image was successful
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): LUN image found\r\n"));

   // Get the size of the LUN image in bytes
   lunFileSize = (uint32_t)f_size(&filesystemState.fileObject[lunNumber]);
   if (debugFlag_filesystem) debugStringInt32_P(PSTR("File system: filesystemCheckLunImage(): LUN size in bytes (according to .dat) = "), lunFileSize, 1);

   // Check that the LUN file size is actually a size which ADFS can support (the number of sectors is limited to a 21 bit number)
   // i.e. a maximum of 0x1FFFFF or 2,097,151 (* 256 bytes per sector = 512Mb = 536,870,656 bytes)
   if (lunFileSize > (((1<<21)) * 256)) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): WARNING: The LUN file size is greater than 512MBytes\r\n"));
   }

   filesystemState.fsLunStatus[lunNumber] = true;

   if (!filesystemReadLunDescriptor( lunNumber)) {

      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): Creating new LUN descriptor\r\n"));

      filesytemdattoconfigGeometry(lunNumber);
   }

   // Calculate the LUN size from the descriptor file

   if (debugFlag_filesystem) debugStringInt32_P(PSTR("File system: filesystemCheckLunImage(): LUN size in bytes (according to cfg) = "), filesystemGetLunTotalBytes(lunNumber), 1);

   // Are the file size and DSC size consistent?
   if (filesystemGetLunTotalBytes(lunNumber) != lunFileSize) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): WARNING: File size and cfg parameters are NOT consistent\r\n"));
   }

   // Exit with success
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): Successful\r\n"));
   return true;
}
// Function to return the LUN image Block size in bytes from the stored geometry
//
uint32_t filesystemGetLunBlockSize( uint8_t lunNumber)
{
	return filesystemState.fsLunGeometry[lunNumber].BlockSize;
}

// Function to return the LUN image heads per cylinder
//
uint32_t filesystemGetheadspercylinder( uint8_t lunNumber)
{
   return filesystemState.fsLunGeometry[lunNumber].Heads;
}


// Function to return the LUN image sectors per track size in bytes from the stored geometry
//
uint32_t filesystemGetLunSPTSize( uint8_t lunNumber)
{
	return filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack;
}

// Function to return the LUN image size in bytes from the stored geometry
//
uint32_t filesystemGetLunTotalBytes( uint8_t lunNumber)
{
	// pointer to the geometry data of the LUN
	struct HDGeometry* ptr = &filesystemState.fsLunGeometry[lunNumber];

	// The drive size (actual data storage) is calculated by the following formula:
	//
	// Tracks = Cylinders * Heads
	// Total Sectors = Tracks * Sectors per Track
	// (the default '33' is because SuperForm uses a 2:1 interleave format with 33 sectors per
	// track (F-2 in the ACB-4000 manual))
	// Total Bytes = Total Sectors * Block Size (block size is normally 256 bytes)
  	return (((*ptr).Heads * (*ptr).Cylinders) * (*ptr).SectorsPerTrack) * (*ptr).BlockSize;
}

// Function to return the LUN image size in sectors from the stored geometry
//
uint32_t filesystemGetLunTotalSectors( uint8_t lunNumber)
{
	// pointer to the geometry data of the LUN
	struct HDGeometry* ptr = &filesystemState.fsLunGeometry[lunNumber];
   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemGetLunTotalSectors(): Cylinders = "), ( uint16_t) (*ptr).Cylinders, 0);
   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemGetLunTotalSectors(): Heads = "), (*ptr).Heads, 0);
   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemGetLunTotalSectors(): SectorsPerTrack = "), (*ptr).SectorsPerTrack, 0);

	// Tracks = Cylinders * Heads
	// Total Sectors = Tracks * Sectors per Track
	return (((*ptr).Cylinders * (*ptr).Heads) * (*ptr).SectorsPerTrack);
}

// Function to return the cylinders and heads from the LUN descriptor file parameters
// into the buffer
//
/* Upstream helper with no caller here; named gate, not deletion, so future
   BeebSCSI diffs stay clean. */
#define BEEBSCSI_GET_CYL_HEADS 0
#if BEEBSCSI_GET_CYL_HEADS
void filesystemGetCylHeads( uint8_t lunNumber, uint8_t *returnbuf)
{
	returnbuf[3] = ((uint8_t)((filesystemState.fsLunGeometry[lunNumber].Cylinders & 0x0000FF00) >> 8));
	returnbuf[4] = ((uint8_t) (filesystemState.fsLunGeometry[lunNumber].Cylinders & 0x000000FF));
	returnbuf[5] = filesystemState.fsLunGeometry[lunNumber].Heads;
}
#endif

// Function to set the current LUN directory (for the LUN jukeboxing functionality)
void filesystemSetLunDirectory(uint8_t hostID, uint8_t lunDirectoryNumber)
{
   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemSetLunDirectory(): setting lun directory\r\n"), lunDirectoryNumber, 0);
   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemSetLunDirectory(): setting scsihostid directory\r\n"), hostID, 0);

   // Change the current LUN directory number
   if (hostID < 16)
      filesystemState.lunDirectory = lunDirectoryNumber;
   else
      filesystemState.lunDirectoryVFS = lunDirectoryNumber;
}

// Function to read the current LUN directory (for the LUN jukeboxing functionality)
uint8_t filesystemGetLunDirectory(void)
{
   return filesystemState.lunDirectory;
}

uint8_t filesystemGetLunDirectoryVFS(void)
{
   return filesystemState.lunDirectoryVFS;
}

// Functions for creating LUNs and LUN descriptors ---------------------------------------------------------------------------

// Beeb_write_protect rule of thumb for the functions below and their write
// siblings: a write to an ALREADY-mounted LUN (WriteNextSector, WriteAttributes,
// FormatLun) must return SUCCESS (true) so a live ADFS never sees a CHECK_COND;
// a first-time CREATE (image/descriptor/directory) returns false, refusing to
// bring a new LUN into existence.  Keep new write paths on the right side of it.
// Function to create a new LUN image (makes an empty .dat file)
bool filesystemCreateLunImage(uint8_t lunNumber)
{
   FRESULT fsResult;
   FIL fileObject;

   /* A VFS volume (/BeebVFS*, LUN >= 8) is read-only LaserDisc media: the
      firmware must never create images there. An ADFS-style format sequence
      once grew 57 MB scsi0.dat files inside video-only directories. */
   if (lunNumber >= 8) return false;

   if (config_beeb_write_protected()) return false;   // no .dat auto-create under write-protect

   if (filesystemCheckLunImage(lunNumber)) {
      // File opened ok - which means it already exists...
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunImage(): .dat already exists - ignoring request to create a new .dat\r\n"));
      return true;
   }

   if (lunNumber >7)
   {
      // VFS doesn't support creating .dat files
      return false;
   }

   // Assemble the .dat file name
   snprintf(fileName, sizeof(fileName), "/BeebSCSI%d/scsi%d.dat", filesystemState.lunDirectory, lunNumber);

   // Create a new .dat file
   fsResult = f_open(&fileObject, fileName, FA_CREATE_NEW | FA_READ | FA_WRITE);
   if (fsResult != FR_OK) {
      // Create .dat file failed
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunImage(): ERROR: Could not create new .dat file!\r\n"));
      return false;
   }

   f_close(&fileObject);
   // LUN .dat file created successfully

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunImage(): Successful\r\n"));
   return true;
}

// Function to create a new LUN descriptor (makes an default .cfg file)
bool filesystemCreateLunDescriptor(uint8_t lunNumber)
{
   if (lunNumber >= 8) return false;   /* VFS volumes are read-only */

   if (lunNumber >7 || config_beeb_write_protected())
   {
      // VFS never creates .cfg; write-protect blocks .cfg auto-create too
      return false;
   }

   // Assemble the .cfg file name
   snprintf(fileName, sizeof(fileName), "/BeebSCSI%d/scsi%d.cfg", filesystemState.lunDirectory, lunNumber);
   // release any values from a previous parse so re-parsing doesn't leak them
   parse_releasekeyvalues(filesystemState.keyvalues[lunNumber], NUM_KEYS);
   if(parse_readfile(fileName, 0, scsiattributes, filesystemState.keyvalues[lunNumber] ))
      return true;

   if (parse_readfile("/Pi1MHz/defscsi.cfg",fileName, scsiattributes, filesystemState.keyvalues[lunNumber] ))
   {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunDescriptor(): Successful\r\n"));
      // now parse the new file
      parse_releasekeyvalues(filesystemState.keyvalues[lunNumber], NUM_KEYS);
      parse_readfile(fileName, 0, scsiattributes, filesystemState.keyvalues[lunNumber]);
      return true;
   }
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunDescriptor(): ERROR: Could not create new .cfg file!\r\n"));
   return false;
}

/* Attach a fast-seek cluster link map, growing it to the size FatFs asks
   for. FatFs's protocol: cltbl[0] = table size in entries; f_lseek(fp,
   CREATE_LINKMAP) builds the map, or fails with FR_NOT_ENOUGH_CORE after
   writing the REQUIRED size into cltbl[0]. Without a map every f_lseek on
   a fragmented file walks the FAT chain from the start - tens of ms. */
bool filesystemAttachLinkMap(FIL *file, DWORD **map, uint32_t *entries)
{
   if (!*map) {
      *entries = 256;
      *map = malloc(*entries * sizeof(DWORD));
   }
   file->cltbl = NULL;
   for (int attempt = 0; *map && attempt < 2; attempt++) {
      (*map)[0] = *entries;
      file->cltbl = *map;
      FRESULT r = f_lseek(file, CREATE_LINKMAP);
      if (r == FR_OK)
         return true;
      file->cltbl = NULL;
      uint32_t needed = (uint32_t)(*map)[0];   /* before free() reuses it */
      if (r == FR_NOT_ENOUGH_CORE && needed > *entries && needed < 65536u) {
         free(*map);
         *entries = needed;
         *map = malloc(*entries * sizeof(DWORD));
         continue;
      }
      break;
   }
   return false;
}

/* Read a single text Key= value ("Title" / "Description") for the disc
   menu, straight from the mounted VFS disc's already-parsed attributes:
   the VFS LUN's mount fills keyvalues[8] from its scsi0.cfg (a BeebVFS
   directory only ever holds scsi0), and the cache cannot be stale -
   every jukebox path is gated on all LUNs being stopped, and stopping
   releases the values. The menu scans discs by jukeboxing to each
   directory and remounting, so no separate file read is needed. */
/* Read a side's Title/Description WITHOUT jukeboxing to it: a jukebox is a
   remount, and the menu's rescan was paying one per side purely to read a
   name. */
bool filesystemReadVFSCfgTextDir(uint8_t dir, enum parserkeyvalueenum key,
                                 char *out, uint32_t maxLen)
{
   if (!maxLen)
      return false;
   out[0] = '\0';

   /* The mounted side's values were parsed at mount - serve that cache. */
   if (dir == (uint8_t)filesystemState.lunDirectoryVFS) {
      const parserkeyvalue *v = &filesystemState.keyvalues[8][key];
      if (v->v.string && v->length) {
         uint32_t n = v->length < maxLen - 1 ? (uint32_t)v->length : maxLen - 1;
         memcpy(out, v->v.string, n);
         out[n] = '\0';
         return true;
      }
   }

   /* Any other side, and a video-only disc (video.pvf, no scsi0.dat) which
      never mounts so its cache never fills: parse that directory's
      scsi0.cfg. No presence check first - parse_readfile simply fails if
      the file is not there, and an f_stat costs the same LFN directory
      walk that made the scan slow in the first place.
      The menu asks ?T then ?Y for the SAME side, so one entry of cache
      makes the pair share a parse. Keeping it to ONE entry means a rescan
      still re-reads every side, so an edited cfg can never be stale by
      more than the side currently being asked about. */
   if ((int16_t)dir != vfs_cfg_dir) {
      parserkeyvalue values[NUM_KEYS] = {0};
      vfs_cfg_dir = (int16_t)dir;
      vfs_cfg_title[0] = '\0';
      vfs_cfg_desc[0]  = '\0';
      snprintf(fileName, sizeof(fileName), "/BeebVFS%u/scsi0.cfg", dir);
      if (parse_readfile(fileName, 0, scsiattributes, values)) {
         if (values[TITLE].v.string && values[TITLE].length) {
            uint32_t n = values[TITLE].length < sizeof(vfs_cfg_title) - 1 ?
                         (uint32_t)values[TITLE].length : sizeof(vfs_cfg_title) - 1;
            memcpy(vfs_cfg_title, values[TITLE].v.string, n);
            vfs_cfg_title[n] = '\0';
         }
         if (values[DESCRIPTION].v.string && values[DESCRIPTION].length) {
            uint32_t n = values[DESCRIPTION].length < sizeof(vfs_cfg_desc) - 1 ?
                         (uint32_t)values[DESCRIPTION].length : sizeof(vfs_cfg_desc) - 1;
            memcpy(vfs_cfg_desc, values[DESCRIPTION].v.string, n);
            vfs_cfg_desc[n] = '\0';
         }
         parse_releasekeyvalues(values, NUM_KEYS);
      }
   }

   const char *src = (key == TITLE) ? vfs_cfg_title : vfs_cfg_desc;
   if (!src[0])
      return false;
   uint32_t n = (uint32_t)strlen(src);
   if (n > maxLen - 1) n = maxLen - 1;
   memcpy(out, src, n);
   out[n] = '\0';
   return true;
}

// Function to read a LUN descriptor
bool filesystemReadLunDescriptor(uint8_t lunNumber)
{
   if (lunNumber >= 8 && !filesystemVFSVolumePresent() && !filesystemVFSDatPresent())
      return false;                  /* drive empty: no cfg churn */
   if (!filesystemCheckExtAttributes(lunNumber))
   {
      FIL fileObject;
      FRESULT fsResult;
      // Check if the LUN descriptor file (.dsc) is present
      if (lunNumber < 8 )
         snprintf(fileName, sizeof(fileName), "/BeebSCSI%d/scsi%d.dsc", filesystemState.lunDirectory, lunNumber);
      else
         // this isn't expected to exist
         snprintf(fileName, sizeof(fileName), "/BeebVFS%d/scsi%d.dsc", filesystemState.lunDirectoryVFS, lunNumber & 7);

      if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemReadLunDescriptor(): Checking for (.dsc) LUN descriptor "), (uint16_t)lunNumber, 1);
      fsResult = f_open(&fileObject, fileName, FA_READ);

      if (fsResult != FR_OK) {
         // LUN descriptor file is not found
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadLunDescriptor(): LUN descriptor not found\r\n"));
         return false;
      } else {
         // LUN descriptor file is present
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadLunDescriptor(): LUN descriptor found\r\n"));

         UINT fsCounter;

         // pointer to the geometry data of the currently selected LUN
         struct HDGeometry* ptr = &filesystemState.fsLunGeometry[lunNumber];

         uint8_t Buffer[22];
         // Read the DSC data
         fsResult = f_read(&fileObject, Buffer, 22, &fsCounter);

         // Check that the file was read OK and is the correct length
         if (fsResult != FR_OK  || fsCounter != 22) {
            // Something went wrong
            if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadLunDescriptor(): ERROR: Could not read .dsc file\r\n"));
            f_close(&fileObject);

            return false;
         }

         // Interpret the DSC information and calculate the LUN size
         if (debugFlag_filesystem) debugLunDescriptor(Buffer);

         // read the parameters into the cache
         (*ptr).BlockSize 	= (((uint32_t)Buffer[9] << 16) |
                              ((uint32_t)Buffer[10] << 8) |
                                 (uint32_t)Buffer[11]);

         (*ptr).Cylinders 	= (((uint32_t)Buffer[13] << 8) |
                                 (uint32_t)Buffer[14]);

         (*ptr).Heads 		=  (uint8_t)Buffer[15];

         (*ptr).SectorsPerTrack = DEFAULT_SECTORS_PER_TRACK;			// .dsc files don't contain sectors per track, always assume the default

         // Note:
         //
         // The drive size (actual data storage) is calculated by the following formula:
         //
         // tracks = heads * cylinders
         // sectors = tracks * 33
         // (the '33' is because SuperForm uses a 2:1 interleave format with 33 sectors per
         // track (F-2 in the ACB-4000 manual))
         // bytes = sectors * block size (block size is always 256 bytes
         filesystemLunToconfigGeometry(lunNumber);
         f_close(&fileObject);
      }
      return true;
   }
   return true;
}

// Function to write a LUN descriptor
bool filesystemWriteAttributes(uint8_t lunNumber)
{
   // Write-protect: ignore the .cfg overwrite but report success (true, NOT
   // the VFS branch's false) so a mounted ADFS never sees a CHECK_COND.
   if (config_beeb_write_protected()) return true;
   if (lunNumber >7)
   {
      // VFS doesn't support write to .cfg files
      return false;
   }

   // Assemble the .cfg file name
   snprintf(fileName, sizeof(fileName), "/BeebSCSI%d/scsi%d.cfg", filesystemState.lunDirectory, lunNumber);

   if (parse_readfile(fileName, fileName, scsiattributes, filesystemState.keyvalues[lunNumber] ))
   {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteAttributes(): .cfg overwritten Successful\r\n"));
      return true;
   }

   // assume no cfg file so create one

   if (parse_readfile("/Pi1MHz/defscsi.cfg",fileName, scsiattributes, filesystemState.keyvalues[lunNumber] ))
   {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteAttributes(): .cfg created Successful\r\n"));
      return true;
   }

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteAttributes(): ERROR: Could not create new .cfg file!\r\n"));
   return false;
}

// Function to format a LUN image
bool filesystemFormatLun(uint8_t lunNumber, uint8_t dataPattern)
{
   FIL fileObject;
   FRESULT fsResult;

   // Write-protect: ignore FORMAT (which would truncate via FA_CREATE_ALWAYS),
   // report success. Gated before any f_open so nothing is destroyed.
   if (config_beeb_write_protected()) return true;

   if (lunNumber >7)
   {
      // VFS doesn't support creating .dat files
      return false;
   }

   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemFormatLun(): Formatting LUN image "), lunNumber, true);

   filesystemSetLunStatus(lunNumber, false );

   if (debugFlag_filesystem) debugStringInt32_P(PSTR("File system: filesystemFormatLun(): Sectors required = "), filesystemGetLunTotalSectors(lunNumber), true);

   // Assemble the .dat file name
   snprintf(fileName, sizeof(fileName), "/BeebSCSI%d/scsi%d.dat", filesystemState.lunDirectory, lunNumber);

   // Note: We are using the expand FAT method to create the LUN image... the dataPattern byte
   // will be ignored.
   // Fill the sector buffer with the required data pattern
   // for (counter = 0; counter < 256; counter++) sectorBuffer[counter] = dataPattern;

   // Create the .dat file (the old .dat file, if present, will be unlinked (i.e. gone forever))

   fsResult = f_open(&fileObject, fileName, FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
   if (fsResult == FR_OK) {
      // Write the required number of sectors to the DAT file
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemFormatLun(): Performing format...\r\n"));

      // If we try to write 512MBs of data to the SD card in 256 byte chunks
      // via SPI it will take a very long time to complete...
      //
      // So instead we use the FAT FS expand command to allocate a file of the required
      // LUN size
      //
      // Note: This allocates a contiguous area for the file which can help to
      // speed up read/write times.  If you would prefer the file to be small
      // and grow as used, just remove the f_expand and the fsResult check.  Every
      // thing will work fine without them.
      //
      // This ignores the data pattern (since the file is only allocated - not
      // actually written).
      fsResult = f_expand(&fileObject /*&filesystemState.fileObject[lunNumber]*/, (FSIZE_t)(filesystemGetLunTotalBytes(lunNumber)), 1);

      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemFormatLun(): Format complete\r\n"));

      // Check that the file was written OK
      if (fsResult != FR_OK) {
         // Something went wrong writing to the .dat
         if (debugFlag_filesystem) debugStringInt8Hex_P(PSTR("File system: filesystemFormatLun(): ERROR: Could not write .dat : \r\n"),fsResult,1);
         return false;
      }
   } else {
      // Something went wrong opening the .dat
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemFormatLun(): ERROR: Could not open .dat\r\n"));
      return false;
   }

   // Formatting successful
   f_close(&fileObject);
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemFormatLun(): Successful\r\n"));
   return true;
}

// Check an extended attributes file is available for that LUN
// and register it
bool filesystemCheckExtAttributes( uint8_t lunNumber)
{
   char extAttributes_fileName[255];
   if (lunNumber <8)
      snprintf(extAttributes_fileName, sizeof(extAttributes_fileName), "/BeebSCSI%d/scsi%d.cfg", filesystemState.lunDirectory, lunNumber);
   else
      snprintf(extAttributes_fileName, sizeof(extAttributes_fileName), "/BeebVFS%d/scsi%d.cfg", filesystemState.lunDirectoryVFS, lunNumber & 7);

   // release any values from a previous parse: this runs on every MODE SENSE
   // and TRANSLATE, and re-parsing without freeing leaked the whole key set
   parse_releasekeyvalues(filesystemState.keyvalues[lunNumber], NUM_KEYS);
   if (parse_readfile(extAttributes_fileName, 0, scsiattributes, filesystemState.keyvalues[lunNumber]))
   {
      // LUN extended attributes file is present
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckExtAttributes: LUN extended attributes file found\r\n"));
      filesystemConfigToLunGeometry(lunNumber);
      return true;
   }
   else
   {
      snprintf(fileName, sizeof(fileName), "/Pi1MHz/defscsi.cfg");
      parse_readfile(fileName,0, scsiattributes, filesystemState.keyvalues[lunNumber]);

      // LUN extended attributes file is not present
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckExtAttributes: LUN extended attributes file not found\r\n"));
      filesystemConfigToLunGeometry(lunNumber);
   }

	return false;
}

// takes lun geometry and updates the keyvalues

void filesystemLunToconfigGeometry(uint8_t lunNumber)
{
   int index;
   // Update the extended attributes from the cached geometry for this LUN
   index = MODEPAGE0;
   if  (!filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemState.keyvalues[lunNumber][index].v.string = malloc(10);
      if (!filesystemState.keyvalues[lunNumber][index].v.string)
      {
         LOG_INFO(PSTR("File system: filesystemLunToconfigGeometry(): ERROR: Unable to allocate memory for MODEPAGE0\r\n"));
         return;   /* alloc failed: do not dereference NULL below */
      }
      filesystemState.keyvalues[lunNumber][index].length = 10;
   }

   filesystemState.keyvalues[lunNumber][index].v.string[0] = 1 ;
   filesystemState.keyvalues[lunNumber][index].v.string[1] = (char)(filesystemState.fsLunGeometry[lunNumber].Cylinders>>8); // cylinder count MSB
   filesystemState.keyvalues[lunNumber][index].v.string[2] = (char)filesystemState.fsLunGeometry[lunNumber].Cylinders; // cylinder count LSB
   filesystemState.keyvalues[lunNumber][index].v.string[3] = filesystemState.fsLunGeometry[lunNumber].Heads; // Heads
   filesystemState.keyvalues[lunNumber][index].v.string[4] = 0x00; // Reduced write current MSB
   filesystemState.keyvalues[lunNumber][index].v.string[5] = 0x80; // Reduced write current LSB
   filesystemState.keyvalues[lunNumber][index].v.string[6] = 0x00; // Write precompensation MSB
   filesystemState.keyvalues[lunNumber][index].v.string[7] = 0x80; // Write precompensation LSB
   filesystemState.keyvalues[lunNumber][index].v.string[8] = 0x00; // Landing zone
   filesystemState.keyvalues[lunNumber][index].v.string[9] = 0x01; // Step pulse count
   if (filesystemState.keyvalues[lunNumber][index].length < 10)
      filesystemState.keyvalues[lunNumber][index].length = 10;

   index = MODEPAGE4;
   if  (!filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemState.keyvalues[lunNumber][index].v.string = malloc(6);
      if (!filesystemState.keyvalues[lunNumber][index].v.string)
      {
         LOG_INFO(PSTR("File system: filesystemLunToconfigGeometry(): ERROR: Unable to allocate memory for MODEPAGE4\r\n"));
         return;   /* alloc failed: do not dereference NULL below */
      }
      filesystemState.keyvalues[lunNumber][index].length = 6;
   }
   filesystemState.keyvalues[lunNumber][index].v.string[0] = 04 ;
   filesystemState.keyvalues[lunNumber][index].v.string[1] = 04 ;
   filesystemState.keyvalues[lunNumber][index].v.string[2] = 0 ;
   filesystemState.keyvalues[lunNumber][index].v.string[3] = (char)(filesystemState.fsLunGeometry[lunNumber].Cylinders>>8); // cylinder count MSB
   filesystemState.keyvalues[lunNumber][index].v.string[4] = (char)filesystemState.fsLunGeometry[lunNumber].Cylinders; // cylinder count LSB
   filesystemState.keyvalues[lunNumber][index].v.string[5] = filesystemState.fsLunGeometry[lunNumber].Heads; // Heads
   if (filesystemState.keyvalues[lunNumber][index].length < 6)
      filesystemState.keyvalues[lunNumber][index].length = 6;
}

void filesytemdattoconfigGeometry(uint8_t lunNumber)
{
      uint32_t lunFileSize = (uint32_t)f_size(&filesystemState.fileObject[lunNumber]);

      lunFileSize = lunFileSize / (filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack * filesystemState.fsLunGeometry[lunNumber].BlockSize);
      uint8_t heads = 16;

      while ((lunFileSize % heads != 0) && heads > 1) heads--;
      uint32_t cylinders = lunFileSize / heads;

      filesystemState.fsLunGeometry[lunNumber].Cylinders = cylinders;
      filesystemState.fsLunGeometry[lunNumber].Heads = heads;

      filesystemLunToconfigGeometry(lunNumber);
}


//
// Transfer key Values from attributes to the LUN geometry
//
void filesystemConfigToLunGeometry(uint8_t lunNumber)
{
   int index = MODEPARAMHEADER;
   if  (!filesystemState.keyvalues[lunNumber][index].v.string)
   {
      // now recreate the ModeParamHeader
      filesystemState.keyvalues[lunNumber][index].v.string = malloc(4);
      if (!filesystemState.keyvalues[lunNumber][index].v.string)
      {
         LOG_INFO(PSTR("File system: filesystemConfigToLunGeometry(): ERROR: Unable to allocate memory for MODEPARAMHEADER\r\n"));
         return;   /* alloc failed: do not dereference NULL below */
      }
      filesystemState.keyvalues[lunNumber][index].v.string[0] = 0x00; // Reserved
      filesystemState.keyvalues[lunNumber][index].v.string[1] = 0x00; // Reserved
      filesystemState.keyvalues[lunNumber][index].v.string[2] = 0x00; // Reserved
      filesystemState.keyvalues[lunNumber][index].v.string[3] = 0x08; // Reserved
      filesystemState.keyvalues[lunNumber][index].length = 4;
   }

	// Update the cached geometry for this LUN from the extended attributes
   index = LBADESCRIPTOR;
   if  (filesystemState.keyvalues[lunNumber][index].v.string)
      {
         filesystemState.fsLunGeometry[lunNumber].BlockSize = (uint32_t)(((uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[5] << 16) +
                                                                         ((uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[6] << 8) +
                                                                          (uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[7]);
      }
      else
      {
         filesystemState.fsLunGeometry[lunNumber].BlockSize = DEFAULT_BLOCK_SIZE;
         //now recreate the LBADescriptor
         filesystemState.keyvalues[lunNumber][index].v.string = malloc(8);
         if (!filesystemState.keyvalues[lunNumber][index].v.string)
         {
            LOG_INFO(PSTR("File system: filesystemConfigToLunGeometry(): ERROR: Unable to allocate memory for LBADescriptor\r\n"));
            return;   /* alloc failed: do not dereference NULL below */
         }
         filesystemState.keyvalues[lunNumber][index].v.string[0] = 0x00; // Reserved
         filesystemState.keyvalues[lunNumber][index].v.string[1] = 0x00; // Reserved
         filesystemState.keyvalues[lunNumber][index].v.string[2] = 0x00; // Reserved
         filesystemState.keyvalues[lunNumber][index].v.string[3] = 0x00; // Reserved
         filesystemState.keyvalues[lunNumber][index].v.string[4] = 0x00; // Reserved
         filesystemState.keyvalues[lunNumber][index].v.string[5] = (char)(DEFAULT_BLOCK_SIZE >>16); // block size MSB
         filesystemState.keyvalues[lunNumber][index].v.string[6] = (char)(DEFAULT_BLOCK_SIZE >>8); // block size
         filesystemState.keyvalues[lunNumber][index].v.string[7] = (char)(DEFAULT_BLOCK_SIZE); // block size LSB
         filesystemState.keyvalues[lunNumber][index].length = 8;
      }

   // A short or hand-edited cfg value decodes as zero - avoid a divide by
   // zero in the geometry calculations
   if (filesystemState.fsLunGeometry[lunNumber].BlockSize == 0)
      filesystemState.fsLunGeometry[lunNumber].BlockSize = DEFAULT_BLOCK_SIZE;

   index = MODEPAGE3;
   if  (filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack = (uint16_t)((((uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[10] << 8) +
                                                                              (uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[11]));
   }
   else
   {
      filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack = DEFAULT_SECTORS_PER_TRACK;
   }
   if (filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack == 0)
      filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack = DEFAULT_SECTORS_PER_TRACK;

   index = MODEPAGE0;
   if  (filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemState.fsLunGeometry[lunNumber].Cylinders = (uint32_t)((((uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[1] << 8) +
                                                                        (uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[2]));
      filesystemState.fsLunGeometry[lunNumber].Heads =     (uint8_t)   (filesystemState.keyvalues[lunNumber][index].v.string[3]);

   }
   else
   {
      // check if page4 is present
      index = MODEPAGE4;
      if  (filesystemState.keyvalues[lunNumber][index].v.string)
      {
               filesystemState.fsLunGeometry[lunNumber].Cylinders = (uint32_t)((((uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[3] << 8) +
                                                                        (uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[4]));
               filesystemState.fsLunGeometry[lunNumber].Heads =     (uint8_t)   (filesystemState.keyvalues[lunNumber][index].v.string[5]);

               filesystemCopyPage4toPage0(lunNumber);
      } else
      {
         // set to default values
         filesytemdattoconfigGeometry(lunNumber);
      }
   }
}


void filesystemCopyPage0toPage4(uint8_t lunNumber)
{
   int index = MODEPAGE0;
   if (filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemState.fsLunGeometry[lunNumber].Cylinders = (uint32_t)((((uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[1] << 8) +
                                                                        (uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[2]));
      filesystemState.fsLunGeometry[lunNumber].Heads =     (uint8_t)   (filesystemState.keyvalues[lunNumber][index].v.string[3]);
      // now recreate Page4
      index = MODEPAGE4;
      if  (filesystemState.keyvalues[lunNumber][index].v.string)
      {
         filesystemState.keyvalues[lunNumber][index].v.string[2] = 0 ;
         filesystemState.keyvalues[lunNumber][index].v.string[3] = (char)(filesystemState.fsLunGeometry[lunNumber].Cylinders>>8); // cylinder count MSB
         filesystemState.keyvalues[lunNumber][index].v.string[4] = (char)filesystemState.fsLunGeometry[lunNumber].Cylinders; // cylinder count LSB
         filesystemState.keyvalues[lunNumber][index].v.string[5] = filesystemState.fsLunGeometry[lunNumber].Heads; // Heads
         if (filesystemState.keyvalues[lunNumber][index].length < 6)
            filesystemState.keyvalues[lunNumber][index].length = 6;
      }
   }
}

void filesystemCopyPage4toPage0(uint8_t lunNumber)
{
   int index = MODEPAGE4;
   if (filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemState.fsLunGeometry[lunNumber].Cylinders = (uint32_t)((((uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[3] << 8) +
                                                                        (uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[4]));
      filesystemState.fsLunGeometry[lunNumber].Heads =     (uint8_t)   (filesystemState.keyvalues[lunNumber][index].v.string[5]);
      // now recreate Page0
      index = MODEPAGE0;
      if  (filesystemState.keyvalues[lunNumber][index].v.string)
      {
         filesystemState.keyvalues[lunNumber][index].v.string[1] = (char)(filesystemState.fsLunGeometry[lunNumber].Cylinders>>8); // cylinder count MSB
         filesystemState.keyvalues[lunNumber][index].v.string[2] = (char)filesystemState.fsLunGeometry[lunNumber].Cylinders; // cylinder count LSB
         filesystemState.keyvalues[lunNumber][index].v.string[3] = filesystemState.fsLunGeometry[lunNumber].Heads; // Heads
         if (filesystemState.keyvalues[lunNumber][index].length < 4)
            filesystemState.keyvalues[lunNumber][index].length = 4;
      }
   }
}

// Functions for reading and writing LUN images --------------------------------------------------------------------

// Function to open a LUN ready for reading
// Note: The read functions use a multi-sector buffer to lower the number of required
// reads from the physical media.  This is to allow more efficient (larger) reads of data.
bool filesystemOpenLunForRead(uint8_t lunNumber, uint32_t startSector, uint32_t requiredNumberOfSectors)
{
   // Move to the correct point in the DAT file
   // Check that the file seek was OK

   // Reads past the end of a growing image must not extend it (a plain
   // f_lseek on a write-mode file allocates clusters): clamp the seek to the
   // file size and let the short read return zeros for the unwritten sectors
   uint32_t seekPoint = startSector * 256;
   if (seekPoint > f_size(&filesystemState.fileObject[lunNumber]))
      seekPoint = (uint32_t)f_size(&filesystemState.fileObject[lunNumber]);

   if (f_lseek(&filesystemState.fileObject[lunNumber], seekPoint) != FR_OK) {
      // Something went wrong with seeking, do not retry
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForRead(): ERROR: Unable to seek to required sector in LUN image file!\r\n"));
      return false;
   }
   sectorsRemaining = requiredNumberOfSectors;
   sectorsInBuffer = 0;
#ifdef DEBUG
   map_check_pending = (startSector == 0 && lunNumber < 8);
#endif

   // Exit with success
   filesystemState.fsLunStatus[lunNumber] = true;
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForRead(): Successful\r\n"));
   return true;
}

// Function to read next sector from a LUN
bool filesystemReadNextSector(uint8_t lunNumber, uint8_t **buffer)
{
   if (sectorsInBuffer == 0)
   {
      uint32_t sectorsToRead = sectorsRemaining;
      FRESULT fsResult;
      UINT fsCounter;
      if (sectorsToRead > SECTOR_BUFFER_LENGTH) sectorsToRead = SECTOR_BUFFER_LENGTH;

      // Read the required data into the sector buffer
      fsResult = f_read(&filesystemState.fileObject[lunNumber], sectorBuffer, sectorsToRead * 256, &fsCounter);

      // Check that the file was read OK
      if (fsResult != FR_OK) {
         // Something went wrong
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadNextSector(): ERROR: Cannot read from LUN image!\r\n"));
         return false;
      }

      // A short read means the host is reading past the end of an image that
      // has not been grown to full geometry yet - unwritten sectors read as
      // zeros rather than whatever the buffer last held
      if (fsCounter < sectorsToRead * 256) {
         memset(sectorBuffer + fsCounter, 0, (sectorsToRead * 256) - fsCounter);
      }

      sectorsInBuffer = (uint8_t)sectorsToRead;
      currentBufferSector = 0;
      sectorsRemaining = sectorsRemaining - sectorsInBuffer;
#ifdef DEBUG
      if (map_check_pending && fsCounter >= 512u) {
         map_check_pending = false;
         uint8_t c0 = adfs_map_cksum(sectorBuffer);
         uint8_t c1 = adfs_map_cksum(sectorBuffer + 256);
         LOG_INFO("MAPCHECK: s0 %02x/%02x s1 %02x/%02x %s\r\n",
                  sectorBuffer[255], c0, sectorBuffer[511], c1,
                  (sectorBuffer[255] == c0 && sectorBuffer[511] == c1) ? "OK" : "BAD");
      }
#endif
   }
   // return pointer to buffer with sector
   *buffer = sectorBuffer + (currentBufferSector * 256);
   currentBufferSector++;
   sectorsInBuffer--;

   // Exit with success
   return true;
}

// Function to close a LUN for reading
bool filesystemCloseLunForRead(uint8_t lunNumber)
{
   filesystemSetLunStatus(lunNumber, false);
   return true;
}

// Function to open a LUN ready for writing
bool filesystemOpenLunForWrite(uint8_t lunNumber, uint32_t startSector, uint32_t requiredNumberOfSectors)
{
   // VFS LUNs (>= 8, the /BeebVFS* images) are READ-ONLY.  Every other
   // mutating entry point already refuses them - filesystemFormatLun,
   // filesystemCreateLunImage, filesystemCreateLunDescriptor and
   // filesystemWriteAttributes all bail on lunNumber > 7 - but the WRITE6
   // path did not, so a host that simply addressed LUN 8-15 (the SCSI id is
   // taken straight off the databus) could overwrite a Domesday image.
   if (lunNumber > 7)
      return false;

#if FF_USE_FASTSEEK
   FIL *fp = &filesystemState.fileObject[lunNumber];

   if (fp->cltbl != 0 && startSector * 256 > f_size(fp)) {
      // Fast seek clips seeks at the current file size and cannot allocate
      // clusters, so a write starting beyond the end of a growing image
      // would land at the wrong offset. Extend the file with fast seek
      // disabled, then rebuild the link map to cover the new clusters.
      FRESULT fsResult;

      fp->cltbl = 0;
      fsResult = f_lseek(fp, startSector * 256);

      if (!filesystemAttachLinkMap(fp, &filesystemState.clmt[lunNumber],
                                   &filesystemState.clmtEntries[lunNumber])) {
         // Too fragmented for the map - fall back to slow seek (as at open)
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForWrite(): LUN very fragmented falling back to slow seek\r\n"));
      }

      // A seek that stopped short of the target means the SD card filled up
      // while the image was being extended
      if (fsResult != FR_OK || f_tell(fp) != startSector * 256) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForWrite(): ERROR: Unable to grow LUN image file!\r\n"));
         return false;
      }
   }
#endif

   // Move to the correct point in the DAT file
   // Check that the file seek was OK
   if (f_lseek(&filesystemState.fileObject[lunNumber], startSector * 256) != FR_OK) {
      // Something went wrong with seeking, do not retry
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForWrite(): ERROR: Unable to seek to required sector in LUN image file!\r\n"));
      return false;
   }

   sectorsRemaining = requiredNumberOfSectors;
   currentBufferSector = 0;

   // Exit with success
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForWrite(): Successful\r\n"));
   return true;
}

// Function to write next sector to a LUN
bool filesystemWriteNextSector(uint8_t lunNumber, uint8_t const buffer[])
{
   // Beeb_write_protect: swallow the sector, report success, write nothing.
   // Returning true (never false) keeps a mounted ADFS from seeing an error.
   if (config_beeb_write_protected()) return true;

   memcpy(sectorBuffer + (currentBufferSector * 256), buffer , 256 );
   currentBufferSector++;

   if ( (currentBufferSector == SECTOR_BUFFER_LENGTH) || ( currentBufferSector == sectorsRemaining)) {
      FRESULT fsResult;
      UINT fsCounter;
      uint32_t sectorsToWrite;

      if (currentBufferSector == SECTOR_BUFFER_LENGTH)
         sectorsToWrite = SECTOR_BUFFER_LENGTH;
      else
         sectorsToWrite = sectorsRemaining;

      // Write the required data
      fsResult = f_write(&filesystemState.fileObject[lunNumber], sectorBuffer, sectorsToWrite * 256, &fsCounter);

#if FF_USE_FASTSEEK
      if (fsResult == FR_OK && fsCounter != sectorsToWrite * 256 &&
          filesystemState.fileObject[lunNumber].cltbl != 0) {
         // In fast seek mode f_write cannot allocate new clusters, so a write
         // that grows the image stops at the end of the mapped cluster chain
         // and reports a short transfer. Grow the file with fast seek
         // disabled, then rebuild the link map to cover the new clusters.
         FIL *fp = &filesystemState.fileObject[lunNumber];
         UINT fsExtended = 0;

         fp->cltbl = 0;
         fsResult = f_write(fp, sectorBuffer + fsCounter, (sectorsToWrite * 256) - fsCounter, &fsExtended);
         fsCounter += fsExtended;

         if (!filesystemAttachLinkMap(fp, &filesystemState.clmt[lunNumber],
                                      &filesystemState.clmtEntries[lunNumber])) {
            // Too fragmented for the map - fall back to slow seek (as at open)
            if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteNextSector(): LUN very fragmented falling back to slow seek\r\n"));
         }
      }
#endif

      // Check that the file was written OK and in full (a short transfer
      // here means the SD card is full)
      if (fsResult != FR_OK || fsCounter != sectorsToWrite * 256) {
         // Something went wrong
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteNextSector(): ERROR: Cannot write to LUN image!\r\n"));
         return false;
      }

      currentBufferSector = 0;
      sectorsRemaining -= sectorsToWrite;

      if (sectorsRemaining == 0)
         f_sync(&filesystemState.fileObject[lunNumber]);
   }
   // Exit with success
   return true;
}

// Function to close a LUN for writing
bool filesystemCloseLunForWrite(uint8_t lunNumber)
{
   filesystemSetLunStatus(lunNumber, false);
   return true;
}

// Functions for reading SCSI attributes -------------------------------------------------------------


char * filesystemGetInquiryData(uint8_t lunNumber, size_t * length)
{
   int index = INQUIRY;
   *length = filesystemState.keyvalues[lunNumber][index].length;
	return filesystemState.keyvalues[lunNumber][index].v.string;
}

char * filesystemGetModeParamHeaderData(uint8_t lunNumber, size_t * length)
{
   int index = MODEPARAMHEADER;
   *length= filesystemState.keyvalues[lunNumber][index].length;
	return filesystemState.keyvalues[lunNumber][index].v.string;
}

char * filesystemGetLBADescriptorData(uint8_t lunNumber, size_t * length)
{
   int index = LBADESCRIPTOR;
   *length= filesystemState.keyvalues[lunNumber][index].length;
	return filesystemState.keyvalues[lunNumber][index].v.string;
}

// takes a page number and returns the index of the mode page

static int filesystemPagetoIndex( uint8_t page)
{
   page = page & 0x3f;
   char page1 , page2;
   if (page >= 10)
   {
      page1 = (char) ( (page/10)+'0');
      page2 = (char) ( (page%10)+'0');
   }
   else
      {
      page1 = (char) (page+'0');
      page2 = 0;
      }

   const char mode[] = {'M','o','d','e','P','a','g','e',page1,page2,0};
   return parse_findindex(mode,scsiattributes);
}

static int filesystemWritetoIndex( uint8_t page)
{
   page = page & 0x3f;
   char page1 , page2;
   if (page >= 10)
   {
      page1 = (char) ( (page/10)+'0');
      page2 = (char) ( (page%10)+'0');
   }
   else
      {
      page1 = (char) (page+'0');
      page2 = 0;
      }

   const char mode[] = {'W','r','i','t','P','a','g','e',page1,page2,0};
   return parse_findindex(mode,scsiattributes);
}

// returns the mode page data for a given page

char * filesystemGetModePageData(uint8_t lunNumber, uint8_t page, size_t * length)
{

   int index = filesystemPagetoIndex(page);
   if (index == -1)
      return 0;
   *length= filesystemState.keyvalues[lunNumber][index].length;
   return filesystemState.keyvalues[lunNumber][index].v.string;
}

// Writes the mode page data for a given page

int filesystemWriteModePageData(uint8_t lunNumber, uint8_t page, uint8_t len, const uint8_t * Buffer)
{
   int index = filesystemPagetoIndex(page);
   if (index == -1)
      return 0;

   int windex = filesystemWritetoIndex(page);
   if (windex != -1 && filesystemState.keyvalues[lunNumber][index].v.string != NULL
                    && filesystemState.keyvalues[lunNumber][windex].v.string != NULL)
      {
         unsigned int i;
         for ( i=0; (i< len) && (i < filesystemState.keyvalues[lunNumber][windex].length ); i++)
         {
            filesystemState.keyvalues[lunNumber][index].v.string[i] = (char) ( (Buffer[i] & filesystemState.keyvalues[lunNumber][windex].v.string[i]) |
                      ((filesystemState.keyvalues[lunNumber][index].v.string[i] & ~filesystemState.keyvalues[lunNumber][windex].v.string[i])));
         }
         if (filesystemState.keyvalues[lunNumber][index].length < i)
            filesystemState.keyvalues[lunNumber][index].length = i;
         return 1;
      }

   // Allocate at least the key's declared maximum (zero filled) so the
   // fixed-offset geometry consumers and the masked write above can never
   // overrun a page the host supplied short
   size_t alloc = len;
   if ((size_t)scsiattributes[index].max > alloc)
      alloc = (size_t)scsiattributes[index].max;
   filesystemState.keyvalues[lunNumber][index].length = len;
   if (filesystemState.keyvalues[lunNumber][index].v.string)
      free(filesystemState.keyvalues[lunNumber][index].v.string);
   filesystemState.keyvalues[lunNumber][index].v.string = malloc(alloc);
   if (filesystemState.keyvalues[lunNumber][index].v.string == NULL) {
      filesystemState.keyvalues[lunNumber][index].length = 0;
      LOG_INFO(PSTR("File system: filesystemWriteModePageData(): ERROR: Unable to allocate memory for mode page data\r\n"));
      return 0; /* allocation failed */
   }
   memset(filesystemState.keyvalues[lunNumber][index].v.string, 0, alloc);
   memcpy(filesystemState.keyvalues[lunNumber][index].v.string, Buffer, len);
   return 1;
}

// Functions for FAT Transfer support --------------

// Change the filesystem's FAT transfer directory
bool filesystemSetFatDirectory(const uint8_t *buffer)
{
   /* copy safely, ensure null-termination */
   strlcpy(fatDirectory, (const char *)buffer, sizeof(fatDirectory));
   if (debugFlag_filesystem) {
      debugString_P(PSTR("File system: filesystemSetFatDirectory(): FAT transfer directory changed to: "));
      debugString(fatDirectory);
      debugString_P(PSTR("\r\n"));
   }
   return true;
}

// Read an entry from the FAT directory and place the information about the entry into the buffer
//
// The buffer format is as follows:
// Byte 0: Status of file (0 = does not exist, 1 = file exists, 2 = directory)
// Byte 1 - 4: Size of file in number of bytes (32-bit)
// Byte 5 - 126: Reserved (0)
// Byte 127- 255: File name string terminated with 0x00 (NULL)
//
bool filesystemGetFatFileInfo(uint32_t fileNumber, uint8_t *buffer)
{
   //uint16_t byteCounter;
   uint32_t fileEntryNumber;
   FRESULT fsResult;
   DIR dirObject;
   FILINFO fsInfo;
   // Is the file system mounted?
   if (filesystemState.fsMountState == false) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemGetFatFileInfo(): ERROR: No file system mounted\r\n"));
      return false;
   }

   // Open the FAT transfer directory
   fsResult = f_opendir(&dirObject, fatDirectory);

   // Did a directory exist?
   if (fsResult == FR_NO_PATH) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckFatDirectory(): f_opendir returned FR_NO_PATH - Directory does not exist\r\n"));
      // Create the FAT transfer directory - it's not present on the SD card
      // Check the result
      if (f_mkdir(fatDirectory) != FR_OK) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): ERROR: Unable to create FAT transfer directory\r\n"));
         return false;
      }
      f_opendir(&dirObject, fatDirectory);

      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckFatDirectory(): Created FAT transfer directory entry\r\n"));
   } else {
      if (fsResult != FR_OK) {
         if (debugFlag_filesystem) {
            debugString_P(PSTR("File system: filesystemCheckFatDirectory(): ERROR: f_opendir returned "));
            filesystemPrintfserror(fsResult);
         }
         return false;
      }
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckFatDirectory(): FAT transfer directory found\r\n"));
   }

   // Get the requested file entry number object
   for (fileEntryNumber = 0; fileEntryNumber <= fileNumber; fileEntryNumber++) {
      fsResult = f_readdir(&dirObject, &fsInfo);

      // Exit on error or end of directory object entries
      if (fsResult != FR_OK || fsInfo.fname[0] == 0) {
         // The requested directory entry does not exist
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemGetFatFileInfo(): Requested directory entry does not exist\r\n"));
         buffer[0] = 0; // file does not exist
         f_closedir(&dirObject);
         return true; // This is a valid (successful) return condition
      }
   }
   if (debugFlag_filesystem) debugStringInt32_P(PSTR("File system: filesystemGetFatFileInfo(): Requested directory entry found for entry number "), fileNumber, true);

   // Is the entry a file or sub-directory?
   if (fsInfo.fattrib & AM_DIR) {
      // Directory
      buffer[0] = 2; // directory entry is a directory
      if (debugFlag_filesystem) {
         debugString_P(PSTR("File system: filesystemGetFatFileInfo(): Directory entry is a directory called "));
         debugString(fsInfo.fname);
         debugString_P(PSTR("\r\n"));
      }
      // Directories always have a file size of 0
      buffer[1] = 0;
      buffer[2] = 0;
      buffer[3] = 0;
      buffer[4] = 0;

   } else {
      // File
      buffer[0] = 1; // directory entry is a file
      if (debugFlag_filesystem) {
         debugString_P(PSTR("File system: filesystemGetFatFileInfo(): Directory entry is a file called "));
         debugString(fsInfo.fname);
         debugString_P(PSTR("\r\n"));
      }

      // Get the directory entry's file size in bytes
      FSIZE_t fileSize = fsInfo.fsize;

      // The maximum supported file size in ADFS is 512Mbytes (524,288 Kbytes or 536,870,912)
      // If the file size is bigger than this, the file must be truncated.
      if (fileSize > 536870912) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemGetFatFileInfo(): Directory entry is > 536870912 bytes... it will be truncated.\r\n"));
         fileSize = 536870912; // Perhaps this limit should be ~500Mbytes, as file-system overhead will prevent 512MB files being stored? Should be stress-tested...
      } else {
         if (debugFlag_filesystem) debugStringInt32_P(PSTR("File system: filesystemGetFatFileInfo(): Directory entry file size (in bytes) is "), (uint32_t)fileSize, true);
      }

      // Convert the file size into a 32 bit number and place it in 4 bytes of the buffer (1-4)
      buffer[1] = (uint8_t)((fileSize & 0xFF000000UL) >> 24);
      buffer[2] = (uint8_t)((fileSize & 0x00FF0000UL) >> 16);
      buffer[3] = (uint8_t)((fileSize & 0x0000FF00UL) >>  8);
      buffer[4] = (uint8_t)((fileSize & 0x000000FFUL));
   }

   // Store the file name of the directory entry in the buffer (limited to 126 characters and NULL (0x00) terminated)
   // Copy the string into the buffer - starting from byte 127
   strlcpy((char*)buffer+127, fsInfo.fname, 127);

   // Close the directory object
   f_closedir(&dirObject);

   return true;
}

// Open a FAT file ready for reading
bool filesystemOpenFatForRead(uint32_t fileNumber, uint32_t blockNumber)
{
   FRESULT fsResult;
   DIR dirObject;
   FILINFO fsInfo;

   // Is the file system mounted?
   if (filesystemState.fsMountState == false) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenFatForRead(): ERROR: No file system mounted\r\n"));
      return false;
   }

   // Open the FAT transfer directory
   fsResult = f_opendir(&dirObject, fatDirectory);

   // Check the open directory action's result
   if (fsResult == FR_OK) {
      uint32_t fileEntryNumber;

      for (fileEntryNumber = 0; fileEntryNumber <= fileNumber; fileEntryNumber++) {
         fsResult = f_readdir(&dirObject, &fsInfo);

         // Exit on error or end of directory object entries
         if (fsResult != FR_OK || fsInfo.fname[0] == 0) {
            // The requested directory entry does not exist
            if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenFatForRead(): Requested directory entry does not exist\r\n"));
            f_closedir(&dirObject);
            return false;
         }
      }

      // Is the entry a file or sub-directory?
      if (fsInfo.fattrib & AM_DIR) {
         // Directory
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenFatForRead(): Requested directory entry was a directory - can not read!\r\n"));
         f_closedir(&dirObject);
         return false;
      } else {
         char tempfileName[514];
         // Assemble the full path name and file name for the requested file
         snprintf(tempfileName, sizeof(tempfileName), "%s/%s", fatDirectory, fsInfo.fname);
         f_closedir(&dirObject);

         // Open the requested file for reading
         fsResult = f_open(&fileObjectFAT, tempfileName, FA_READ);
         if (fsResult != FR_OK) {
            if (debugFlag_filesystem) {
               debugString_P(PSTR("File system: filesystemOpenFatForRead(): ERROR: f_open on FAT file returned "));
               filesystemPrintfserror(fsResult);
            }
            return false;
         }

         // Seek to the correct point in the file
         fsResult  = f_lseek(&fileObjectFAT, blockNumber * 256);
         if (fsResult != FR_OK) {
            if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenFatForRead(): Could not seek to required block number!\r\n"));
            f_close(&fileObjectFAT);
            return false;
         }
      }
   } else {
      // Couldn't open directory object
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenFatForRead(): Could not open transfer directory!\r\n"));
      return false;
   }

   // File opened successfully
   return true;
}

// Read the next block from a FAT file
bool filesystemReadNextFatBlock(uint8_t *buffer)
{
   UINT byteCounter;
   FRESULT fsResult;

   // Read 256 bytes of data into the buffer
   fsResult  = f_read(&fileObjectFAT, buffer, 256, &byteCounter);
   if (fsResult != FR_OK) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadNextFatBlock(): Could not read data from the target file!\r\n"));
      return false;
   }

   if (byteCounter != 256) {

      for (UINT i = byteCounter; i < 256; i++) {
         buffer[i] = 0; // pad the rest of the buffer with zeros
      }
   }

   return true;
}

// Close a FAT file previously opened for reading
bool filesystemCloseFatForRead(void)
{
   f_close(&fileObjectFAT);
   return true;
}

// read a file buffer can be malloced if the address is NULL

uint32_t filesystemReadFile(const char * filename, uint8_t **address, unsigned int max_size)
{
   UINT byteCounter;
   FRESULT fsResult;
   FIL fileObject;
   LOG_DEBUG("filesystemReadFile: %s\n\r", filename);
   if (filesystemState.fsMountState == false) {
         fsResult = f_mount(&filesystemState.fsObject, "", 1);
         if (fsResult != FR_OK) {
            return 0;
         }
   }
   fsResult = f_open(&fileObject, filename, FA_READ);
   if (fsResult != FR_OK) {
      return 0;
   }
   /* Determine file size and clamp requested read size to file size */
   {
      FSIZE_t fileSize = f_size(&fileObject);
      size_t read_len;

      if (*address == NULL) {
         /* allocate buffer to hold entire file, plus a NUL terminator so
            text consumers (fileparser key match / strtol) can never read
            past the end of the allocation */
         *address = malloc((size_t)fileSize + 1u);
         if (*address == NULL) {
            LOG_INFO(PSTR("File system: filesystemReadFile(): ERROR: Unable to allocate memory for file read\r\n"));
            f_close(&fileObject);
            return 0;
         }
         (*address)[fileSize] = 0;
         read_len = (size_t)fileSize;
      } else {
         /* caller provided a buffer length in max_size; clamp to file size */
         read_len = (size_t)max_size;
         if ((FSIZE_t)read_len > fileSize) read_len = (size_t)fileSize;
      }

      fsResult = f_read(&fileObject, *address, read_len, &byteCounter);
      f_close(&fileObject);
      if (fsResult != FR_OK) {
         LOG_DEBUG("filesystemReadFile: f_read failed %d\n\r", fsResult);
         return 0;
      }

      return (uint32_t)byteCounter;
   }
}


// Write a file

uint32_t filesystemWriteFile(const char * filename, const uint8_t *address, uint32_t max_size)
{
   UINT byteCounter;
   FRESULT fsResult;
   FIL fileObject;

   fsResult = f_open(&fileObject, filename, FA_CREATE_ALWAYS |FA_WRITE);
   if (fsResult != FR_OK)
      return 0;

   f_write(&fileObject, address, max_size, &byteCounter);
   f_close(&fileObject);
   return byteCounter;
}