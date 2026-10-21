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
#include <strings.h>
#include <stdlib.h>

#include "debug.h"
#include "scsi.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "filesystem.h"
#include "../videoplayer.h"
#include "../config.h"			/* Beeb_write_protect, storage= */
#include "../usb_storage.h"
#include "../rpi/rpi.h"
#include "../rpi/systimer.h"
#include "../rpi/fileparser.h"
#include "../fsroute.h"		/* f_* by path: the card, the USB drive or the SMB share */


/* The longest a vendor mode page (32 to 38) may be, in bytes. */
#define VENDOR_PAGE_MAX 32
/* Pages 3 and 4 as a SCSI drive has them: up to 24 bytes (code, length,
   22 of data), and FORMAT under DriveController=SCSI stores the page a
   host sent (up to 32 bytes, scsi.c) - a smaller limit cut it short at the
   next start, under a length byte that promised the rest.  The old .cfg
   pages (23 and 6 bytes) keep their own length. */
#define DRIVE_PAGE_MAX 32

static const parserkey scsiattributes[] = {
   { "Title"           , 0 , 39 , STRING},
   { "Description"     , 0 ,255 , STRING },
   { "Inquiry"         , 0 ,101 , NUMSTRING },
   { "ModeParamHeader" , 0 ,  4 , NUMSTRING },
   { "LBADescriptor"   , 0 ,  8 , NUMSTRING },
   { "ModePage0"       , 0 ,  10 , NUMSTRING},
   { "ModePage1"       , 0 ,  5 , NUMSTRING},
   { "WritPage1"       , 0 ,  5 , NUMSTRING },
   { "ModePage3"       , 0 , DRIVE_PAGE_MAX , NUMSTRING },
   { "WritPage3"       , 0 , DRIVE_PAGE_MAX , NUMSTRING },
   { "ModePage4"       , 0 , DRIVE_PAGE_MAX , NUMSTRING},
   /* The vendor pages are whatever the drive's own .cfg says, and real
      drives differ: some drives' pages 35, 37 and 38 are 4, 8 and 8
      bytes, which the old limits of 3, 6 and 6 cut short - a page then went
      out with a length byte that promised more than it carried, and the
      Acorn FileStore, which reads pages 37 and 38 for "(C)Acorn", turned the
      drive down.  The page's own length is what is stored and sent, so a
      larger limit costs only the zero-padded allocation. */
   { "ModePage32"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "ModePage33"      , 0 , VENDOR_PAGE_MAX , NUMSTRING },
   { "ModePage35"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "ModePage36"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "ModePage37"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "ModePage38"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   /* Each page's changeable mask (MaskPageN: a set bit may be changed by MODE SELECT; WritPage1
      and WritPage3 are the old names, read when there is no MaskPage line) and factory defaults
      (FDefPageN), and how a host asks for them: PageControl=CCS for a drive with no page control
      field (an early CCS drive answers pages 3D/3E/3F with every page's factory, changeable
      and current values), SCSI (or absent) for page control 1-3. */
   { "MaskPage1"       , 0 , 5 , NUMSTRING},
   { "MaskPage3"       , 0 , DRIVE_PAGE_MAX , NUMSTRING},
   { "MaskPage4"       , 0 , DRIVE_PAGE_MAX , NUMSTRING},
   { "MaskPage32"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "MaskPage33"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "MaskPage35"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "MaskPage36"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "MaskPage37"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "MaskPage38"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "FDefPage1"       , 0 , 5 , NUMSTRING},
   { "FDefPage3"       , 0 , DRIVE_PAGE_MAX , NUMSTRING},
   { "FDefPage4"       , 0 , DRIVE_PAGE_MAX , NUMSTRING},
   { "FDefPage32"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "FDefPage33"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "FDefPage35"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "FDefPage36"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "FDefPage37"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "FDefPage38"      , 0 , VENDOR_PAGE_MAX , NUMSTRING},
   { "PageControl"     , 0 ,  4 , STRING },
   /* A SCSI drive's track layout, TrackFormat=T,O,S in decimal, used by
      FORMAT under DriveController=SCSI (an example drive: 20384,70,1):
        physical sectors per track = T / (bytes per sector + O), rounded
        down; sectors per track = physical - S.
        T, track bytes: usable bytes on one track (the example has 20,484 raw
          bytes per revolution; its firmware keeps the gap at the index
          pulse and a margin for speed variation).
        O, overhead: bytes each sector takes on the disc besides its data -
          sync field, address mark, ID field (cylinder, head, sector, check
          bytes), gap, data sync and address mark, the 4-byte ECC, closing gap.
        S, spare sectors: kept in reserve on every track so a bad sector can
          be remapped on the same track; formatted but never given a block
          number.
      The example: 128/256/512/1024 bytes -> 101/61/34/17 (256: 20384/326 = 62
      physical, 61 used, 306 x 4 x 61 = 74,664 blocks).  No TrackFormat
      line: sectors per track stay as page 3 says. */
   { "TrackFormat"     , 0 , 24 , STRING },
   /* 5: the LaserVision user code carries the '=' (e.g. "1=986").
      At 4 the parser truncated it to "1=98", and filesystemReadLunUserCode
      read string[4] off the end of the 4-byte allocation. */
   { "LDUserCode"      , 0 ,  5 , STRING },
   /* Picture alignment of the Domesday video on the Beeb's raster, in Beeb
      pixels: X in MODE 0 pixels (1/640 of the line, positive = right),
      Y in Beeb rows (1/256 of the screen, positive = down).  Absent = -2,-2. */
   { "LDVideoXoffset"  , -768 , 768 , INTEGER },
   { "LDVideoYoffset"  , -288 , 288 , INTEGER },
   { NULL , 0 ,0, 0} // end of list
};

#define NUM_KEYS (sizeof(scsiattributes)/sizeof(parserkey))

/* The public enum (filesystem.h) indexes this table: pin the ordering */
_Static_assert(LDVIDEOYOFFSET + 2 == NUM_KEYS,
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
   bool fsLunReadOnly[MAX_LUNS];       // ADFS image is AM_RDO, opened FA_READ: Beeb writes to it are swallowed
   bool fsLunBlockSizeInMemory[MAX_LUNS]; // the file's block size cannot be used: 256 is, in memory only
	struct HDGeometry fsLunGeometry[MAX_LUNS];   // Keep the geometry details for each LUN
   parserkeyvalue keyvalues[MAX_LUNS][NUM_KEYS];   // keys from .cfg file for each LUN
   uint32_t driveGeneration;           // changes when LUNs 0-7 are no longer the same drives
} filesystemState;


// The shipped default descriptor, for a LUN whose .cfg is missing: a create
// (filesystemCreateLunDescriptor, filesystemWriteAttributesAdding) copies it
// to the LUN's .cfg; a start or MODE SENSE (filesystemCheckExtAttributes)
// only parses it into memory - mounting never writes.
static const char defaultLunDescriptor[] = "/Pi1MHz/defscsi.cfg";
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

static bool filesystemCheckLunDirectory(uint8_t lunNumber);

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
   /* fsMountState is left alone: it starts false, and by the Harddisc's
      power-on call config_load has mounted the card - clearing it there
      would hide that mount from filesystemDismount, and the remount that
      follows would skip the hooks' dismount half. */
}

// Function to initialise the file system control functions (called on a cold-start of the AVR)
void filesystemInitialiseVFS(uint8_t vfsjuke)
{
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemInitialiseVFS(): Initialising file system\r\n"));
   filesystemState.lunDirectoryVFS = vfsjuke;      // Default to LUN directory 0
}

/* ---- SD card swap ------------------------------------------------------
   filesystemEject() makes the card safe to pull: every subsystem that keeps
   files open closes them (an unsynced write would otherwise be lost, or -
   worse - its cached FAT sectors written to whichever card is in the slot
   next), then the volume is dismounted and the card forgotten.  Until
   filesystemInsert() or a BBC reset nothing may mount it again: USB/MTP and
   others mount on demand.  The hooks are registered by the subsystems
   themselves, so this layer need not know them. */
#define EJECT_HOOKS 8u
static struct {
   bool (*eject)(void);       /* true once nothing of its own is open */
   void (*inserted)(void);    /* a card is mounted again */
} eject_hook[EJECT_HOOKS];
static unsigned int eject_hooks;
static bool fsEjected;

/* Called from init functions, which run again on every BBC reset. */
void filesystemRegisterEject(bool (*eject)(void), void (*inserted)(void))
{
   for (unsigned int i = 0; i < eject_hooks; i++)
      if (eject_hook[i].eject == eject && eject_hook[i].inserted == inserted)
         return;
   if (eject_hooks < EJECT_HOOKS) {
      eject_hook[eject_hooks].eject = eject;
      eject_hook[eject_hooks].inserted = inserted;
      eject_hooks++;
   }
}

/* One step: call it until it returns true (a recording still being written
   out makes a subsystem wait). */
bool filesystemEject(void)
{
   bool ready = true;
   fsEjected = true;
   for (unsigned int i = 0; i < eject_hooks; i++)
      if (eject_hook[i].eject && !eject_hook[i].eject())
         ready = false;
   if (!ready)
      return false;
   if (filesystemState.fsMountState)
      (void)filesystemDismount();
   /* filesystemDismount only re-registers the volume, which FatFs mounts
      again on the next f_open from anyone (the webserver does) - and that
      would identify whatever card is in the slot.  Unregister it: until
      filesystemMount registers it again, FatFs answers FR_NOT_ENABLED. */
   (void)f_mount(NULL, "", 0);
   disk_forget();
   return true;
}

/* Mount whatever card is in the slot now.  False if none could be. */
bool filesystemInsert(void)
{
   filesystemReset();
   return filesystemState.fsMountState;
}

bool filesystemEjected(void)
{
   return fsEjected;
}

/* This layer has the card mounted.  FatFs alone would mount it again on
   any f_open after a dismount; a holder resuming work asks this first. */
bool filesystemMounted(void)
{
   return filesystemState.fsMountState;
}

/* ---- Volume remount ----------------------------------------------------
   Every f_mount() of the volume - filesystemDismount(), and the mount in
   filesystemMount() - makes each FIL opened before it fail validate() for
   good: an unsynced write is dropped, f_close fails, the directory entry
   keeps its old size and the clusters written are orphaned.  A BBC reset
   and the jukebox (filesystemReset), the Beeb's f unmount, and any mount
   while unmounted (MTP, filesystemReadFile) do that with no eject first,
   and none can wait for a subsystem the way an eject does - the BREAK
   budget and a Domesday disc flip are both short.  So a subsystem that
   holds a file open across polls registers here.  The hook runs just
   before each f_mount(), must close what it holds at once, and may reopen
   once filesystemMounted() says the card is mounted.  A reset calls it
   twice (dismount, then mount), and it can precede a mount that fails.

   Kept apart from the eject hooks: those are polled until they say they
   are ready, these are told once and cannot refuse. */
#define REMOUNT_HOOKS 4u
static void (*remount_hook[REMOUNT_HOOKS])(void);
static unsigned int remount_hooks;

/* Called from init functions, which run again on every BBC reset. */
void filesystemRegisterRemount(void (*closing)(void))
{
   for (unsigned int i = 0; i < remount_hooks; i++)
      if (remount_hook[i] == closing)
         return;
   if (remount_hooks < REMOUNT_HOOKS)
      remount_hook[remount_hooks++] = closing;
}

static void filesystemRemountNotify(void)
{
   for (unsigned int i = 0; i < remount_hooks; i++)
      remount_hook[i]();
}

/* ---- Where the Beeb's storage lives ---------------------------------------
   The SD card, a USB flash drive (storage=usb: FatFs volume "1:",
   usb_storage.c) or a network share (storage=smb: "2:", fsroute.c): every
   file the Beeb reaches - the BeebSCSI and BeebVFS directories, the video,
   the FAT transfer directory and, through filesystemStoragePath, the FAT
   service (MMFS), FujiNet and Music 5000 recordings.  Pi1MHz.cfg, the ROMs
   and the kernel stay on the card (the Pi's own /Pi1MHz folder), as do
   WebDAV and MTP.

   Decided at each BBC reset, at the first use after it, and kept until the
   next: a drive plugged in while a program runs is used from the next
   BREAK, never swapped in under it.  The Beeb can choose for itself
   (*FX147,203,4, 5 or 6 - filesystemChooseStorage), which overrides
   storage= until the Pi restarts.  At power-on the drive is still being
   enumerated when the Beeb first asks, so until STORAGE_POWER_ON_WAIT_US
   after the first reset the decision waits for it, running USB meanwhile:
   the Beeb sees a slow answer inside a command - it has no timeouts - never
   the SD card's discs in place of the drive's.  The share waits for the
   WiFi in the same way (smb_client_connect).  Never from the FIQ. */
#define STORAGE_POWER_ON_WAIT_US 5000000u

static enum { ROOT_SD, ROOT_USB, ROOT_SMB, ROOT_UNDECIDED, ROOT_UNDECIDED_SMB } fsRoot;
/* The Beeb's own choice (filesystemChooseStorage), in .bss: until the Pi
   restarts - BREAK keeps it. */
static enum { CHOICE_CONFIG, CHOICE_SD, CHOICE_USB, CHOICE_SMB } fsStorageChoice;
static bool     fsRootBooted;          /* the first reset has set the wait */
static uint64_t fsRootWaitUntil;     /* 64-bit: never wraps back into the window */

static void filesystemStorageReset(void)
{
   if (!fsRootBooted) {
      fsRootBooted = true;
      fsRootWaitUntil = RPI_GetSystemTime64() + STORAGE_POWER_ON_WAIT_US;
   }
   const char *storage = config_get("storage");
   if (fsStorageChoice == CHOICE_SD)
      fsRoot = ROOT_SD;
   else if (fsStorageChoice == CHOICE_USB)
      /* chosen on purpose, so no power-on wait: the drive, if it is there */
      fsRoot = usb_storage_mounted() ? ROOT_USB : ROOT_SD;
   else if (fsStorageChoice == CHOICE_SMB ||
            (storage != NULL && strcasecmp(storage, "smb") == 0))
      /* a logon that failed is tried again here, at every BREAK */
      fsRoot = ROOT_UNDECIDED_SMB;
   else
      fsRoot = (storage != NULL && strcasecmp(storage, "usb") == 0) ? ROOT_UNDECIDED : ROOT_SD;
   /* storage=smb: after a lost link, the next use reconnects */
   fsr_reset();
   /* The card's remount puts its current directory back at the root; the
      drive is not remounted, so do the same for it (no drive: an at once
      FR_NOT_ENABLED or FR_NOT_READY). */
   (void)f_chdir("1:/");
}

bool filesystemChooseStorage(filesystem_storage_t storage)
{
   /* A drive asked for but not there, or a share with no server set:
      refused, and the choice left as it was - a later BREAK must not swap
      to a drive plugged in after.  A share that is set up but cannot be
      reached is chosen all the same (filesystemStorageRoot). */
   if (storage == STORAGE_USB && !usb_storage_mounted())
      return false;
   if (storage == STORAGE_SMB && !fsr_net_configured())
      return false;
   fsStorageChoice = storage == STORAGE_USB ? CHOICE_USB
                   : storage == STORAGE_SMB ? CHOICE_SMB : CHOICE_SD;
   return true;
}

const char *filesystemStorageRoot(void)
{
   if (fsRoot == ROOT_UNDECIDED)
      fsRoot = (usb_storage_mounted() || usb_storage_wait_for_drive(fsRootWaitUntil))
             ? ROOT_USB : ROOT_SD;
   /* The share (fsroute.c, smb/smb_client.c): log on, waiting at power-on
      for the WiFi.  One that cannot be reached leaves the Beeb on the card
      until the next BREAK, which tries again, as a missing drive does.  A
      caller inside the network poll cannot wait for the logon: the share
      stays undecided and that one call fails on it - deciding there would
      leave the Beeb on the card for the whole session. */
   if (fsRoot == ROOT_UNDECIDED_SMB && fsr_net_can_wait())
      fsRoot = fsr_net_connect() ? ROOT_SMB : ROOT_SD;
   return fsRoot == ROOT_USB ? "1:"
        : (fsRoot == ROOT_SMB || fsRoot == ROOT_UNDECIDED_SMB) ? "2:" : "";
}

/* The drive is decided on.  Never decides, so never waits. */
bool filesystemStorageOnUsb(void)
{
   return fsRoot == ROOT_USB;
}

bool filesystemStorageOnSmb(void)
{
   return fsRoot == ROOT_SMB;
}

/* The Beeb's files are not on the card (the drive or the share): the host
   interlocks (WebDAV, MTP), which compare card paths, have nothing of
   theirs to guard.  Never decides, so never waits: undecided answers false,
   the cautious side for a host-write interlock. */
bool filesystemStorageOffCard(void)
{
   return fsRoot == ROOT_USB || fsRoot == ROOT_SMB;
}

// Reset the file system (called when the host signals reset)
void filesystemReset(void)
{
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReset(): Resetting file system\r\n"));

   // Reset the default FAT transfer directory
   snprintf(fatDirectory, sizeof(fatDirectory), "/Transfer");
   vfs_vol_cached_dir = -1;          /* re-stat the VFS volume marker */
   vfs_cfg_dir = -1;                 /* and re-parse the cached side's title: a card swap comes through here */
   filesystemStorageReset();         /* SD card or USB drive: decided again at the next use */

   // ensure the file-system is closed on reset
   filesystemDismount();
   // Now Mount the filesystem - after an eject, whichever card is in now
   bool was_ejected = fsEjected;
   fsEjected = false;
   if (filesystemMount()) {
      if (was_ejected)
         for (unsigned int i = 0; i < eject_hooks; i++)
            if (eject_hook[i].inserted)
               eject_hook[i].inserted();
   } else if (was_ejected) {
      /* No card yet: nothing may keep retrying it - and the failed mount
         left the volume registered, which the next f_open from anyone (the
         webserver) would mount behind this layer's back.  As at eject. */
      fsEjected = true;
      (void)f_mount(NULL, "", 0);
   }
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
   if (fsEjected) return false;          /* until filesystemInsert() or a BBC reset */

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemMount(): Mounting file system\r\n"));

   // Mount the SD card
   filesystemRemountNotify();
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
   filesystemState.driveGeneration++;
   // Dismount the SD card
     FRESULT fsResult;
   filesystemRemountNotify();
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

// Assemble a LUN's directory, and the stem its files share, exactly as
// filesystemCheckLunImage() assembles the .dat name.
// The one place the host-side naming of a LUN's files lives: LUN 0-7 are
// /BeebSCSI<n>/scsi<lun>.<ext> in the SCSI jukebox directory, LUN 8-15 are
// /BeebVFS<n>/scsi<lun&7>.<ext> in the read-only VFS jukebox directory.
// Every open of a .dat/.dsc/.cfg goes through here, so the VFS masking and
// directory choice cannot be forgotten at a call site.  A look into a VFS
// side by its directory number (not a LUN) goes through filesystemVFSFilePath.
// root is the volume prefix: filesystemStorageRoot() for the Beeb's own
// access, "" for the host-side names (WebDAV and MTP see the SD card).
static void fsLunDirPathOn(const char *root, uint8_t lunNumber, char *buf, size_t size)
{
   if (lunNumber < 8)
      snprintf(buf, size, "%s/BeebSCSI%d", root, filesystemState.lunDirectory);
   else
      snprintf(buf, size, "%s/BeebVFS%d", root, filesystemState.lunDirectoryVFS);
}

static void fsLunFilePathOn(const char *root, uint8_t lunNumber, const char *ext,
                            char *buf, size_t size)
{
   if (lunNumber < 8)
      snprintf(buf, size, "%s/BeebSCSI%d/scsi%d.%s", root, filesystemState.lunDirectory, lunNumber, ext);
   else
      snprintf(buf, size, "%s/BeebVFS%d/scsi%d.%s", root, filesystemState.lunDirectoryVFS, lunNumber & 7, ext);
}

static void fsLunFilePath(uint8_t lunNumber, const char *ext, char *buf, size_t size)
{
   fsLunFilePathOn(filesystemStorageRoot(), lunNumber, ext, buf, size);
}

// A named file in /BeebVFS<dir> - any side, not only the jukeboxed one: the
// disc menu and the video player look into a side without mounting it.
void filesystemVFSFilePath(uint8_t dir, const char *name, char *buf, size_t size)
{
   snprintf(buf, size, "%s/BeebVFS%u/%s", filesystemStorageRoot(), dir, name);
}

static void fsHostLunNames(uint8_t lunNumber, char *dir, size_t dirSize,
                           char *stem, size_t stemSize)
{
   fsLunDirPathOn("", lunNumber, dir, dirSize);
   fsLunFilePathOn("", lunNumber, "", stem, stemSize);    // "/BeebSCSI<n>/scsi<lun>."
}

// True if `path` is a file of a started (or host-locked) LUN, or a directory
// containing one. Deliberately conservative: it matches the whole "scsi<n>."
// stem, so the descriptor and config alongside the image are covered too -
// they are read when the LUN starts, so replacing them under a running LUN is
// the same class of problem.
bool filesystemHostPathBusy(const char *path)
{
   if (path == NULL || path[0] == '\0') return false;
   if (filesystemStorageOffCard()) return false;    // the LUNs' files are on the drive or share

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
      if (pathLen >= stemLen && strncasecmp(path, stem, stemLen) == 0) return true;

      // path is a directory at or above the one holding them
      size_t dirLen = strlen(dir);
      if (pathLen <= dirLen && strncasecmp(path, dir, pathLen) == 0 &&
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
   if (filesystemStorageOffCard()) return -1;       // the LUNs' files are on the drive or share

   for (uint8_t lunNumber = 0; lunNumber < MAX_LUNS; lunNumber++) {
      char dir[24];
      char stem[40];
      fsHostLunNames(lunNumber, dir, sizeof(dir), stem, sizeof(stem));
      if (strncasecmp(path, stem, strlen(stem)) == 0) return (int8_t)lunNumber;
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
      if (!filesystemCheckLunDirectory(lunNumber)) {
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
   fsr_close(&filesystemState.fileObject[lunNumber]);
   parse_releasekeyvalues(filesystemState.keyvalues[lunNumber], NUM_KEYS);
   filesystemState.fsLunStatus[lunNumber] = false;

   if (debugFlag_filesystem) {
      debugStringInt16_P(PSTR("File system: filesystemSetLunStatus(): LUN number "), (uint16_t)lunNumber, false);
      debugString_P(PSTR(" is stopped\r\n"));
   }

   // Exit with success
   return true;
}

/* A jukebox swaps one host's LUN directory: stop just that directory's
   eight LUNs (0-7 ADFS, 8-15 VFS) and drop their cached .cfg values, as
   filesystemDismount does for all sixteen - but leave the card mounted, so
   every other file open on it (FAT service, FujiNet, MTP, WebDAV, the
   video) stays valid.  The VFS volume cache is keyed by directory and
   re-checks itself. */
void filesystemStopDirectoryLuns(uint8_t firstLun)
{
   for (uint8_t i = firstLun; i < firstLun + 8u; i++) {
      filesystemSetLunStatus(i, false);
      parse_releasekeyvalues(filesystemState.keyvalues[i], NUM_KEYS);
   }
   if (firstLun < 8)
      filesystemState.driveGeneration++;
}

// Function to read the status of a LUN image
bool filesystemReadLunStatus(uint8_t lunNumber)
{
   return filesystemState.fsLunStatus[lunNumber];
}

/* Upstream function with no caller here; named gate (filesystem.h), not
   deletion, so future BeebSCSI diffs stay clean. */
#if BEEBSCSI_TEST_LUN_STATUS
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
#endif

// Function to read the user code for the specified LUN image
void filesystemReadLunUserCode(uint8_t lunNumber, uint8_t userCode[5])
{
   int index = LDUSERCODE;
   /* Copy at most .length bytes, not a fixed five.  LDUserCode is a STRING
      key and store_string() allocates copylen + 1 - only NUMSTRING allocates
      key->max - so a short value like "LDUserCode=12" gives a 3-byte block
      and reading [2]..[4] ran off the end of the heap, handing adjacent heap
      to the Domesday AIV software as the disc's user code.  The key table's
      comment shows this was found once and "fixed" by bumping max 4->5, which
      only covers values that are exactly five characters long.  Pad the rest
      with zeroes, as the not-found path below does. */
   if (filesystemState.keyvalues[lunNumber][index].v.string)
   {
      size_t have = filesystemState.keyvalues[lunNumber][index].length;
      if (have > 5u) have = 5u;
      for (size_t i = 0; i < have; i++)
         userCode[i] = (uint8_t)filesystemState.keyvalues[lunNumber][index].v.string[i];
      for (size_t i = have; i < 5u; i++)
         userCode[i] = 0;
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
static bool filesystemCheckLunDirectory(uint8_t lunNumber)
{
   char fileName[256];                 // a LUN or side file's name (LFN)
   FRESULT fsResult;
   DIR dirObject;
   // Is the file system mounted?
   if (filesystemState.fsMountState == false) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): ERROR: No file system mounted\r\n"));
      return false;
   }

   // Does a directory exist for the currently selected LUN directory - if not, create it
   // (VFS is a read-only filesystem: check its directory but never create it)
   fsLunDirPathOn(filesystemStorageRoot(), lunNumber, fileName, sizeof(fileName));

   fsResult = fsr_opendir(&dirObject, fileName);

   // Did a directory exist?
   if (fsResult == FR_NO_PATH) {

      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): f_opendir returned FR_NO_PATH - Directory does not exist\r\n"));
      if (lunNumber >= 8 || config_beeb_write_protected())
         return false;                        // no LUN-dir auto-create under write-protect
      // Create the LUN image directory - it's not present on the SD card
      // Check the result
      if (fsr_mkdir(fileName) != FR_OK) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): ERROR: Unable to create LUN directory\r\n"));
         return false;
      }

      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): Created LUN directory entry\r\n"));
      return true;

   }

   fsr_closedir(&dirObject);
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
      char fileName[256];
      filesystemVFSFilePath(filesystemState.lunDirectoryVFS, VFS_VIDEO_FILENAME, fileName, sizeof(fileName));
      vfs_vol_present = (fsr_stat(fileName, &fno) == FR_OK);
      vfs_vol_cached_dir = (int16_t)filesystemState.lunDirectoryVFS;
   }
   return vfs_vol_present;
}

/* Does the current /BeebVFS<n> hold a scsi0.dat? Distinguishes a full
   VFS disc from a video-only volume for the menu's ?V F-code. Uncached -
   only the menu scan asks, once per directory. */
bool filesystemVFSDatPresent(void)
{
   char fileName[256];                 // a LUN or side file's name (LFN)
   FILINFO fno;
   filesystemVFSFilePath(filesystemState.lunDirectoryVFS, "scsi0.dat", fileName, sizeof(fileName));
   return fsr_stat(fileName, &fno) == FR_OK;
}

/* What /BeebVFS<dir> holds, without jukeboxing to it: 2 = full disc
   (scsi0.dat), 1 = video-only volume (video.pvf), 0 = nothing. The menu's
   rescan asks this for every side; making it take the directory is what
   lets it stop remounting each one just to look. */
uint8_t filesystemVFSDirType(uint8_t dir)
{
   char fileName[256];                 // a LUN or side file's name (LFN)
   FILINFO fno;
   filesystemVFSFilePath(dir, "scsi0.dat", fileName, sizeof(fileName));
   if (fsr_stat(fileName, &fno) == FR_OK)
      return 2;
   filesystemVFSFilePath(dir, VFS_VIDEO_FILENAME, fileName, sizeof(fileName));
   if (fsr_stat(fileName, &fno) == FR_OK)
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
   return filesystemVFSDirType(dir) != 0u;
}

bool filesystemCheckLunImage(uint8_t lunNumber)
{
   char fileName[256];                 // a LUN or side file's name (LFN)
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
   fsLunFilePath(lunNumber, "dat", fileName, sizeof(fileName));

   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemCheckLunImage(): Checking for (.dat) LUN image "), (uint16_t)lunNumber, 1);
   /* LUN >= 8 is /BeebVFS read-only media: open it FA_READ.  FatFs returns
      FR_DENIED for a write-mode open of an AM_RDO file, so a user who marked
      the image read-only - the natural thing to do to read-only media, and
      something an MTP or Explorer copy can carry across - could not start the
      LUN at all, and READ6's auto-start handed BAD_FORMAT to a host with no
      timeout.  This is only the Beeb-facing LUN handle; MTP and WebDAV write
      these files through their own paths and are unaffected. */
   fsResult = fsr_open(&filesystemState.fileObject[lunNumber], fileName,
                     (lunNumber >= 8) ? FA_READ : (FA_READ | FA_WRITE));

   /* The same FR_DENIED for an ADFS image marked read-only: start it FA_READ
      rather than hand the host BAD_FORMAT.  Its writes are then swallowed and
      reported as done, as under Beeb_write_protect (fsLunReadOnly). */
   filesystemState.fsLunReadOnly[lunNumber] = false;
   if (fsResult == FR_DENIED && lunNumber < 8) {
      fsResult = fsr_open(&filesystemState.fileObject[lunNumber], fileName, FA_READ);
      filesystemState.fsLunReadOnly[lunNumber] = (fsResult == FR_OK);
   }

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

   // The started flag itself belongs to filesystemSetLunStatus(), the one
   // owner of the stopped<->started transition; this function only opens
   // the image and loads its descriptor on that owner's behalf.
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

// The size of the blocks READ and WRITE move for a LUN: its block size, as
// FORMAT laid it down.  filesystemConfigToLunGeometry has already made it
// one the data path can take (256 on the VFS LUNs), so it is the size READ
// CAPACITY reports too.
uint32_t filesystemGetLunDataBlockSize( uint8_t lunNumber)
{
   return filesystemState.fsLunGeometry[lunNumber].BlockSize;
}

// A block size the data path cannot move (outside 128-2048, or anything on
// a VFS LUN, which are LaserVision's 256-byte blocks) becomes 256, and the
// size used goes into the LBA descriptor too, so READ CAPACITY, MODE SENSE,
// FORMAT and the data path all agree.
static void filesystemUseUsableBlockSize(uint8_t lunNumber)
{
   uint32_t blockSize = filesystemState.fsLunGeometry[lunNumber].BlockSize;
   if ((lunNumber >= 8) || (blockSize < MIN_BLOCK_SIZE) || (blockSize > MAX_BLOCK_SIZE)) {
      if (blockSize != DEFAULT_BLOCK_SIZE)
         filesystemState.fsLunBlockSizeInMemory[lunNumber] = true;   // never saved: see filesystemWriteAttributesAdding
      blockSize = filesystemState.fsLunGeometry[lunNumber].BlockSize = DEFAULT_BLOCK_SIZE;
   }
   if (filesystemState.keyvalues[lunNumber][LBADESCRIPTOR].length >= 8)
   {
      filesystemState.keyvalues[lunNumber][LBADESCRIPTOR].v.string[5] = (char)(blockSize >>16);
      filesystemState.keyvalues[lunNumber][LBADESCRIPTOR].v.string[6] = (char)(blockSize >>8);
      filesystemState.keyvalues[lunNumber][LBADESCRIPTOR].v.string[7] = (char)(blockSize);
   }
}

// Function to return the LUN image heads per cylinder
//
uint32_t filesystemGetheadspercylinder( uint8_t lunNumber)
{
   return filesystemState.fsLunGeometry[lunNumber].Heads;
}


// Function to return the LUN image cylinders
//
uint32_t filesystemGetLunCylinders( uint8_t lunNumber)
{
   return filesystemState.fsLunGeometry[lunNumber].Cylinders;
}

// Changes whenever LUNs 0-7 stop being the same drives: the card is
// dismounted, or the ADFS directory is changed or stopped (the jukebox).
// Anything kept for a drive (scsi.c's pending pages) is stale after it.
uint32_t filesystemGetDriveGeneration(void)
{
   return filesystemState.driveGeneration;
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
/* Upstream helper with no caller here; named gate (filesystem.h), not
   deletion, so future BeebSCSI diffs stay clean. */
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
   if (hostID < 16) {
      // LUNs 0-7 are now another directory's drives
      if (filesystemState.lunDirectory != lunDirectoryNumber)
         filesystemState.driveGeneration++;
      filesystemState.lunDirectory = lunDirectoryNumber;
   } else
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
   char fileName[256];                 // a LUN or side file's name (LFN)
   FRESULT fsResult;
   FIL fileObject;

   /* A VFS volume (/BeebVFS*, LUN >= 8) is read-only LaserDisc media: the
      firmware must never create images there. An ADFS-style format sequence
      once grew 57 MB scsi0.dat files inside video-only directories. */
   if (lunNumber >= 8) return false;

   if (config_beeb_write_protected()) return false;   // no .dat auto-create under write-protect

   // Assemble the .dat file name
   fsLunFilePath(lunNumber, "dat", fileName, sizeof(fileName));

   {
      // Existence is a question for f_stat: filesystemCheckLunImage would
      // open the image and load its descriptor for a LUN that stays stopped,
      // breaking "fileObject open iff started" for the FORMAT path.
      FILINFO fno;
      if (fsr_stat(fileName, &fno) == FR_OK) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunImage(): .dat already exists - ignoring request to create a new .dat\r\n"));
         return true;
      }
   }

   // Create a new .dat file
   fsResult = fsr_open(&fileObject, fileName, FA_CREATE_NEW | FA_READ | FA_WRITE);
   if (fsResult != FR_OK) {
      // Create .dat file failed
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunImage(): ERROR: Could not create new .dat file!\r\n"));
      return false;
   }

   fsr_close(&fileObject);
   // LUN .dat file created successfully

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunImage(): Successful\r\n"));
   return true;
}

// A FORMAT the controller refused, of a LUN that had no image: the empty .dat made for it
// goes again, so a refused command leaves nothing behind.  Only an empty one is removed.
void filesystemRemoveEmptyLunImage(uint8_t lunNumber)
{
   char fileName[256];                 // a LUN or side file's name (LFN)
   FILINFO fno;

   if (lunNumber >= 8 || config_beeb_write_protected())
      return;
   fsLunFilePath(lunNumber, "dat", fileName, sizeof(fileName));
   if ((fsr_stat(fileName, &fno) == FR_OK) && (fno.fsize == 0))
      (void)fsr_unlink(fileName);
}

// Function to create a new LUN descriptor (makes an default .cfg file)
bool filesystemCreateLunDescriptor(uint8_t lunNumber)
{
   char fileName[256];                 // a LUN or side file's name (LFN)
   if (lunNumber >7 || config_beeb_write_protected())
   {
      // VFS never creates .cfg; write-protect blocks .cfg auto-create too
      return false;
   }

   // Assemble the .cfg file name
   fsLunFilePath(lunNumber, "cfg", fileName, sizeof(fileName));
   // release any values from a previous parse so re-parsing doesn't leak them
   parse_releasekeyvalues(filesystemState.keyvalues[lunNumber], NUM_KEYS);
   if(parse_readfile(fileName, 0, scsiattributes, filesystemState.keyvalues[lunNumber] ))
      return true;

   if (parse_readfile(defaultLunDescriptor, fileName, scsiattributes, filesystemState.keyvalues[lunNumber] ))
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
   for. FatFs's protocol: cltbl[0] = table size in entries; fsr_lseek(fp,
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
      FRESULT r = fsr_lseek(file, CREATE_LINKMAP);
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

/* An INTEGER key (LDVideoXoffset / LDVideoYoffset) for a side, by the same
   two routes as the text reader below: the mounted side's parsed cache, or
   one parse of that directory's scsi0.cfg.  The video player asks at every
   open and media change, so this is rare and a fresh parse is fine. */
bool filesystemReadVFSCfgIntDir(uint8_t dir, enum parserkeyvalueenum key, int *out)
{
   char fileName[256];                 // a LUN or side file's name (LFN)
   if (dir == (uint8_t)filesystemState.lunDirectoryVFS) {
      const parserkeyvalue *v = &filesystemState.keyvalues[8][key];
      if (v->v.integer && v->length) {
         *out = *v->v.integer;
         return true;
      }
   }
   bool found = false;
   parserkeyvalue values[NUM_KEYS] = {0};
   filesystemVFSFilePath(dir, "scsi0.cfg", fileName, sizeof(fileName));
   if (parse_readfile(fileName, 0, scsiattributes, values)) {
      if (values[key].v.integer && values[key].length) {
         *out = *values[key].v.integer;
         found = true;
      }
   }
   parse_releasekeyvalues(values, NUM_KEYS);
   return found;
}

/* Read a side's Title/Description for the disc menu WITHOUT jukeboxing to
   it: a jukebox is a remount, and the menu's rescan was paying one per side
   purely to read a name.  The mounted side is served from keyvalues[8],
   which the VFS LUN's start filled from its scsi0.cfg (a BeebVFS directory
   only ever holds scsi0); that cache cannot be stale - a VFS jukebox is
   refused while LUN 8 is started (the *FX147 poke stops every LUN first),
   and stopping releases the values. */
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
      char fileName[256];
      vfs_cfg_dir = (int16_t)dir;
      vfs_cfg_title[0] = '\0';
      vfs_cfg_desc[0]  = '\0';
      filesystemVFSFilePath(dir, "scsi0.cfg", fileName, sizeof(fileName));
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
   filesystemState.fsLunBlockSizeInMemory[lunNumber] = false;   // the files say again
   if (lunNumber >= 8 && !filesystemVFSVolumePresent() && !filesystemVFSDatPresent())
      return false;                  /* drive empty: no cfg churn */
   if (!filesystemCheckExtAttributes(lunNumber))
   {
      char fileName[256];
      FIL fileObject;
      FRESULT fsResult;
      // Check if the LUN descriptor file (.dsc) is present
      fsLunFilePath(lunNumber, "dsc", fileName, sizeof(fileName));

      if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemReadLunDescriptor(): Checking for (.dsc) LUN descriptor "), (uint16_t)lunNumber, 1);
      fsResult = fsr_open(&fileObject, fileName, FA_READ);

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
         fsResult = fsr_read(&fileObject, Buffer, 22, &fsCounter);

         // Check that the file was read OK and is the correct length
         if (fsResult != FR_OK  || fsCounter != 22) {
            // Something went wrong
            if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadLunDescriptor(): ERROR: Could not read .dsc file\r\n"));
            fsr_close(&fileObject);

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

         // The .dsc's block size (256 if the data path cannot move it) also
         // into LBADescriptor (from defscsi.cfg until now), for MODE SENSE
         filesystemUseUsableBlockSize(lunNumber);

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
         fsr_close(&fileObject);
      }
      return true;
   }
   return true;
}

// Function to write a LUN descriptor
static bool filesystemWriteAttributesAdding(uint8_t lunNumber, const bool append[]);

bool filesystemWriteAttributes(uint8_t lunNumber)
{
   return filesystemWriteAttributesAdding(lunNumber, NULL);
}

// The .cfg rewrite, also adding a line for each append[] key the file has no
// line for (NULL: none) - a rewrite otherwise only replaces the lines it finds.
static bool filesystemWriteAttributesAdding(uint8_t lunNumber, const bool append[])
{
   char fileName[256];                 // a LUN or side file's name (LFN)
   // Write-protect: ignore the .cfg overwrite but report success (true, NOT
   // the VFS branch's false) so a mounted ADFS never sees a CHECK_COND.
   if (config_beeb_write_protected()) return true;
   if (lunNumber >7)
   {
      // VFS doesn't support write to .cfg files
      return false;
   }

   // Assemble the .cfg file name
   fsLunFilePath(lunNumber, "cfg", fileName, sizeof(fileName));

   // A block size the data path cannot take is 256 in memory only: the
   // file's LBADescriptor line is left as it is (a rewrite keeps the line of
   // a key with no value), unless FORMAT has set a block size of its own.
   parserkeyvalue *lba = &filesystemState.keyvalues[lunNumber][LBADESCRIPTOR];
   size_t lbaLength = lba->length;
   if (filesystemState.fsLunBlockSizeInMemory[lunNumber] && !((append != NULL) && append[LBADESCRIPTOR]))
      lba->length = 0;

   bool written = parse_rewritefile_adding(fileName, fileName, scsiattributes, filesystemState.keyvalues[lunNumber], append);
   if (written) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteAttributes(): .cfg overwritten Successful\r\n"));
   } else {
      // assume no cfg file so create one
      written = parse_rewritefile_adding(defaultLunDescriptor, fileName, scsiattributes, filesystemState.keyvalues[lunNumber], append);
      if (written && debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteAttributes(): .cfg created Successful\r\n"));
   }
   lba->length = lbaLength;
   if (written)
      return true;

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteAttributes(): ERROR: Could not create new .cfg file!\r\n"));
   return false;
}

// FORMAT's geometry (the controller's rules are in scsi.c).  Start the LUN
// for its current .cfg first; false: leave it alone (write-protected,
// a VFS LUN, or it cannot start - FORMAT then says so).
bool filesystemStartLunForFormat(uint8_t lunNumber)
{
   if (config_beeb_write_protected() || lunNumber > 7)
      return false;
   return filesystemSetLunStatus(lunNumber, true);
}

// Save the geometry FORMAT lays down into the LUN's .cfg, before
// filesystemFormatLun sizes the image from it: sectors per track (ModePage3
// bytes 10-11), and a block size other than 0 (LBADescriptor, and page 3
// bytes 12-13).  An interleave other than 0 also goes into page 3 (bytes
// 14-15), as a drive with its own controller keeps it.  Both reach the
// file even when it has no line for them (a .cfg made from a .dsc has no
// page 3 at all: one is made here), or the next start would bring the LUN
// back at 33 sectors and 256 bytes under an image laid down otherwise.
// Mixed comes here only with a page 3 and no block size, as before.  A
// read-only image is left alone.  Returns false if the .cfg cannot be
// saved.
bool filesystemSaveFormatGeometry(uint8_t lunNumber, uint16_t sectorsPerTrack, uint32_t blockSize,
                                  uint16_t interleave, bool page4)
{
   parserkeyvalue *page3 = &filesystemState.keyvalues[lunNumber][MODEPAGE3];
   bool append[sizeof scsiattributes / sizeof scsiattributes[0]] = { false };

   if (filesystemState.fsLunReadOnly[lunNumber])
      return true;
   if (page3->v.string == NULL) {
      // A drive's page 3 (format parameters): code, length 22, then zeros
      // but for the fields set below
      page3->v.string = calloc(1, DRIVE_PAGE_MAX);
      if (page3->v.string == NULL)
         return false;
      page3->v.string[0] = 0x03;
      page3->v.string[1] = 0x16;
      page3->length = 24;
      modePage3Put(page3->v.string, MODEPAGE3_BLOCK_SIZE,
                   (uint16_t)filesystemState.fsLunGeometry[lunNumber].BlockSize);
   }
   if (page3->length >= 12) {
      modePage3Put(page3->v.string, MODEPAGE3_SECTORS_PER_TRACK, sectorsPerTrack);
      if ((blockSize != 0) && (page3->length >= 14))
         modePage3Put(page3->v.string, MODEPAGE3_BLOCK_SIZE, (uint16_t)blockSize);
      if ((interleave != 0) && (page3->length >= 16))
         modePage3Put(page3->v.string, MODEPAGE3_INTERLEAVE, interleave);
      append[MODEPAGE3] = true;
   }
   if (blockSize != 0) {
      filesystemSetLunBlockSize(lunNumber, blockSize);
      append[LBADESCRIPTOR] = true;
   }
   // A page 4 the host sent (SCSI drive: made current at FORMAT) is saved too, added if the
   // .cfg had none: the FORMAT's restart reads the .cfg, and would size the image from the old one
   if (page4 && (filesystemState.keyvalues[lunNumber][MODEPAGE4].v.string != NULL))
      append[MODEPAGE4] = true;
   filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack = sectorsPerTrack;
   return filesystemWriteAttributesAdding(lunNumber, append);
}

// Function to format a LUN image
bool filesystemFormatLun(uint8_t lunNumber, uint8_t dataPattern)
{
   char fileName[256];                 // a LUN or side file's name (LFN)
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

   // The size below comes from fsLunGeometry, which is only current for a
   // started LUN (loaded at START, updated by MODE SELECT).  For a stopped
   // one it was never read since boot - zero, so f_expand failed after
   // FA_CREATE_ALWAYS had already truncated the image to 0 bytes - or it
   // belongs to the image of the directory before a jukebox.  Starting the
   // LUN loads its geometry exactly as the START after this FORMAT will;
   // if it cannot start there is no size to format to, so refuse before
   // anything is truncated.
   if (!filesystemSetLunStatus(lunNumber, true)) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemFormatLun(): ERROR: Could not read the LUN's geometry\r\n"));
      return false;
   }
   filesystemSetLunStatus(lunNumber, false );

   // A read-only image (just found so by that start): ignore FORMAT and
   // report success, as Beeb_write_protect does above
   if (filesystemState.fsLunReadOnly[lunNumber]) return true;

   if (debugFlag_filesystem) debugStringInt32_P(PSTR("File system: filesystemFormatLun(): Sectors required = "), filesystemGetLunTotalSectors(lunNumber), true);

   // Assemble the .dat file name
   fsLunFilePath(lunNumber, "dat", fileName, sizeof(fileName));

   // Note: We are using the expand FAT method to create the LUN image... the dataPattern byte
   // will be ignored.
   // Fill the sector buffer with the required data pattern
   // for (counter = 0; counter < 256; counter++) sectorBuffer[counter] = dataPattern;

   // Create the .dat file (the old .dat file, if present, will be unlinked (i.e. gone forever))

   fsResult = fsr_open(&fileObject, fileName, FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
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
      fsResult = fsr_expand(&fileObject /*&filesystemState.fileObject[lunNumber]*/, (FSIZE_t)(filesystemGetLunTotalBytes(lunNumber)), 1);

      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemFormatLun(): Format complete\r\n"));

      // Check that the file was written OK
      if (fsResult != FR_OK) {
         // Something went wrong writing to the .dat
         if (debugFlag_filesystem) debugStringInt8Hex_P(PSTR("File system: filesystemFormatLun(): ERROR: Could not write .dat : \r\n"),fsResult,1);
         fsr_close(&fileObject);
         return false;
      }
   } else {
      // Something went wrong opening the .dat
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemFormatLun(): ERROR: Could not open .dat\r\n"));
      return false;
   }

   // Formatting successful
   fsr_close(&fileObject);
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemFormatLun(): Successful\r\n"));
   return true;
}

// Check an extended attributes file is available for that LUN
// and register it
bool filesystemCheckExtAttributes( uint8_t lunNumber)
{
   char extAttributes_fileName[255];
   fsLunFilePath(lunNumber, "cfg", extAttributes_fileName, sizeof(extAttributes_fileName));

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
      parse_readfile(defaultLunDescriptor, 0, scsiattributes, filesystemState.keyvalues[lunNumber]);

      // LUN extended attributes file is not present
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckExtAttributes: LUN extended attributes file not found\r\n"));
      filesystemConfigToLunGeometry(lunNumber);
   }

	return false;
}

// takes lun geometry and updates the keyvalues

/* ModePage0 is the Adaptec's drive parameter list on an ACB or SASI board,
   and carries the geometry.  On a drive with its own controller page 0 is
   an ordinary mode page (a CCS drive's Unit Attention page: some drives
   hold 80 02 00 00, others have none) and the geometry is page 4 alone. */
static bool page0IsDriveList = true;

/* DriveGeometry=CFG (true): a .cfg without its geometry page gets none
   made up; Auto (false, the default) deduces it.  Set by scsi.c. */
static bool geometryFromCfgOnly = false;

void filesystemSetGeometryFromCfgOnly(bool cfgOnly)
{
   geometryFromCfgOnly = cfgOnly;
}

void filesystemSetPage0IsDriveList(bool driveList)
{
   page0IsDriveList = driveList;
}

static bool filesystemPage0IsDriveList(void)
{
   return page0IsDriveList;
}

/* The geometry fields of page 0, the Adaptec drive list (cylinders bytes
   1-2, heads byte 3), and of page 4 (cylinders bytes 2-4, of which 16 bits
   are kept, heads byte 5). */
#define MODEPAGE0_CYLINDERS 1
#define MODEPAGE4_CYLINDERS 3

static unsigned int filesystemPageCylindersAt(uint8_t page)
{
   return (page == 0) ? MODEPAGE0_CYLINDERS : MODEPAGE4_CYLINDERS;
}

// The LUN's cylinders and heads from page 0 or 4 (which must be held)
static void filesystemGeometryFromPage(uint8_t lunNumber, uint8_t page)
{
   const char *data = filesystemState.keyvalues[lunNumber][(page == 0) ? MODEPAGE0 : MODEPAGE4].v.string;
   unsigned int at = filesystemPageCylindersAt(page);
   filesystemState.fsLunGeometry[lunNumber].Cylinders = modePageGetField(data, at, 2);
   filesystemState.fsLunGeometry[lunNumber].Heads = (uint8_t)data[at + 2];
}

// The LUN's cylinders and heads into page 0 or 4 (which must be held)
static void filesystemGeometryToPage(uint8_t lunNumber, uint8_t page)
{
   char *data = filesystemState.keyvalues[lunNumber][(page == 0) ? MODEPAGE0 : MODEPAGE4].v.string;
   unsigned int at = filesystemPageCylindersAt(page);
   if (page == 4)
      data[2] = 0;                                  // cylinder count bits 16-23
   modePagePutField(data, at, 2, filesystemState.fsLunGeometry[lunNumber].Cylinders);
   data[at + 2] = (char)filesystemState.fsLunGeometry[lunNumber].Heads;
}

void filesystemLunToconfigGeometry(uint8_t lunNumber)
{
   int index;
   // Update the extended attributes from the cached geometry for this LUN
   index = MODEPAGE0;
   if (filesystemPage0IsDriveList()) {
   if  (!filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemState.keyvalues[lunNumber][index].v.string = malloc(10);
      if (!filesystemState.keyvalues[lunNumber][index].v.string)
      {
         LOG_INFO(PSTR("File system: filesystemLunToconfigGeometry(): ERROR: Unable to allocate memory for MODEPAGE0\n"));
         return;   /* alloc failed: do not dereference NULL below */
      }
      filesystemState.keyvalues[lunNumber][index].length = 10;
   }

   filesystemState.keyvalues[lunNumber][index].v.string[0] = 1 ;
   filesystemGeometryToPage(lunNumber, 0);
   filesystemState.keyvalues[lunNumber][index].v.string[4] = 0x00; // Reduced write current MSB
   filesystemState.keyvalues[lunNumber][index].v.string[5] = 0x80; // Reduced write current LSB
   filesystemState.keyvalues[lunNumber][index].v.string[6] = 0x00; // Write precompensation MSB
   filesystemState.keyvalues[lunNumber][index].v.string[7] = 0x80; // Write precompensation LSB
   filesystemState.keyvalues[lunNumber][index].v.string[8] = 0x00; // Landing zone
   filesystemState.keyvalues[lunNumber][index].v.string[9] = 0x01; // Step pulse count
   if (filesystemState.keyvalues[lunNumber][index].length < 10)
      filesystemState.keyvalues[lunNumber][index].length = 10;
   }

   index = MODEPAGE4;
   if  (!filesystemState.keyvalues[lunNumber][index].v.string)
   {
      // The key's whole size, zero filled, as the parser allocates every page:
      // a masked MODE SELECT writes up to the mask's length (MaskPage4 up to
      // DRIVE_PAGE_MAX), not just the 6 bytes made here
      filesystemState.keyvalues[lunNumber][index].v.string = calloc(1, (size_t)scsiattributes[index].max);
      if (!filesystemState.keyvalues[lunNumber][index].v.string)
      {
         LOG_INFO(PSTR("File system: filesystemLunToconfigGeometry(): ERROR: Unable to allocate memory for MODEPAGE4\n"));
         return;   /* alloc failed: do not dereference NULL below */
      }
      filesystemState.keyvalues[lunNumber][index].length = 6;
   }
   filesystemState.keyvalues[lunNumber][index].v.string[0] = 04 ;
   filesystemState.keyvalues[lunNumber][index].v.string[1] = 04 ;
   filesystemGeometryToPage(lunNumber, 4);
   if (filesystemState.keyvalues[lunNumber][index].length < 6)
      filesystemState.keyvalues[lunNumber][index].length = 6;
}

/* The geometry from the image's size alone: sectors per track from page 3
   (or the default), heads the largest divisor up to 16, cylinders the rest.
   A guess, not the drive's: only for a .cfg without its geometry page. */
static void filesystemGeometryFromImage(uint8_t lunNumber)
{
      /* f_size() is only the image's size while the LUN holds it open.  A
         stopped LUN's FIL is closed and still reports the last image it
         had - another directory's after a jukebox, zero since boot - so
         ask the card (MODE SELECT on a new disc gets here stopped). */
      uint32_t lunFileSize = 0;
      if (filesystemState.fsLunStatus[lunNumber]) {
         lunFileSize = (uint32_t)f_size(&filesystemState.fileObject[lunNumber]);
      } else {
         char datName[48];
         FILINFO fno;
         fsLunFilePath(lunNumber, "dat", datName, sizeof(datName));
         if (fsr_stat(datName, &fno) == FR_OK)
            lunFileSize = (uint32_t)fno.fsize;
      }

      lunFileSize = lunFileSize / (filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack * filesystemState.fsLunGeometry[lunNumber].BlockSize);
      uint8_t heads = 16;

      while ((lunFileSize % heads != 0) && heads > 1) heads--;
      uint32_t cylinders = lunFileSize / heads;

      filesystemState.fsLunGeometry[lunNumber].Cylinders = cylinders;
      filesystemState.fsLunGeometry[lunNumber].Heads = heads;
}

void filesytemdattoconfigGeometry(uint8_t lunNumber)
{
   filesystemGeometryFromImage(lunNumber);
   if (!geometryFromCfgOnly)            // DriveGeometry=CFG: no page invented
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
         LOG_INFO(PSTR("File system: filesystemConfigToLunGeometry(): ERROR: Unable to allocate memory for MODEPARAMHEADER\n"));
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
         filesystemState.fsLunGeometry[lunNumber].BlockSize =
            modePageGetField(filesystemState.keyvalues[lunNumber][index].v.string, LBADESCRIPTOR_BLOCK_SIZE, 3);
      }
      else
      {
         filesystemState.fsLunGeometry[lunNumber].BlockSize = DEFAULT_BLOCK_SIZE;
         //now recreate the LBADescriptor
         filesystemState.keyvalues[lunNumber][index].v.string = malloc(8);
         if (!filesystemState.keyvalues[lunNumber][index].v.string)
         {
            LOG_INFO(PSTR("File system: filesystemConfigToLunGeometry(): ERROR: Unable to allocate memory for LBADescriptor\n"));
            return;   /* alloc failed: do not dereference NULL below */
         }
         filesystemState.keyvalues[lunNumber][index].v.string[0] = 0x00; // Reserved
         filesystemState.keyvalues[lunNumber][index].v.string[1] = 0x00; // Reserved
         filesystemState.keyvalues[lunNumber][index].v.string[2] = 0x00; // Reserved
         filesystemState.keyvalues[lunNumber][index].v.string[3] = 0x00; // Reserved
         filesystemState.keyvalues[lunNumber][index].v.string[4] = 0x00; // Reserved
         modePagePutField(filesystemState.keyvalues[lunNumber][index].v.string, LBADESCRIPTOR_BLOCK_SIZE, 3,
                          DEFAULT_BLOCK_SIZE);
         filesystemState.keyvalues[lunNumber][index].length = 8;
      }

   // A short or hand-edited cfg value can decode as zero (a divide by zero
   // in the geometry calculations) or as a size the data path cannot move,
   // and the VFS LUNs are LaserVision's 256-byte blocks whatever their .cfg
   // says.  Use 256 then, and say so in the LBA descriptor too.
   filesystemUseUsableBlockSize(lunNumber);

   index = MODEPAGE3;
   if  (filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack =
         modePage3Get(filesystemState.keyvalues[lunNumber][index].v.string, MODEPAGE3_SECTORS_PER_TRACK);
   }
   else
   {
      filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack = DEFAULT_SECTORS_PER_TRACK;
   }
   if (filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack == 0)
      filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack = DEFAULT_SECTORS_PER_TRACK;

   index = MODEPAGE0;
   if  (filesystemPage0IsDriveList() && filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemGeometryFromPage(lunNumber, 0);

   }
   else
   {
      // check if page4 is present
      index = MODEPAGE4;
      if  (filesystemState.keyvalues[lunNumber][index].v.string)
      {
               filesystemGeometryFromPage(lunNumber, 4);

               filesystemCopyPage4toPage0(lunNumber);
      } else
      {
         // The .cfg has no geometry page (page 4 on a drive with its own
         // controller, page 0 or 4 otherwise).  DriveGeometry=Auto: deduce
         // it from the image size and publish it as the page; CFG: invent
         // nothing, a host asking for the page is refused as by a drive
         // without it.  A debug build says so once per LUN until the drives
         // change (a remount or another ADFS directory), not at every start,
         // MODE SENSE or TRANSLATE; a release build prints nothing here, on
         // the SCSI command path.
         filesystemGeometryFromImage(lunNumber);
#ifdef DEBUG
         static uint32_t warned[MAX_LUNS];     // drive generation + 1 when said
         if (warned[lunNumber] != filesystemState.driveGeneration + 1u) {
            const struct HDGeometry *g = &filesystemState.fsLunGeometry[lunNumber];
            warned[lunNumber] = filesystemState.driveGeneration + 1u;
            LOG_DEBUG("BeebSCSI: LUN %u: the .cfg has no page 4%s - %s %lu x %u x %u from the image size\n",
                      (unsigned)lunNumber, filesystemPage0IsDriveList() ? " or page 0" : "",
                      geometryFromCfgOnly ? "DriveGeometry=CFG: no page made; capacity" : "geometry deduced:",
                      (unsigned long)g->Cylinders, (unsigned)g->Heads, (unsigned)g->SectorsPerTrack);
         }
#endif
         if (!geometryFromCfgOnly)
            filesystemLunToconfigGeometry(lunNumber);
      }
   }
}


// The LUN's block size, into LBADescriptor (bytes 5-7) and the geometry
void filesystemSetLunBlockSize(uint8_t lunNumber, uint32_t blockSize)
{
   char *lba = filesystemState.keyvalues[lunNumber][LBADESCRIPTOR].v.string;
   if (lba == NULL) {
      filesystemConfigToLunGeometry(lunNumber);   // makes a default one
      lba = filesystemState.keyvalues[lunNumber][LBADESCRIPTOR].v.string;
      if (lba == NULL)
         return;
   }
   modePagePutField(lba, LBADESCRIPTOR_BLOCK_SIZE, 3, blockSize);
   filesystemState.fsLunGeometry[lunNumber].BlockSize = blockSize;
   filesystemState.fsLunBlockSizeInMemory[lunNumber] = false;   // FORMAT's own: saved with it
}

void filesystemCopyPage0toPage4(uint8_t lunNumber)
{
   if (!filesystemPage0IsDriveList())
      return;
   int index = MODEPAGE0;
   if (filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemGeometryFromPage(lunNumber, 0);
      // now recreate Page4
      index = MODEPAGE4;
      if  (filesystemState.keyvalues[lunNumber][index].v.string)
      {
         filesystemGeometryToPage(lunNumber, 4);
         if (filesystemState.keyvalues[lunNumber][index].length < 6)
            filesystemState.keyvalues[lunNumber][index].length = 6;
      }
   }
}

void filesystemCopyPage4toPage0(uint8_t lunNumber)
{
   if (!filesystemPage0IsDriveList())
      return;
   int index = MODEPAGE4;
   if (filesystemState.keyvalues[lunNumber][index].v.string)
   {
      filesystemGeometryFromPage(lunNumber, 4);
      // now recreate Page0
      index = MODEPAGE0;
      if  (filesystemState.keyvalues[lunNumber][index].v.string)
      {
         filesystemGeometryToPage(lunNumber, 0);
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
   FSIZE_t seekPoint = (FSIZE_t)startSector * filesystemGetLunDataBlockSize(lunNumber);
   if (seekPoint > f_size(&filesystemState.fileObject[lunNumber]))
      seekPoint = f_size(&filesystemState.fileObject[lunNumber]);

   if (fsr_lseek(&filesystemState.fileObject[lunNumber], seekPoint) != FR_OK) {
      // Something went wrong with seeking, do not retry
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForRead(): ERROR: Unable to seek to required sector in LUN image file!\r\n"));
      return false;
   }
   sectorsRemaining = requiredNumberOfSectors;
   sectorsInBuffer = 0;
#ifdef DEBUG
   map_check_pending = (startSector == 0 && lunNumber < 8 &&
                        filesystemGetLunDataBlockSize(lunNumber) == 256);
#endif

   // Exit with success
   filesystemState.fsLunStatus[lunNumber] = true;
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForRead(): Successful\r\n"));
   return true;
}

// Function to read next sector from a LUN
bool filesystemReadNextSector(uint8_t lunNumber, uint8_t **buffer)
{
   // The LUN's block size: as many whole blocks as the buffer holds
   uint32_t blockSize = filesystemGetLunDataBlockSize(lunNumber);

   if (sectorsInBuffer == 0)
   {
      uint32_t sectorsToRead = sectorsRemaining;
      FRESULT fsResult;
      UINT fsCounter;
      if (sectorsToRead > SECTOR_BUFFER_SIZE / blockSize) sectorsToRead = SECTOR_BUFFER_SIZE / blockSize;

      // Read the required data into the sector buffer
      fsResult = fsr_read(&filesystemState.fileObject[lunNumber], sectorBuffer, sectorsToRead * blockSize, &fsCounter);

      // Check that the file was read OK
      if (fsResult != FR_OK) {
         // Something went wrong
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadNextSector(): ERROR: Cannot read from LUN image!\r\n"));
         return false;
      }

      // A short read means the host is reading past the end of an image that
      // has not been grown to full geometry yet - unwritten sectors read as
      // zeros rather than whatever the buffer last held
      if (fsCounter < sectorsToRead * blockSize) {
         memset(sectorBuffer + fsCounter, 0, (sectorsToRead * blockSize) - fsCounter);
      }

      sectorsInBuffer = (uint8_t)sectorsToRead;
      currentBufferSector = 0;
      sectorsRemaining = sectorsRemaining - sectorsInBuffer;
#ifdef DEBUG
      if (map_check_pending && fsCounter >= 512u) {
         map_check_pending = false;
         uint8_t c0 = adfs_map_cksum(sectorBuffer);
         uint8_t c1 = adfs_map_cksum(sectorBuffer + 256);
         LOG_INFO("MAPCHECK: s0 %02x/%02x s1 %02x/%02x %s\n",
                  sectorBuffer[255], c0, sectorBuffer[511], c1,
                  (sectorBuffer[255] == c0 && sectorBuffer[511] == c1) ? "OK" : "BAD");
      }
#endif
   }
   // return pointer to buffer with sector
   *buffer = sectorBuffer + (currentBufferSector * blockSize);
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

   // A read-only image (AM_RDO, opened FA_READ): nothing to seek or grow -
   // filesystemWriteNextSector() swallows the data, as under write-protect
   if (filesystemState.fsLunReadOnly[lunNumber])
      return true;

   FSIZE_t seekPoint = (FSIZE_t)startSector * filesystemGetLunDataBlockSize(lunNumber);

#if FF_USE_FASTSEEK
   FIL *fp = &filesystemState.fileObject[lunNumber];

   if (fp->cltbl != 0 && seekPoint > f_size(fp)) {
      // Fast seek clips seeks at the current file size and cannot allocate
      // clusters, so a write starting beyond the end of a growing image
      // would land at the wrong offset. Extend the file with fast seek
      // disabled, then rebuild the link map to cover the new clusters.
      FRESULT fsResult;

      fp->cltbl = 0;
      fsResult = fsr_lseek(fp, seekPoint);

      if (!filesystemAttachLinkMap(fp, &filesystemState.clmt[lunNumber],
                                   &filesystemState.clmtEntries[lunNumber])) {
         // Too fragmented for the map - fall back to slow seek (as at open)
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForWrite(): LUN very fragmented falling back to slow seek\r\n"));
      }

      // A seek that stopped short of the target means the SD card filled up
      // while the image was being extended
      if (fsResult != FR_OK || f_tell(fp) != seekPoint) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForWrite(): ERROR: Unable to grow LUN image file!\r\n"));
         return false;
      }
   }
#endif

   // Move to the correct point in the DAT file
   // Check that the file seek was OK
   if (fsr_lseek(&filesystemState.fileObject[lunNumber], seekPoint) != FR_OK) {
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
   // A read-only image (fsLunReadOnly) is treated the same way.
   if (config_beeb_write_protected() || filesystemState.fsLunReadOnly[lunNumber]) return true;

   // The LUN's block size: as many whole blocks as the buffer holds
   uint32_t blockSize = filesystemGetLunDataBlockSize(lunNumber);
   uint32_t bufferSectors = SECTOR_BUFFER_SIZE / blockSize;

   memcpy(sectorBuffer + (currentBufferSector * blockSize), buffer , blockSize );
   currentBufferSector++;

   if ( (currentBufferSector == bufferSectors) || ( currentBufferSector == sectorsRemaining)) {
      FRESULT fsResult;
      UINT fsCounter;
      uint32_t sectorsToWrite;

      if (currentBufferSector == bufferSectors)
         sectorsToWrite = bufferSectors;
      else
         sectorsToWrite = sectorsRemaining;

      // Write the required data
      fsResult = fsr_write(&filesystemState.fileObject[lunNumber], sectorBuffer, sectorsToWrite * blockSize, &fsCounter);

#if FF_USE_FASTSEEK
      if (fsResult == FR_OK && fsCounter != sectorsToWrite * blockSize &&
          filesystemState.fileObject[lunNumber].cltbl != 0) {
         // In fast seek mode f_write cannot allocate new clusters, so a write
         // that grows the image stops at the end of the mapped cluster chain
         // and reports a short transfer. Grow the file with fast seek
         // disabled, then rebuild the link map to cover the new clusters.
         FIL *fp = &filesystemState.fileObject[lunNumber];
         UINT fsExtended = 0;

         fp->cltbl = 0;
         fsResult = fsr_write(fp, sectorBuffer + fsCounter, (sectorsToWrite * blockSize) - fsCounter, &fsExtended);
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
      if (fsResult != FR_OK || fsCounter != sectorsToWrite * blockSize) {
         // Something went wrong
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteNextSector(): ERROR: Cannot write to LUN image!\r\n"));
         return false;
      }

      currentBufferSector = 0;
      sectorsRemaining -= sectorsToWrite;

      if (sectorsRemaining == 0)
         fsr_sync(&filesystemState.fileObject[lunNumber]);
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

/* The index of the key PREFIX + page number (e.g. "MaskPage3"), or -1. */
static int filesystemPageKeyIndex(const char * prefix, uint8_t page)
{
   char key[16];
   snprintf(key, sizeof key, "%s%u", prefix, (unsigned)(page & 0x3f));
   return parse_findindex(key, scsiattributes);
}

/* A page's changeable mask: MaskPageN, or the old name WritPageN when the
   .cfg has no MaskPage line for it; -1 if there is neither key. */
static int filesystemMaskIndex(uint8_t lunNumber, uint8_t page)
{
   int index = filesystemPageKeyIndex("MaskPage", page);
   if (index != -1 && filesystemState.keyvalues[lunNumber][index].v.string != NULL)
      return index;
   int old = filesystemPageKeyIndex("WritPage", page);
   return (old != -1) ? old : index;
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

/* A page's changeable mask (MaskPageN, the page as a drive returns it for
   "changeable values": page code, length, then 1 = changeable), or NULL. */
char * filesystemGetMaskPageData(uint8_t lunNumber, uint8_t page, size_t * length)
{
   int index = filesystemMaskIndex(lunNumber, page);
   if (index == -1)
      return 0;
   *length = filesystemState.keyvalues[lunNumber][index].length;
   return filesystemState.keyvalues[lunNumber][index].v.string;
}

/* A page's factory defaults (FDefPageN), or NULL. */
char * filesystemGetFDefPageData(uint8_t lunNumber, uint8_t page, size_t * length)
{
   int index = filesystemPageKeyIndex("FDefPage", page);
   if (index == -1)
      return 0;
   *length = filesystemState.keyvalues[lunNumber][index].length;
   return filesystemState.keyvalues[lunNumber][index].v.string;
}

/* PageControl=CCS: the drive has no page control field (MODE SENSE pages
   3D/3E/3F give factory, changeable and current values). */
bool filesystemPageControlCCS(uint8_t lunNumber)
{
   const char * s = filesystemState.keyvalues[lunNumber][PAGECONTROL].v.string;
   return (s != NULL) && (strncasecmp(s, "CCS", 3) == 0);
}

/* A page's mask applied: out[i] takes the sent byte's changeable bits and
   the old byte's others, for the first `count` bytes (the caller's bound:
   what was sent and what the mask covers).  An old byte past oldLength is
   0.  out may be old (a page updated in place).  Returns count. */
static size_t filesystemApplyMask(char * out, const uint8_t * sent, const char * old, size_t oldLength,
                                  const char * mask, size_t count)
{
   for (size_t i = 0; i < count; i++) {
      uint8_t was = (i < oldLength) ? (uint8_t)old[i] : 0u;
      out[i] = (char)((sent[i] & (uint8_t)mask[i]) | (was & (uint8_t)~mask[i]));
   }
   return count;
}

int filesystemWriteModePageData(uint8_t lunNumber, uint8_t page, uint8_t len, const uint8_t * Buffer)
{
   int index = filesystemPagetoIndex(page);
   if (index == -1)
      return 0;

   int windex = filesystemMaskIndex(lunNumber, page);
   if (windex != -1 && filesystemState.keyvalues[lunNumber][index].v.string != NULL
                    && filesystemState.keyvalues[lunNumber][windex].v.string != NULL)
      {
         // onto the stored page, in place: the key is allocated to its maximum
         parserkeyvalue *kv = &filesystemState.keyvalues[lunNumber][index];
         const parserkeyvalue *mask = &filesystemState.keyvalues[lunNumber][windex];
         size_t count = (len < mask->length) ? len : mask->length;
         size_t i = filesystemApplyMask(kv->v.string, Buffer, kv->v.string, SIZE_MAX, mask->v.string, count);
         if (kv->length < i)
            kv->length = i;
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
      LOG_INFO(PSTR("File system: filesystemWriteModePageData(): ERROR: Unable to allocate memory for mode page data\n"));
      return 0; /* allocation failed */
   }
   memset(filesystemState.keyvalues[lunNumber][index].v.string, 0, alloc);
   memcpy(filesystemState.keyvalues[lunNumber][index].v.string, Buffer, len);
   return 1;
}

/* A numeric field of a mode page: where it starts (counting the page code and
   length bytes) and how many bytes it takes. */

/* Page 3 (format parameters) and page 4 (rigid drive geometry).  The Acorn
   FileStore formatter zeroes most of page 3 before FORMAT, as a drive's
   specification asks for read-only fields.  With a mask the drive simply
   ignores those bytes; without one a zero in these fields
   is taken as "the drive's own value". */
static const modePageField_t modePage3Fields[] = {
   {2, 2}, {4, 2}, {6, 2}, {8, 2}, {10, 2}, {12, 2}, {14, 2}, {16, 2}, {18, 2}
};
static const modePageField_t modePage4Fields[] = { {2, 3}, {5, 1} };

const modePageField_t * filesystemModePageFields(uint8_t page, size_t * count)
{
   if (page == 3) {
      *count = sizeof modePage3Fields / sizeof modePage3Fields[0];
      return modePage3Fields;
   }
   if (page == 4) {
      *count = sizeof modePage4Fields / sizeof modePage4Fields[0];
      return modePage4Fields;
   }
   *count = 0;
   return NULL;
}

/* Store a mode page a host has sent, which may have been cut short.

   Buffer is the page as received: the page code, the length byte, then the
   data.  `declared` is the size the length byte promises (counting those two
   bytes) and `received` how much of it actually arrived.  A drive takes the
   bytes it is given and leaves the rest of the page as it was (as a drive's
   own firmware does: a page may be sent shorter than stored, never longer);
   the Acorn formatter relies on it, sending len+4 bytes where a whole page
   needs len+6 (FServFmt: page 3 always, pages 1 and 4 on some drives).
   zeroKeeps: a zero in a numeric field of page 3 or 4 keeps the stored
   value, so formatting cannot wipe the sectors per track, cylinders or
   heads.  keepCode: a page already held keeps its own code byte. */
static size_t filesystemMergeOnto(uint8_t page, const char * old, size_t oldLength, uint8_t declared,
                                  uint8_t received, const uint8_t * Buffer, uint8_t merged[256],
                                  bool zeroKeeps, bool keepCode)
{
   size_t total = declared;
   size_t fieldCount = 0;
   const modePageField_t * fields = zeroKeeps ? filesystemModePageFields(page, &fieldCount) : NULL;

   memset(merged, 0, (size_t)256u);
   if (old != NULL) {
      if (oldLength > (size_t)256u)
         oldLength = (size_t)256u;
      memcpy(merged, old, oldLength);
      if (oldLength > total)
         total = oldLength;
   }
   if (total > (size_t)256u)
      total = (size_t)256u;
   if (received > total)
      received = (uint8_t)total;

   /* The page code, unless the drive already has the page: then its own
      code byte stays, PS bit included (a host's MODE SELECT sends the page
      code without PS; the drive still reports it).  The length byte covers
      the whole stored page. */
   if (received > 0 && (!keepCode || oldLength < 1))
      merged[0] = Buffer[0];
   if (total >= 2)
      merged[1] = (uint8_t)(total - 2);

   for (uint8_t i = 2; i < received; i++) {
      bool inField = false;
      for (size_t f = 0; f < fieldCount; f++)
         if (i >= fields[f].start && i < fields[f].start + fields[f].size)
            inField = true;
      if (!inField)
         merged[i] = Buffer[i];
   }

   for (size_t f = 0; f < fieldCount; f++) {
      if (fields[f].start >= received)
         continue;
      unsigned int have = (unsigned int)received - fields[f].start;
      if (have > fields[f].size)
         have = fields[f].size;
      bool allZero = true;
      for (unsigned int b = 0; b < have; b++)
         if (Buffer[fields[f].start + b] != 0)
            allZero = false;
      if (!allZero)
         memcpy(&merged[fields[f].start], &Buffer[fields[f].start], have);
   }

   return total;
}

/* driveRules (a drive with its own controller): the stored code byte stays
   (PS bit included), and a page with a mask is decided by the mask alone.
   Without them a zero field of page 3 or 4 always keeps the stored value
   and the code byte is the host's, as Pi1MHz has always done. */
int filesystemMergeModePageData(uint8_t lunNumber, uint8_t page, uint8_t declared, uint8_t received,
                                const uint8_t * Buffer, bool driveRules)
{
   uint8_t merged[256];
   size_t oldLength = 0;
   const char * old = filesystemGetModePageData(lunNumber, page, &oldLength);
   size_t maskLength = 0;
   bool zeroKeeps = !driveRules || (filesystemGetMaskPageData(lunNumber, page, &maskLength) == NULL);
   size_t total = filesystemMergeOnto(page, old, oldLength, declared, received, Buffer, merged, zeroKeeps,
                                      driveRules);

   return filesystemWriteModePageData(lunNumber, page, (uint8_t)total, merged);
}

/* What a page becomes when a host sends Buffer onto `base` (the pending
   page): the merge above, then the page's mask as filesystemWriteModePageData
   applies it.  Returns its length; out holds 256 bytes. */
size_t filesystemModePageAfterSelect(uint8_t lunNumber, uint8_t page, const char * base, size_t baseLength,
                                     uint8_t declared, uint8_t received, const uint8_t * Buffer, uint8_t out[256])
{
   uint8_t merged[256];
   size_t maskLength = 0;
   const char * mask = filesystemGetMaskPageData(lunNumber, page, &maskLength);
   size_t total = filesystemMergeOnto(page, base, baseLength, declared, received, Buffer, merged, mask == NULL, true);

   memcpy(out, merged, 256);
   // onto the pending base, which may be shorter than the page
   if ((mask != NULL) && (base != NULL))
      filesystemApplyMask((char *)out, merged, base, baseLength, mask, (total < maskLength) ? total : maskLength);
   return total;
}

/* Store a whole page as given, no mask (FORMAT making the pending page
   current, or a reset to the factory values). */
void filesystemSetModePageRaw(uint8_t lunNumber, uint8_t page, const uint8_t * data, size_t length)
{
   int index = filesystemPagetoIndex(page);
   if ((index == -1) || (length == 0))
      return;
   parserkeyvalue *kv = &filesystemState.keyvalues[lunNumber][index];
   size_t alloc = ((size_t)scsiattributes[index].max > length) ? (size_t)scsiattributes[index].max : length;
   if ((kv->v.string == NULL) || (kv->length < length)) {
      char *grown = malloc(alloc);
      if (grown == NULL)
         return;
      memset(grown, 0, alloc);
      if (kv->v.string != NULL) {
         memcpy(grown, kv->v.string, kv->length);
         free(kv->v.string);
      }
      kv->v.string = grown;
   }
   memcpy(kv->v.string, data, length);
   kv->length = length;                 // a shorter page leaves no old tail behind
}

/* TrackFormat=T,O,S of a LUN; false if it has none (or a broken one) */
bool filesystemGetLunTrackFormat(uint8_t lunNumber, uint32_t *track, uint32_t *overhead, uint32_t *spare)
{
   const char *s = filesystemState.keyvalues[lunNumber][TRACKFORMAT].v.string;
   char *end;
   if (s == NULL)
      return false;
   *track = (uint32_t)strtoul(s, &end, 10);
   if (*end != ',')
      return false;
   *overhead = (uint32_t)strtoul(end + 1, &end, 10);
   if (*end != ',')
      return false;
   *spare = (uint32_t)strtoul(end + 1, &end, 10);
   return *track != 0;
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

   // Open the FAT transfer directory, on the Beeb's storage
   char fatDir[sizeof fatDirectory + 2];
   if (!filesystemStoragePath(fatDirectory, fatDir, sizeof fatDir))
      return false;
   fsResult = fsr_opendir(&dirObject, fatDir);

   // Did a directory exist?
   if (fsResult == FR_NO_PATH) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckFatDirectory(): f_opendir returned FR_NO_PATH - Directory does not exist\r\n"));
      // Create the FAT transfer directory - it's not present on the SD card
      // Check the result
      if (fsr_mkdir(fatDir) != FR_OK) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): ERROR: Unable to create FAT transfer directory\r\n"));
         return false;
      }
      fsr_opendir(&dirObject, fatDir);

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
      fsResult = fsr_readdir(&dirObject, &fsInfo);

      // Exit on error or end of directory object entries
      if (fsResult != FR_OK || fsInfo.fname[0] == 0) {
         // The requested directory entry does not exist
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemGetFatFileInfo(): Requested directory entry does not exist\r\n"));
         buffer[0] = 0; // file does not exist
         fsr_closedir(&dirObject);
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
   fsr_closedir(&dirObject);

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

   // Open the FAT transfer directory, on the Beeb's storage
   char fatDir[sizeof fatDirectory + 2];
   if (!filesystemStoragePath(fatDirectory, fatDir, sizeof fatDir))
      return false;
   fsResult = fsr_opendir(&dirObject, fatDir);

   // Check the open directory action's result
   if (fsResult == FR_OK) {
      uint32_t fileEntryNumber;

      for (fileEntryNumber = 0; fileEntryNumber <= fileNumber; fileEntryNumber++) {
         fsResult = fsr_readdir(&dirObject, &fsInfo);

         // Exit on error or end of directory object entries
         if (fsResult != FR_OK || fsInfo.fname[0] == 0) {
            // The requested directory entry does not exist
            if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenFatForRead(): Requested directory entry does not exist\r\n"));
            fsr_closedir(&dirObject);
            return false;
         }
      }

      // Is the entry a file or sub-directory?
      if (fsInfo.fattrib & AM_DIR) {
         // Directory
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenFatForRead(): Requested directory entry was a directory - can not read!\r\n"));
         fsr_closedir(&dirObject);
         return false;
      } else {
         char tempfileName[514];
         // Assemble the full path name and file name for the requested file
         snprintf(tempfileName, sizeof(tempfileName), "%s/%s", fatDir, fsInfo.fname);
         fsr_closedir(&dirObject);

         // Open the requested file for reading
         fsResult = fsr_open(&fileObjectFAT, tempfileName, FA_READ);
         if (fsResult != FR_OK) {
            if (debugFlag_filesystem) {
               debugString_P(PSTR("File system: filesystemOpenFatForRead(): ERROR: f_open on FAT file returned "));
               filesystemPrintfserror(fsResult);
            }
            return false;
         }

         // Seek to the correct point in the file
         fsResult  = fsr_lseek(&fileObjectFAT, blockNumber * 256);
         if (fsResult != FR_OK) {
            if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenFatForRead(): Could not seek to required block number!\r\n"));
            fsr_close(&fileObjectFAT);
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
   fsResult  = fsr_read(&fileObjectFAT, buffer, 256, &byteCounter);
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
   fsr_close(&fileObjectFAT);
   return true;
}

// read a file buffer can be malloced if the address is NULL

uint32_t filesystemReadFile(const char * filename, uint8_t **address, unsigned int max_size)
{
   UINT byteCounter;
   FRESULT fsResult;
   FIL fileObject;
   LOG_DEBUG("filesystemReadFile: %s\n", filename);
   /* Through filesystemMount(), the one mount path: it tells the remount
      hooks first, and once mounted the next read (every BBC reset's
      config_load) leaves the volume alone. */
   if (!filesystemMount())
      return 0;
   fsResult = fsr_open(&fileObject, filename, FA_READ);
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
            LOG_INFO(PSTR("File system: filesystemReadFile(): ERROR: Unable to allocate memory for file read\n"));
            fsr_close(&fileObject);
            return 0;
         }
         (*address)[fileSize] = 0;
         read_len = (size_t)fileSize;
      } else {
         /* caller provided a buffer length in max_size; clamp to file size */
         read_len = (size_t)max_size;
         if ((FSIZE_t)read_len > fileSize) read_len = (size_t)fileSize;
      }

      fsResult = fsr_read(&fileObject, *address, read_len, &byteCounter);
      fsr_close(&fileObject);
      if (fsResult != FR_OK) {
         LOG_DEBUG("filesystemReadFile: f_read failed %d\n", fsResult);
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

   fsResult = fsr_open(&fileObject, filename, FA_CREATE_ALWAYS |FA_WRITE);
   if (fsResult != FR_OK)
      return 0;

   fsr_write(&fileObject, address, max_size, &byteCounter);
   fsr_close(&fileObject);
   return byteCounter;
}