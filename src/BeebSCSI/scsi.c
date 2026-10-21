/************************************************************************
   scsi.c

   BeebSCSI SCSI emulation functions
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

/* internal LUNs have been increased to 16
   0-7 are used for ADFS
   8-15 are used for LV-DOS
*/
/************************************************************************

   added by Mark Usher 2024

   SCSI Inquiry command
   SCSI SendDiagnostic command
   MODE PAGE other than 0
   MODE SELECT write
************************************************************************/

// Global includes
#include "cpuspecific.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

// Local includes

#include "debug.h"
#include "hostadapter.h"
#include "filesystem.h"
#include "statusled.h"
#include "scsi.h"

// Define the major and minor firmware version number returned
// by the BSSENSE command
#define FIRMWARE_MAJOR     0x03
#define FIRMWARE_MINOR     0x01
#define FIRMWARE_STRING    "V003.001"

// Debug code displays the bytes transfer
// The macro DECLARES bytesTransferred, so a caller writes
//    DEBUG_bytesTransferred(hostadapterPerformReadDMA(buf));
// and then has the byte count available.  In a release build the debug
// PRINTS of it vanish, so it would be unused - hence the (void) cast in the
// non-debug arm, which keeps the declaration (callers that test the count,
// like the short-DMA checks below, need it in both builds) while still
// preventing the warning.

#ifdef DEBUG
#include <stdio.h>
#include "uart.h"
#define DEBUG_bytesTransferred(x)   uint32_t bytesTransferred =(x); scsiDiagXfer += bytesTransferred
#else
#define DEBUG_bytesTransferred(x)   uint32_t bytesTransferred = (x); (void)bytesTransferred
#endif

// Global for the emulation mode (fixed or removable drive)
// Note: The fixed mode emulates SCSI-1 compliant hard drives for the Beeb
// The removable mode emulates the Laser Video Disc Player (LV-DOS) for Domesday
// static uint8_t emulationMode = FIXED_EMULATION;


// Request sense error codes: the ACB-4000's 4-byte sense (OEM manual
// 500504-00 section 6.3.1.3), byte 0 = address valid (bit 7) + class + code,
// bytes 1-3 = LBA.  A SCSI-type drive reports them as extended sense
// (scsiExtendedSense maps each to a sense key and additional sense code).
#define NO_ERROR        (0x00u<<24)
#define UNIT_NOT_READY  (0x02u<<24)   // what the VFS LUNs and Mixed have always reported
#define DRIVE_NOT_READY (0x04u<<24)   // class 0 code 4: drive not ready
#define BAD_FORMAT      (0x1Cu<<24)
#define WRITE_FAULT     (0x03u<<24)
#define ILLEGAL_ADDR    (0xA1u<<24)
#define BAD_ARG         (0x24u<<24)
#define INTERLEAVE_ERROR (0x1Au<<24)
#define INVALID_COMMAND (0x20u<<24)   // ACB-4000 class 2 code 0: invalid command
#define ILLEGAL_FUNCTION (0x22u<<24)  // SCSI type: MODE SELECT list refused (ASC 22h)
#define BAD_PARAM_FIELD (0x26u<<24)   // SCSI type: invalid field in the parameter list

// REQUEST SENSE command error reporting structure
static uint32_t requestSenseData[MAX_LUNS];

// The controller behind the host adaptor (DriveController=, read at power-on)
static scsiController_t scsiController = SCSI_CONTROLLER_MIXED;

void scsiSetController(scsiController_t controller)
{
   scsiController = controller;
   // page 0 is the Adaptec drive list everywhere but on a SCSI drive
   filesystemSetPage0IsDriveList(controller != SCSI_CONTROLLER_SCSI);
}

scsiController_t scsiGetController(void)
{
   return scsiController;
}

// DriveGeometry= (read at power-on): false = Auto, true = CFG
static bool scsiGeometryCfgOnly = false;

void scsiSetGeometryFromCfgOnly(bool cfgOnly)
{
   scsiGeometryCfgOnly = cfgOnly;
   filesystemSetGeometryFromCfgOnly(cfgOnly);
}

bool scsiGeometryFromCfgOnly(void)
{
   return scsiGeometryCfgOnly;
}

// DriveGeometry=Auto or CFG, any case.  No value is Auto; an unknown one is
// Auto too, and the caller is told (false) so it can say so.
bool scsiSetGeometryName(const char *name)
{
   scsiSetGeometryFromCfgOnly(false);
   if ((name == NULL) || (name[0] == '\0') || (strcasecmp(name, "Auto") == 0))
      return true;
   if (strcasecmp(name, "CFG") == 0) {
      scsiSetGeometryFromCfgOnly(true);
      return true;
   }
   return false;
}

// DriveController=<name>: Mixed, ACB-4000, ACB-4070, SASI or SCSI, any
// case.  No value is the default, Mixed; an unknown one is Mixed too, and
// the caller is told (false) so it can say so.
bool scsiSetControllerName(const char *name)
{
   static const struct { const char *name; scsiController_t type; } names[] = {
      { "Mixed",    SCSI_CONTROLLER_MIXED },
      { "ACB-4000", SCSI_CONTROLLER_ACB4000 },
      { "ACB-4070", SCSI_CONTROLLER_ACB4070 },
      { "SASI",     SCSI_CONTROLLER_SASI },
      { "SCSI",     SCSI_CONTROLLER_SCSI },
   };
   scsiSetController(SCSI_CONTROLLER_MIXED);
   if ((name == NULL) || (name[0] == '\0'))
      return true;
   for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
      if (strcasecmp(name, names[i].name) == 0) {
         scsiSetController(names[i].type);
         return true;
      }
   }
   return false;
}

// The Adaptec boards: the ACB-4000 rules (SASI too, until it has its own)
static bool scsiControllerIsAdaptec(void)
{
   return (scsiController == SCSI_CONTROLLER_ACB4000) || (scsiController == SCSI_CONTROLLER_ACB4070) ||
          (scsiController == SCSI_CONTROLLER_SASI);
}

// A drive with its own controller
static bool scsiControllerIsDrive(void)
{
   return scsiController == SCSI_CONTROLLER_SCSI;
}

// Global structure for storing SCSI CDBs
static struct commandDataBlockStruct
{
   uint8_t data[10];
   uint8_t targetLUN;
   uint8_t status;
} commandDataBlock;

// "Not ready" for the LUN addressed.  The Adaptec's code is 04: its 02 is
// "no seek complete" (OEM manual, class 0 codes).  The VFS LUNs, and every
// LUN under Mixed, keep the 02 their hosts have always been given.
static uint32_t scsiUnitNotReady(void)
{
   return ((commandDataBlock.targetLUN >= 8) || (scsiController == SCSI_CONTROLLER_MIXED))
          ? UNIT_NOT_READY : DRIVE_NOT_READY;
}

// Does the controller have this command (CDB byte 0, groups 0, 1 and 7)?
// Only the commands Pi1MHz implements are listed; any other is refused at
// command phase anyway.  The Adaptec boards have no INQUIRY, REASSIGN
// BLOCKS, READ DEFECT DATA or CERTIFY (OEM manual Table 6-5; ACB-4000A
// Users Manual Table 5-1), so they answer those "invalid command" (20).  A
// SCSI drive has no TRANSLATE (the Adaptec's own, not in a SCSI drive's
// INQUIRY command list); it keeps CERTIFY, which some drives run although
// their list leaves it out.  STOP/START UNIT is accepted under every type on
// purpose, as a Pi1MHz extension: a host stops the drives with it before
// the SD card is changed.  Some real drives refuse it with invalid command
// &20 (measured on a real drive).  Mixed has them all.
static bool scsiControllerHasCommand(uint8_t opcode)
{
   if (scsiController == SCSI_CONTROLLER_MIXED)
      return true;
   switch (opcode) {
      case 0x00: case 0x01: case 0x03: case 0x04: case 0x08: case 0x0A:
      case 0x0B: case 0x15: case 0x1A: case 0x1B: case 0x1D: case 0x25:
      case 0x2F:
         return true;
      case 0x0F:
         return scsiControllerIsAdaptec();
      case 0x07: case 0x12: case 0x37: case 0xE2:
         return scsiControllerIsDrive();
      default:
         return true;     // not one of ours: the decoder refuses it
   }
}

// A drive with its own controller (DriveController=SCSI) holds the pages 3
// and 4 a MODE SELECT sends, and a block descriptor's block size, pending
// until FORMAT makes them current (as a real drive does: MODE SENSE and
// READ CAPACITY show the new values only after FORMAT).  An Adaptec or SASI
// board holds the block size of its MODE SELECT list the same way, so an
// abandoned format leaves the image read in the block size it was laid
// down in.  A real drive keeps the pending image on the disc; here it is
// RAM, dropped when LUNs 0-7 stop being the same drives (a remount, or a
// jukebox of the ADFS directory: filesystemGetDriveGeneration).
static struct {
   uint32_t generation;                 // filesystemGetDriveGeneration() it belongs to
   bool     valid[2];                   // [0] page 3, [1] page 4
   uint8_t  length[2];
   uint8_t  data[2][32];
   uint32_t blockSize;                  // 0 = none
} scsiPending[8];

void scsiClearPendingPages(uint8_t lun)
{
   if (lun < 8) {
      memset(&scsiPending[lun], 0, sizeof scsiPending[lun]);
      scsiPending[lun].generation = filesystemGetDriveGeneration();
   }
}

// Empty a LUN's pending state if it belongs to other drives
static void scsiPendingCheck(uint8_t lun)
{
   if (scsiPending[lun].generation != filesystemGetDriveGeneration())
      scsiClearPendingPages(lun);
}

/* Page 3 or 4 as FORMAT will make it: the pending page, else the current */
const char * scsiGetPendingModePageData(uint8_t lun, uint8_t page, size_t * length)
{
   int i = (page == 4);
   if ((lun < 8) && ((page == 3) || (page == 4))) {
      scsiPendingCheck(lun);
      if (scsiPending[lun].valid[i]) {
         *length = scsiPending[lun].length[i];
         return (const char *)scsiPending[lun].data[i];
      }
   }
   return filesystemGetModePageData(lun, page, length);
}

static void scsiSetPendingModePageData(uint8_t lun, uint8_t page, const uint8_t * data, size_t length)
{
   int i = (page == 4);
   if ((lun >= 8) || ((page != 3) && (page != 4)))
      return;
   scsiPendingCheck(lun);
   if (length > sizeof scsiPending[lun].data[i])
      length = sizeof scsiPending[lun].data[i];
   memcpy(scsiPending[lun].data[i], data, length);
   scsiPending[lun].length[i] = (uint8_t)length;
   scsiPending[lun].valid[i] = true;
}

static void scsiSetPendingBlockSize(uint8_t lun, uint32_t blockSize)
{
   if (lun >= 8)
      return;
   scsiPendingCheck(lun);
   scsiPending[lun].blockSize = blockSize;
}

/* FORMAT on a SCSI drive: the pending pages 3 and 4 (and block descriptor
   block size) become current, in memory; scsiFormatGeometry saves them. */
static void scsiApplyPendingPages(uint8_t lun)
{
   scsiPendingCheck(lun);
   for (int i = 0; i < 2; i++)
      if (scsiPending[lun].valid[i])
         filesystemSetModePageRaw(lun, (uint8_t)(3 + i), scsiPending[lun].data[i], scsiPending[lun].length[i]);
   uint32_t blockSize = scsiPending[lun].blockSize;
   if (blockSize == 0)
      return;
   size_t length = 0;
   const char *page3 = filesystemGetModePageData(lun, 3, &length);
   if ((page3 != NULL) && (length >= 14) && (length <= 256)) {
      uint8_t page[256];
      memcpy(page, page3, length);
      page[12] = (uint8_t)(blockSize >> 8);
      page[13] = (uint8_t)blockSize;
      filesystemSetModePageRaw(lun, 3, page, length);
   }
   filesystemSetLunBlockSize(lun, blockSize);
}

/* A SCSI drive checks a new page 3 or 4 before it takes it.  The limits come
   from the page's mask, not from the drive's model: a numeric field the mask
   lets the host change must fit in the mask (a bytes per sector mask of
   07FF refuses 2048 and 4096; a heads mask of 0F 16 heads), and a block
   size must be 128 to 2048 (MIN_BLOCK_SIZE to MAX_BLOCK_SIZE), which the
   data path takes.  A zero in a field the mask protects is ignored.  Sent
   is the page as received, `received` bytes of it; page the result.
   Returns NO_ERROR, or ILLEGAL_FUNCTION, a SCSI drive's answer to such a
   list. */
static uint32_t scsiCheckModePage(uint8_t lun, uint8_t pageCode, const uint8_t * sent, uint8_t received,
                                 const uint8_t * page, size_t length)
{
   size_t fieldCount, maskLength = 0;
   const modePageField_t * fields = filesystemModePageFields(pageCode, &fieldCount);
   const char * mask = filesystemGetMaskPageData(lun, pageCode, &maskLength);

   for (size_t f = 0; (mask != NULL) && (f < fieldCount); f++) {
      bool changeable = false, nonZero = false, fits = true;
      for (unsigned int b = fields[f].start; b < (unsigned int)fields[f].start + fields[f].size; b++) {
         uint8_t m = (b < maskLength) ? (uint8_t)mask[b] : 0u;
         uint8_t v = (b < received) ? sent[b] : 0u;
         changeable = changeable || (m != 0);
         nonZero = nonZero || (v != 0);
         fits = fits && ((v & (uint8_t)~m) == 0);
      }
      if (changeable && nonZero && !fits)
         return ILLEGAL_FUNCTION;
   }
   if ((pageCode == 3) && (length >= 14)) {
      uint32_t blockSize = ((uint32_t)page[12] << 8) | page[13];
      if ((blockSize < MIN_BLOCK_SIZE) || (blockSize > MAX_BLOCK_SIZE))
         return ILLEGAL_FUNCTION;
   }
   return NO_ERROR;
}

// Sectors per track an Adaptec board lays down at FORMAT, by block size
// (256, 512, 1024) and interleave (1, more than 1): ACB-4000 OEM manual
// Table A-1 (MFM) and ACB-4000A Users Manual Tables 2-1 and 2-2 (the
// ACB-4070's RLL).
static const uint8_t acbSectorsPerTrack[2][3][2] = {
   { { 32, 33 }, { 17, 18 }, {  9,  9 } },     // ACB-4000 (MFM)
   { { 46, 47 }, { 25, 26 }, { 14, 14 } },     // ACB-4070 (RLL)
};

// The drive parameter list an Adaptec board takes (OEM manual 6.3.1.11)
#define ACB_MAX_CYLINDERS 2048
#define ACB_MAX_HEADS     16

static int scsiAcbRow(uint32_t blockSize)
{
   return (blockSize == 256u) ? 0 : (blockSize == 512u) ? 1 : (blockSize == 1024u) ? 2 : -1;
}

// FORMAT's sectors per track and block size, by controller (ACB-4000 FORMAT,
// OEM manual 6.3.1.5).  Called before filesystemFormatLun, which sizes the
// image from the .cfg, so the new values go into the LUN's .cfg first
// (filesystemSaveFormatGeometry).  Interleave 0 means 2.
//   Mixed (the default), as Pi1MHz has always done: a LUN at 32 or 33
//     sectors per track follows the interleave, 32 for 1 and 33 otherwise;
//     any other value is kept.
//   ACB-4000 / ACB-4070: a MODE SELECT's block size is laid down now.  The
//     board's table gives the sectors per track, but only where the board
//     chose them: when the LUN's value is the table's for the block size it
//     has now (or the 32/33 Pi1MHz has always given 256-byte LUNs).  Any
//     other value was set in the .cfg by hand (17 or 63 at 256 bytes, say)
//     and is kept.  An interleave above sectors per track - 1 is error 24
//     on the ACB-4000 (OEM 6-7) and 1A on the ACB-4070 (Users Manual 5.3.3).
//   SASI: a MODE SELECT's block size, and the .cfg's sectors per track
//     (a board that sets them by jumper has its value there); interleave as
//     the ACB-4000.
//   SCSI (as a drive's own firmware does it): the pending pages 3 and 4 become
//     current; the block size is page 3's (bytes 12-13); with a TrackFormat
//     the drive works out its sectors per track: N = T / (block size + O)
//     physical, N - S used, and the interleave must be below N - 1; without
//     one, page 3's sectors per track stay and the interleave must be below
//     them.  The interleave (0 kept as 2) goes into page 3 bytes 14-15.
// A write-protected or read-only image is left alone.  Returns NO_ERROR, or
// the error FORMAT is to report (BAD_FORMAT if the .cfg cannot be saved).
static uint32_t scsiFormatGeometry(uint8_t lun, uint16_t interleave)
{
   if (!filesystemStartLunForFormat(lun))
      return NO_ERROR;

   uint32_t oldBlockSize = filesystemGetLunBlockSize(lun);
   uint32_t blockSize = oldBlockSize;
   uint32_t newBlockSize = 0;                 // 0: as it is
   uint16_t now = (uint16_t)filesystemGetLunSPTSize(lun);
   uint16_t sectorsPerTrack = now;
   uint16_t interleaveUsed = interleave ? interleave : 2u;
   uint16_t pageInterleave = 0;               // SCSI: kept in page 3
   bool     savePage4 = false;                // SCSI: a page 4 the host sent
   uint32_t error = NO_ERROR;

   switch (scsiController) {
      case SCSI_CONTROLLER_MIXED: {
         size_t length = 0;
         const char *page3 = filesystemGetModePageData(lun, 3, &length);
         if ((page3 != NULL) && (length >= 12) && ((now == 32u) || (now == 33u)))
            sectorsPerTrack = (interleave == 1u) ? 32u : 33u;
         break;
      }
      case SCSI_CONTROLLER_ACB4000:
      case SCSI_CONTROLLER_ACB4070:
      case SCSI_CONTROLLER_SASI: {
         scsiPendingCheck(lun);
         if (scsiPending[lun].blockSize != 0)
            blockSize = newBlockSize = scsiPending[lun].blockSize;
         if (scsiController == SCSI_CONTROLLER_SASI) {
            if (interleaveUsed > sectorsPerTrack - 1u)
               error = BAD_ARG;
            break;
         }
         int board = (scsiController == SCSI_CONTROLLER_ACB4070);
         int row = scsiAcbRow(blockSize), oldRow = scsiAcbRow(oldBlockSize);
         if (row < 0) {
            error = BAD_ARG;             // a block size the board cannot have
            break;
         }
         bool boardChose = ((oldBlockSize == 256u) && ((now == 32u) || (now == 33u))) ||
                           ((oldRow >= 0) && ((now == acbSectorsPerTrack[board][oldRow][0]) ||
                                              (now == acbSectorsPerTrack[board][oldRow][1])));
         if (boardChose)
            sectorsPerTrack = acbSectorsPerTrack[board][row][interleaveUsed > 1u];
         if (interleaveUsed > sectorsPerTrack - 1u)
            error = board ? INTERLEAVE_ERROR : BAD_ARG;
         break;
      }
      case SCSI_CONTROLLER_SCSI: {
         uint32_t track, overhead, spare, limit;
         size_t length = 0;
         scsiPendingCheck(lun);
         bool hostPage3 = scsiPending[lun].valid[0];
         savePage4 = scsiPending[lun].valid[1];
         scsiApplyPendingPages(lun);
         filesystemConfigToLunGeometry(lun);
         blockSize = filesystemGetLunBlockSize(lun);
         // The block size is the .cfg's LBADescriptor (or a MODE SELECT's), and page 3's
         // bytes 12-13 only when the host sent a page 3: a .cfg's own page 3 can disagree
         // with a hand-set LBADescriptor, which wins (page 3 is then saved to match)
         const char *page3 = filesystemGetModePageData(lun, 3, &length);
         if (hostPage3 && (page3 != NULL) && (length >= 14)) {
            uint32_t pageBlockSize = ((uint32_t)(uint8_t)page3[12] << 8) | (uint8_t)page3[13];
            if (pageBlockSize != 0)
               blockSize = pageBlockSize;
         }
         newBlockSize = blockSize;
         pageInterleave = interleaveUsed;
         sectorsPerTrack = (uint16_t)filesystemGetLunSPTSize(lun);
         limit = sectorsPerTrack;
         if ((blockSize < MIN_BLOCK_SIZE) || (blockSize > MAX_BLOCK_SIZE)) {
            error = BAD_ARG;
            break;
         }
         if (filesystemGetLunTrackFormat(lun, &track, &overhead, &spare)) {
            uint32_t physical = track / (blockSize + overhead);
            // a count page 3's 16-bit field cannot hold is refused too
            if ((physical <= spare) || (physical < 2u) || (physical - spare > UINT16_MAX)) {
               error = BAD_ARG;
               break;
            }
            sectorsPerTrack = (uint16_t)(physical - spare);
            limit = physical - 1u;
         }
         if (interleaveUsed >= limit)
            error = BAD_ARG;
         break;
      }
   }

   // Mixed saves the .cfg only when the sectors per track change, as before
   if ((error == NO_ERROR) && ((scsiController != SCSI_CONTROLLER_MIXED) || (sectorsPerTrack != now))) {
      if (filesystemSaveFormatGeometry(lun, sectorsPerTrack, newBlockSize, pageInterleave, savePage4))
         scsiClearPendingPages(lun);
      else
         error = BAD_FORMAT;
   }
   filesystemSetLunStatus(lun, false);
   return error;
}

// SCSI STATUS phase results

#define SCSI_STATUS_OK           (0x00)
#define SCSI_STATUS_CHECK_COND   (0x02)


// Global for storing the current SCSI emulation state
static uint8_t scsiState;

/* /status diagnostics: where the state machine sits (SCSI_BUSFREE=0 etc.) */
uint8_t scsiDiagState(void) { return scsiState; }

/* Wedge forensics: the last CDB seen and the data bytes moved by the last
   data-phase transfer - read from /status while a hang is live. */
uint8_t  scsiDiagCdb[6];
uint32_t scsiDiagXfer;
uint32_t scsiDiagCmdCount;

uint8_t scsiHostID;
uint8_t ourscsiid;

static void scsiInformationTransferPhase(uint8_t transferPhase);

static uint8_t scsiEmulationBusFree(void);
static uint8_t scsiEmulationCommand(void);
static uint8_t scsiEmulationStatus(void);
static uint8_t scsiEmulationMessage(void);

/* BUS FREE's sub-state, at file scope so a forced return to BUS FREE (see
   scsiProcessEmulation) can send it through case 0's bus clear instead of
   resuming in case 1 with BSY and REQ still up from the abandoned command. */
static uint8_t scsiEmulationBusFreestate = 0;


static uint8_t scsiCommandTestUnitReady(void);
static uint8_t scsiCommandRezeroUnit(void);
static uint8_t scsiCommandRequestSense(void);
static uint8_t scsiCommandFormat(void);
static uint8_t scsiCommandReassignBlocks(void);
static uint8_t scsiCommandRead6(void);
static uint8_t scsiCommandWrite6(void);
static uint8_t scsiCommandSeek(void);
static uint8_t scsiCommandTranslate(void);
static uint8_t scsiCommandModeSelect6(void);
static uint8_t scsiCommandModeSense6(void);
static uint8_t scsiCommandStartStop(void);
static uint8_t scsiCommandCertify(void);
static uint8_t scsiCommandVerify(void);
static uint8_t scsiCommandReadCapacity(void);
static uint8_t scsiCommandReadDefectData10(void);
static uint8_t scsiCommandInquiry(void);
static uint8_t scsiCommandSendDiagnostic(void);

#include "fcode.h"
#include "../harddisc_emulator.h"
#include "../videoplayer.h"
static uint8_t scsiWriteFCode(void);
static uint8_t scsiReadFCode(void);

static uint8_t scsiBeebScsiSense(void);
static uint8_t scsiBeebScsiSelect(void);

static uint8_t scsiBeebScsiFatPath(void);
static uint8_t scsiBeebScsiFatInfo(void);
static uint8_t scsiBeebScsiFatRead(void);

static uint32_t scsiTransformLUNid(uint8_t LUN);

// Initialise the SCSI emulation (called on a cold-start of the AVR)
void scsiInitialise(void)
{
/* Upstream's cold-start banner; compiled out with a named gate rather than
   deleted so future BeebSCSI diffs stay clean. */
#define BEEBSCSI_STARTUP_BANNER 0
#if BEEBSCSI_STARTUP_BANNER
   // On a cold-start we always output debug information (ignoring the setting of the
   // debug flags) - as this is useful for initial board testing
/*
   debugString_P(PSTR("\r\n\r\nBeebSCSI - Acorn SCSI-1 Emulation\r\n\r\n"));
   debugString_P(PSTR("(c)2018 Simon Inns\r\n"));
   debugString_P(PSTR("https://www.domesday86.com\r\n"));
   debugString_P(PSTR("Open-source GPLv3 firmware\r\n"));
   debugString_P(PSTR("\r\n"));
   debugString_P(PSTR("Firmware: "));
   debugString_P(PSTR(FIRMWARE_STRING));
   debugString_P(PSTR("\r\n"));
  */
   // Determine the emulation mode (fixed or LV-DOS)
   if (hostadapterConnectedToExternalBus()) {
      emulationMode = FIXED_EMULATION;
      debugString_P(PSTR("Emulation mode is Winchester (ADFS SCSI-1 hard-drive)\r\n"));
   } else {
      emulationMode = LVDOS_EMULATION;
      debugString_P(PSTR("Emulation mode is Philips VP415 (VFS LaserDisc player)\r\n"));
   }

   // Show the USB status
   if (usbHardwareDetect()) {
      // USB hardware present
      debugString_P(PSTR("USB capable board detected\r\n"));
      } else {
      // No USB hardware
      debugString_P(PSTR("USB capable board not detected\r\n"));
   }

   debugString_P(PSTR("\r\n"));
#endif
   if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: Initialising SCSI emulation\r\n"));
}

// Reset the SCSI emulation (called when the host signals reset)
void scsiReset(uint8_t scsiid)

{
   uint8_t lunNumber;

   if (debugFlag_scsiState) {
      debugString_P(PSTR("\r\n\r\nSCSI State: Resetting SCSI emulation\r\n"));
#if BEEBSCSI_STARTUP_BANNER
   // this won't have changed from last time so don't both
      debugString_P(PSTR("SCSI State: Firmware: "));
      debugString_P(PSTR(FIRMWARE_STRING));
      debugString_P(PSTR("\r\n"));

      // Determine the emulation mode (fixed or LV-DOS)
      if (hostadapterConnectedToExternalBus()) {
         emulationMode = FIXED_EMULATION;
         debugString_P(PSTR("Emulation mode is Winchester (ADFS SCSI-1 hard-drive)\r\n"));
      } else {
         emulationMode = LVDOS_EMULATION;
         debugString_P(PSTR("Emulation mode is Philips VP415 (VFS LaserDisc player)\r\n"));
      }
#endif
      debugString_P(PSTR("\r\n"));
   }

   // Clear the request sense error globals
   for (lunNumber = 0; lunNumber < MAX_LUNS; lunNumber++) {
      requestSenseData[lunNumber] = NO_ERROR;
   }
   ourscsiid = scsiid; // Set the SCSI ID for this device
   fcodeClearBuffer(); // clear the FCODE buffer

   // Ensure the SCSI bus phase is BUS FREE
   scsiState = SCSI_BUSFREE;
   scsiEmulationBusFreestate = 0;
}

// Process the SCSI emulation
void scsiProcessEmulation(void)
{
   /* A selection can only be made from BUS FREE: the host raises SEL, we
      answer with BSY, and SEL drops (hostadapterWriteBusyFlag).  So SEL seen
      while we are in any other state has exactly one meaning - the host has
      restarted underneath us, a CTRL-BREAK mid-transfer, and is now waiting
      for BSY on a command we still believe is in progress.  ADFS has no
      timeout, so nothing else would ever end that wait.  Abandon ours and go
      through BUS FREE's full clear so the selection is taken cleanly.

      This is what makes recovery independent of how promptly anyone noticed
      the reset: the nRST interrupt tears the bus down within microseconds,
      but the poll loop can be inside a handler when it fires, and this rule
      holds whether that teardown was early, late, or never happened. */
   if (scsiState != SCSI_BUSFREE && hostadapterReadSelectFlag()) {
      scsiState = SCSI_BUSFREE;
      scsiEmulationBusFreestate = 0;
   }

   // Process SCSI emulation state
   switch (scsiState) {
      // Handle SCSI bus states:
      case SCSI_BUSFREE:  scsiState = scsiEmulationBusFree(); break;
      case SCSI_COMMAND:  scsiState = scsiEmulationCommand(); break;
      case SCSI_STATUS:   scsiState = scsiEmulationStatus();  break;
      case SCSI_MESSAGE:  scsiState = scsiEmulationMessage(); break;

      // Handle SCSI commands:
      case SCSI_TESTUNITREADY: scsiState = scsiCommandTestUnitReady(); break;
      case SCSI_REZEROUNIT:    scsiState = scsiCommandRezeroUnit();    break;
      case SCSI_REQUESTSENSE:  scsiState = scsiCommandRequestSense();  break;
      case SCSI_FORMAT:        scsiState = scsiCommandFormat();        break;
      case SCSI_REASSIGNBLOCKS:scsiState = scsiCommandReassignBlocks();break;
      case SCSI_READ6:         scsiState = scsiCommandRead6();         break;
      case SCSI_WRITE6:        scsiState = scsiCommandWrite6();        break;
      case SCSI_SEEK:          scsiState = scsiCommandSeek();          break;
      case SCSI_TRANSLATE:     scsiState = scsiCommandTranslate();     break;
      case SCSI_MODESELECT6:   scsiState = scsiCommandModeSelect6();   break;
      case SCSI_MODESENSE6:    scsiState = scsiCommandModeSense6();    break;
      case SCSI_STARTSTOP:     scsiState = scsiCommandStartStop();     break;
      case SCSI_VERIFY:        scsiState = scsiCommandVerify();        break;
      case SCSI_READCAPACITY:  scsiState = scsiCommandReadCapacity();  break;
		case SCSI_READDEFECTDATA10:scsiState = scsiCommandReadDefectData10(); break;
      case SCSI_INQUIRY:         scsiState = scsiCommandInquiry();          break;
      case SCSI_SENDDIAGNOSTIC:  scsiState = scsiCommandSendDiagnostic();   break;

      // Handle LV-DOS specific group 6 commands
      case SCSI_WRITE_FCODE:  scsiState = scsiWriteFCode(); break;
      case SCSI_READ_FCODE:   scsiState = scsiReadFCode();  break;

      // Handle BeebSCSI specific group 6 commands
      case SCSI_BEEBSCSI_SENSE:  scsiState = scsiBeebScsiSense();  break;
      case SCSI_BEEBSCSI_SELECT: scsiState = scsiBeebScsiSelect(); break;
      case SCSI_BEEBSCSI_FATPATH:scsiState = scsiBeebScsiFatPath();break;
      case SCSI_BEEBSCSI_FATINFO:scsiState = scsiBeebScsiFatInfo();break;
      case SCSI_BEEBSCSI_FATREAD:scsiState = scsiBeebScsiFatRead();break;

      // Handle Vendor specific group 7 commands
      case SCSI_CERTIFY: scsiState = scsiCommandCertify(); break;

      default:
      if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: ERROR: Invalid SCSI state!\r\n"));
   }

   // Show activity using the status LED on whenever we are not in the bus free state
   if (scsiState == SCSI_BUSFREE) statusledActivity(0); else statusledActivity(1);
}

// SCSI Bus state emulation functions -------------------------------------------------------------

// Function to set the bus signals for the various
// Information transfer phases
static void scsiInformationTransferPhase(uint8_t transferPhase)
{
   // Note: (from the SCSI specification documentation)
   // MSG   CD IO
   // 0  0  0  Data out phase
   // 0  0  1  Data in phase
   // 0  1  0  Command phase
   // 0  1  1  Status phase
   // 1  0  0  *
   // 1  0  1  *
   // 1  1  0  Message out phase
   // 1  1  1  Message in phase

   switch(transferPhase) {
      case ITPHASE_DATAOUT:
      hostadapterWriteDataPhaseFlags(false, false, false); // MSG, CD, IO
      if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: Information transfer phase: Data out\r\n"));
      break;

      case ITPHASE_DATAIN:
      hostadapterWriteDataPhaseFlags(false, false, true); // MSG, CD, IO
      if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: Information transfer phase: Data in\r\n"));
      break;

      case ITPHASE_COMMAND:
      hostadapterWriteDataPhaseFlags(false, true, false); // MSG, CD, IO
      if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: Information transfer phase: Command\r\n"));
      break;

      case ITPHASE_STATUS:
      hostadapterWriteDataPhaseFlags(false, true, true); // MSG, CD, IO
      if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: Information transfer phase: Status\r\n"));
      break;

      case ITPHASE_MESSAGEOUT:
      hostadapterWriteDataPhaseFlags(true, true, false); // MSG, CD, IO
      if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: Information transfer phase: Message out\r\n"));
      break;

      case ITPHASE_MESSAGEIN:
      hostadapterWriteDataPhaseFlags(true, true, true); // MSG, CD, IO
      if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: Information transfer phase: Message in\r\n"));
      break;
   }
}

// SCSI Bus free state
static uint8_t scsiEmulationBusFree(void)
{

   switch (scsiEmulationBusFreestate)
   {
      case 0:
         if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: Bus Free\r\n"));

         // Clear reset condition
         hostadapterWriteResetFlag(false);

         // Reset bus to Data out IT phase
         scsiInformationTransferPhase(ITPHASE_DATAOUT);

         // Clear busy and request flags
         hostadapterWriteBusyFlag(false);
         hostadapterWriteRequestFlag(false);

         // Wait for selection (or reset condition)
         //while ( (!hostadapterReadSelectFlag()) && (!hostadapterReadResetFlag()) );
         scsiEmulationBusFreestate=1; // fall through

      case 1:
         /* Service a FIQ-latched jukebox poke here: the machine parks in
            this sub-state on every idle poll, and selection has not been
            accepted yet (BSY still clear), so stopping the swapped
            directory's LUNs can never close a FIL an in-flight command is
            using. Placing it
            in case 0 would delay service until after the NEXT command
            completes, letting a mount sequence interleave two discs. */
         hd_juke_service();
         hd_card_service();   /* the *FX SD card eject/insert/reboot, same reasoning */
         fcodePoll();      /* deliver any completed F-code acknowledgement */

         if (!hostadapterReadSelectFlag()) return SCSI_BUSFREE;
      //   if (hostadapterReadResetFlag()) {scsiEmulationBusFreestate=0; return SCSI_BUSFREE;}
         // Read the host ID (from the host databus)
         scsiHostID = hostadapterReadDatabus();
         if ((ourscsiid != 0) && (scsiHostID != ourscsiid)) {
            // SCSI ID not for us
            return SCSI_BUSFREE;
         }

         // Set busy flag to active
         hostadapterWriteBusyFlag(true);

         // We are now in the selected state
         if (debugFlag_scsiState) debugStringInt16_P(PSTR("SCSI State: Selected by host ID "), scsiHostID, true);
         scsiEmulationBusFreestate = 0;
         break;
   }
         // Transition to command state
   return SCSI_COMMAND;
}

// SCSI Command state
// Validates the command sent and returns the command
uint8_t scsiEmulationCommand(void)
{
   uint8_t commandDataBlockPointer = 0;

   if (debugFlag_scsiState) debugString_P(PSTR("SCSI State: Command phase\r\n"));

   // Set signals to indicate command state on the bus
   scsiInformationTransferPhase(ITPHASE_COMMAND);

   // Get the first byte of the command
   commandDataBlock.data[commandDataBlockPointer] = hostadapterReadByte();

   // Decode the CDB 1st byte
   uint8_t group = (commandDataBlock.data[0] & 0xE0) >> 5;
   uint8_t opCode = (commandDataBlock.data[0] & 0x1F);
   uint8_t length;

   // Set the length of the CDB based on the command group
   switch (group) {
      case 0: length = 6; break;
      case 1: length = 10;break;
      case 6: length = 6; break;
      case 7: length = 6; break;

      default:
      length = 6; // guess at a length !
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: ERROR: BAD command group received\r\n"));
      break;
   }

   // Show CDB byte 0 decode
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: CDB byte 0 decode: "));
      debugStringInt16_P(PSTR("Command group "), group, false);
      debugStringInt16_P(PSTR(" ("), length, false);
      debugStringInt16_P(PSTR(" bytes) opcode "), opCode, true);
      // Show received byte value
      debugString_P(PSTR("SCSI Commands: Received command operand bytes:"));
      debugStringInt16_P(PSTR(" "), commandDataBlock.data[commandDataBlockPointer], false);
   }

   // Next byte...
   commandDataBlockPointer++;

   // Get the remainder of the CDB bytes;
   while (commandDataBlockPointer < length) {
      commandDataBlock.data[commandDataBlockPointer] = hostadapterReadByte();

      // Show received byte value
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR(" "), commandDataBlock.data[commandDataBlockPointer], false);

      commandDataBlockPointer++;
   }
   if (debugFlag_scsiCommands) debugString_P(PSTR("\r\n"));

   // A host reset or reselection while the CDB was being read leaves stale
   // bus bytes in it: never run that as a command
   if (hostadapterReadResetFlag()) return SCSI_BUSFREE;

  // if the format Drive[1,2,4,8].0 was used, convert to to Drive 1.[0 1 2 3]

   uint32_t newLUN = scsiTransformLUNid(commandDataBlock.data[1]);
	commandDataBlock.data[1] = (uint8_t)(newLUN & 0xFF);
#ifdef DEBUG
   memcpy(scsiDiagCdb, commandDataBlock.data, sizeof(scsiDiagCdb));
   scsiDiagCmdCount++;
#endif
   // Decode the target LUN
   commandDataBlock.targetLUN = ((newLUN & 0x1E0) >> 5);

   // A drive with its own controller keeps its sense data until the next
   // command that is not REQUEST SENSE (measured on a real drive: a second
   // REQUEST SENSE still gives the same sense key); the Adaptec boards clear
   // it when it has been sent (below).
   if (scsiControllerIsDrive() && (commandDataBlock.targetLUN < 8) && (commandDataBlock.data[0] != 0x03))
      requestSenseData[commandDataBlock.targetLUN] = NO_ERROR;

   // Only the commands the controller has, for the drives behind it (LUNs
   // 0-7).  Pi1MHz's own group 6 commands and the VFS LUNs are not the
   // controller's, so they are never refused here.
   if ((commandDataBlock.targetLUN < 8) && (group != 6)
       && !scsiControllerHasCommand(commandDataBlock.data[0])) {
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: not a command of this controller\r\n"));
      commandDataBlock.status = SCSI_STATUS_CHECK_COND;
      requestSenseData[commandDataBlock.targetLUN] = INVALID_COMMAND;
      return SCSI_STATUS;
   }

   // Transition to command based on received opCode (group 0 commands)
   if (group == 0) {
      // Select group 0 command type
      switch (opCode) {
         case 0x00: return SCSI_TESTUNITREADY; break;
         case 0x01: return SCSI_REZEROUNIT;    break;
         case 0x03: return SCSI_REQUESTSENSE;  break;
         case 0x04: return SCSI_FORMAT;        break;
         case 0x07: return SCSI_REASSIGNBLOCKS;break;
         case 0x08: return SCSI_READ6;         break;
         case 0x0A: return SCSI_WRITE6;        break;
         case 0x0B: return SCSI_SEEK;          break;
         case 0x0F: return SCSI_TRANSLATE;     break;
         case 0x12: return SCSI_INQUIRY;       break;
         case 0x15: return SCSI_MODESELECT6;   break;
         case 0x1A: return SCSI_MODESENSE6;    break;
         case 0x1B: return SCSI_STARTSTOP;     break;
         case 0x1D: return SCSI_SENDDIAGNOSTIC;break;
      }
   }

   // Transition to command based on received opCode (group 1 commands)
   if (group == 1) {
      // Select group 1 command type
      switch (opCode) {
         case 0x05: return SCSI_READCAPACITY;     break;
         case 0x0F: return SCSI_VERIFY;           break;
         case 0x17: return SCSI_READDEFECTDATA10; break;
      }
   }

   // Transition to command based on received opCode (group 6 LV-DOS commands)
   if (group == 6 && (scsiHostID == 16)) {
      // Select group 6 command type
      switch (opCode) {
         case 0x0A: return SCSI_WRITE_FCODE; break;
         case 0x08: return SCSI_READ_FCODE;  break;
      }
   }

   // Transition to command based on received opCode (group 6 BeebSCSI commands)
   if (group == 6) {
      // Select group 6 command type
      switch (opCode) {
         case 0x10: return SCSI_BEEBSCSI_SENSE;   break;
         case 0x11: return SCSI_BEEBSCSI_SELECT;  break;
         case 0x12: return SCSI_BEEBSCSI_FATPATH; break;
         case 0x13: return SCSI_BEEBSCSI_FATINFO; break;
         case 0x14: return SCSI_BEEBSCSI_FATREAD; break;
      }
   }
   // Transition to command based on received opCode (group 7 Vendor specific commands)
   if (group == 7) {
      // Select group 7 command type
      switch (opCode) {
         case 0x02: return SCSI_CERTIFY; break;
		}
	}

   // Unrecognized command received!
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: ERROR: BAD opcode received - transitioning to bus free\r\n"));

	// prevent the requester from potentially hanging if it sends an unknown command
	// Indicate unsuccessful command in status and message
	commandDataBlock.status = SCSI_STATUS_CHECK_COND;      // 0x02 = Bad

   // Set request sense error globals (without, REQUEST SENSE says NO ERROR)
   requestSenseData[commandDataBlock.targetLUN] = INVALID_COMMAND; // 20 Invalid command
   return SCSI_STATUS;
}

// SCSI status state
uint8_t scsiEmulationStatus(void)
{
   if (debugFlag_scsiState) debugStringInt16_P(PSTR("SCSI State: Status.  Status byte = "), commandDataBlock.status, true);

   // Set signals to indicate status state on the bus
   scsiInformationTransferPhase(ITPHASE_STATUS);

   // Write the status byte to the host
   hostadapterWriteByte(commandDataBlock.status);

   // Transition to SCSI message state
   return SCSI_MESSAGE;
}

// SCSI message in state
uint8_t scsiEmulationMessage(void)
{
   if (debugFlag_scsiState) debugStringInt16_P(PSTR("SCSI State: Message In.  Message byte = "), 0x00, true);

   // Set signals to indicate message in state on the bus
   scsiInformationTransferPhase(ITPHASE_MESSAGEIN);

   // Write the message byte to the host
   hostadapterWriteByte(0x00);

   // Transition to the bus free state (command is complete)
   return SCSI_BUSFREE;
}

// SCSI command execution functions -----------------------------------------------------

// SCSI Command (0x00) TestUnitReady
//
// Adaptec ACB-4000 Manual notes:
// The TEST UNIT READY command verifies that the drive is powered on
// and the DRIVE READY line is true. Once the drive is re-zeroed,
// with no errors, this command will check to see that the drive is
// ready to write and read data. Drive write fault condition is also
// checked.
uint8_t scsiCommandTestUnitReady(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: TESTUNITREADY command (0x00) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // Check to see if the requested LUN is started
   if (filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // Indicate successful command in status and message
      commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   } else {
      // Indicate unsuccessful command in status and message
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Unavailable LUN #"), commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Failed

      // Set request sense error globals

      requestSenseData[commandDataBlock.targetLUN] = scsiUnitNotReady(); // Unit not ready

      return SCSI_STATUS;
   }

   return SCSI_STATUS;
}

// SCSI Command (0x01) ReZero Unit
//
// Only used by removable media in the LV-DOS emulation mode
//
// VP415 LVDP Manual notes:
// Displays the logical picture zero of the volume that is accessed through
// the logic unit number <>f the command. If successful, the status
// returned is 0x00; otherwise the status returned is 0x02.
static uint8_t scsiCommandRezeroUnit(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: REZEROUNIT command (0x01) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // Check to see if the requested LUN is started
   if (filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // Indicate successful command in status and message
      commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   } else {
      // Indicate unsuccessful command in status and message
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Unavailable LUN #"), commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Failed
      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = scsiUnitNotReady(); // Unit not ready

      return SCSI_STATUS;
   }

   return SCSI_STATUS;
}

// SCSI Command (0x03) RequestSense
//
// Adaptec ACB-4000 Manual notes:
// The REQUEST SENSE (03 hex) command is the command that gives the
// most detailed description of status from the controller. This
// command is needed whenever a CHECK STATUS is found in the
// COMPLETION STATUS BYTE. If the CHECK BIT is set after a command,
// a REQUEST SENSE must follow in order to read status and clear the
// check condition.
//
// Note: This command is used by the host after another command returns
// a 'bad' status (0x02).  It is a request for a more detailed error code.
// If the command is incorrectly handled it will cause ADFS to hang...
// The 4-byte (ACB-4000) error held for a LUN as SCSI extended sense: 70h
// (F0h with the address valid, the LBA in bytes 3-6), the sense key in byte
// 2, 0Ah more bytes, the additional sense code in byte 12.  ADFS reads byte 0
// as its error number, the FileStore byte 12.
static void scsiExtendedSense(uint32_t error, uint8_t sense[18])
{
   static const struct { uint8_t code, key, asc; } map[] = {
      { 0x00, 0x0, 0x00 },   // no error
      { 0x02, 0x2, 0x04 },   // not ready
      { 0x04, 0x2, 0x04 },
      { 0x03, 0x4, 0x03 },   // write fault
      { 0x1C, 0x3, 0x31 },   // bad format: medium format corrupted
      { 0x1A, 0x5, 0x24 },   // interleave error: invalid field in CDB
      { 0x20, 0x5, 0x20 },   // invalid command operation code
      { 0x21, 0x5, 0x21 },   // logical block address out of range
      { 0x22, 0x5, 0x22 },   // illegal function
      { 0x24, 0x5, 0x24 },   // invalid field in CDB
      { 0x25, 0x5, 0x25 },   // logical unit not supported
      { 0x26, 0x5, 0x26 },   // invalid field in parameter list
   };
   uint8_t code = (uint8_t)((error >> 24) & 0x7F);
   bool addressValid = (error & 0x80000000u) != 0;
   uint8_t key = 0x4, asc = code;       // anything else: hardware error
   for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
      if (map[i].code == code) {
         key = map[i].key;
         asc = map[i].asc;
         break;
      }
   }
   memset(sense, 0, 18);
   sense[0] = addressValid ? 0xF0 : 0x70;
   sense[2] = key;
   if (addressValid) {
      sense[4] = (uint8_t)((error >> 16) & 0x1F);
      sense[5] = (uint8_t)(error >> 8);
      sense[6] = (uint8_t)error;
   }
   sense[7] = 0x0A;                     // additional sense length
   sense[12] = asc;
}

static uint8_t scsiCommandRequestSense(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: REQUESTSENSE command (0x03) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);

   // Since this command provides additional error information, it is available even if the
   // LUN is unavailable (no LUN image on file system).

      // The ACB-4000 manual (section 5.5.1) states that, if the number of sense bytes
      // is less than 4, it should default to four bytes

      // Note: The ACB-4000 manual is a little confusing around the subject of the
      // number of bytes.  There doesn't seem to be any condition in which the
      // number would be anything other than 4 (nor any result with less than 4 bytes).

    debugStringInt16_P(PSTR("SCSI Commands: Sense bytes = "), (commandDataBlock.data[4]<4)?4:commandDataBlock.data[4], true);
   }
   // Set up the control signals ready for the data in phase
   scsiInformationTransferPhase(ITPHASE_DATAIN);

   // Is there an error waiting to be reported?
#ifdef DEBUG
   if (requestSenseData[commandDataBlock.targetLUN] == NO_ERROR) {
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: No error flagged\r\n"));
   } else {
      // Assemble request sense error response
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Error flagged - sending to host\r\n"));
   }
#endif
	// TO DO - should this section be in the preceding if ... else ? or is all the data always output

   if (scsiControllerIsDrive() && (commandDataBlock.targetLUN < 8)) {
      // A drive with its own controller sends extended sense
      uint8_t sense[18];
      scsiExtendedSense(requestSenseData[commandDataBlock.targetLUN], sense);
      // Cut to the allocation length; 0 means 4 bytes (as a CCS drive's
      // specification gives REQUEST SENSE)
      uint8_t length = commandDataBlock.data[4] ? commandDataBlock.data[4] : 4;
      for (uint8_t i = 0; (i < length) && (i < sizeof sense); i++)
         hostadapterWriteByte(sense[i]);
   } else {
   hostadapterWriteByte((uint8_t)((requestSenseData[commandDataBlock.targetLUN] & 0xFF000000) >> 24)); // Error code
   hostadapterWriteByte((uint8_t)((requestSenseData[commandDataBlock.targetLUN] & 0x1F0000) >> 16)); // LBA
   hostadapterWriteByte((uint8_t)((requestSenseData[commandDataBlock.targetLUN] & 0x00FF00) >> 8)); // LBA
   hostadapterWriteByte((uint8_t)((requestSenseData[commandDataBlock.targetLUN] & 0x0000FF))); // LBA
   }

   // Clear the request sense error reporting globals - on an Adaptec board;
   // a drive with its own controller keeps them for the next command
   if (!scsiControllerIsDrive() || (commandDataBlock.targetLUN >= 8))
      requestSenseData[commandDataBlock.targetLUN] = NO_ERROR;

   // Indicate successful command in status and message
   commandDataBlock.status =  0x00; // 0x00 = Good

   return SCSI_STATUS;
}

// SCSI Command (0x04) Format
//
// Adaptec ACB-4000 Manual notes:
// The FORMAT command writes drive characteristics, ID and data
// fields onto the drive and writes a fill pattern into the user
// data field. This fill pattern can be changed by use of the FORMAT
// command. This feature is useful in writing worst case data
// patterns onto the drive at format time.
//
// When using the FORMAT command, a drive defect list can be
// appended to the command in a cylinder, head and "bytes from
// index" form. This form is the same form that most drive
// manufacturers use. This form can also be generated from the drive
// by using the TRANSLATE with the SEARCH DATA NOT EQUAL command.
static uint8_t scsiCommandFormat(void)
{
   // Format unit command parameters:
   uint8_t formatOptions;
   uint8_t dataPattern;
   uint16_t interleave;

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: FORMAT command (0x04) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   /* VFS write-protect: never create or format a LUN >= 8. No legitimate
      sender exists (VFS ROM never emits FORMAT) - refuse as a tripwire. */
   if (commandDataBlock.targetLUN >= 8) {
      commandDataBlock.status = SCSI_STATUS_CHECK_COND;
      requestSenseData[commandDataBlock.targetLUN] = WRITE_FAULT;
      return SCSI_STATUS;
   }

   // Interpret command parameters
   formatOptions = (commandDataBlock.data[1] & 0x1F);
   interleave = (uint16_t)((commandDataBlock.data[3] << 8) + commandDataBlock.data[4]);

   if (scsiControllerIsAdaptec()) {
      // The Adaptec takes no defect list, or the cylinder/head/bytes-from-
      // index list (with or without its own fill byte): byte 1 00, 02, 1C or
      // 1E; "all other combinations ... rejected" (OEM manual 6-8)
      if ((formatOptions != 0x00) && (formatOptions != 0x02) &&
          (formatOptions != 0x1C) && (formatOptions != 0x1E)) {
         commandDataBlock.status = SCSI_STATUS_CHECK_COND;
         requestSenseData[commandDataBlock.targetLUN] = BAD_ARG;
         return SCSI_STATUS;
      }
      // The fill byte is CDB byte 2 when bit 1 says so, else 6C
      dataPattern = (formatOptions & 0x02) ? commandDataBlock.data[2] : 0x6C;
   } else if (scsiControllerIsDrive()) {
      // As a drive's own firmware does: the control byte must be 0
      if (commandDataBlock.data[5] != 0) {
         commandDataBlock.status = SCSI_STATUS_CHECK_COND;
         requestSenseData[commandDataBlock.targetLUN] = BAD_ARG;
         return SCSI_STATUS;
      }
      dataPattern = commandDataBlock.data[2];
   } else {
      dataPattern = commandDataBlock.data[2];     // Mixed: as it always was
   }

   // Make sure the target LUN is started
   bool noImage = false;                        // the image is made for this FORMAT (if not refused)
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // If the LUN is unavailable we need to create the LUN image on the file system
      // before formatting it.
      noImage = true;
      if (!filesystemCreateLunImage(commandDataBlock.targetLUN)) {
         // Could not create LUN image... return with error status
         if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: ERROR: Could not create new LUN image for LUN #"), commandDataBlock.targetLUN, true);
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

         // Set request sense error globals
         requestSenseData[commandDataBlock.targetLUN] = BAD_FORMAT; // Unformatted or Bad format

         // The LUN is in an unknown state... Stop the LUN
         filesystemSetLunStatus(commandDataBlock.targetLUN, false);
         return SCSI_STATUS;
      }
   }

   if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Format option = "), formatOptions, true);

   // The interleave limit (sectors per track - 1, error 24 or 1A) is the
   // controller's: scsiFormatGeometry below checks it.

   // Note: The defect list is ignored by the emulation since it does not apply
   // to the emulated file system; this code is included for debug completion.

   // If specified (by the options) read the defect list (ACB-4000 figure 5-6)
   if (formatOptions == 28 || formatOptions == 30) {
      uint8_t Buffer[8];
      // Read the defect list
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Reading the defect list:\r\n"));

      // Set up the control signals ready for the data out phase
      scsiInformationTransferPhase(ITPHASE_DATAOUT);

      // Read the defect list header
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Defect list header:\r\n"));
      for (size_t byteCounter = 0; byteCounter < 4; byteCounter++)
        Buffer[byteCounter] = hostadapterReadByte();

      uint16_t defectListLength = (uint16_t) (((Buffer[2] << 8) + Buffer[3]) / 8);
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands:   Length = "), defectListLength, true);

      // Read the defect records
      for (uint16_t defectListRecords = 0; defectListRecords < defectListLength; defectListRecords++) {
         // Read the defect data
         for (size_t byteCounter = 0; byteCounter < 8; byteCounter++)
           // cppcheck-suppress unreadVariable
           Buffer[byteCounter] = hostadapterReadByte();

         // Output defect to debug
         if (debugFlag_scsiCommands) {
            debugStringInt16_P(PSTR("SCSI Commands: Defect #"), defectListRecords, true);
            debugStringInt32_P(PSTR("SCSI Commands:   Cylinder = "),
         ((uint32_t)Buffer[0] << 16) + ((uint32_t)Buffer[1] << 8) + (uint32_t)Buffer[2],
         true);
            debugStringInt16_P(PSTR("SCSI Commands:   Head = "), Buffer[3], true);
            debugStringInt32_P(PSTR("SCSI Commands:   Bytes = "),
         ((uint32_t)Buffer[4] << 24) + ((uint32_t)Buffer[5] << 16) +
         ((uint32_t)Buffer[6] << 8) + (uint32_t)Buffer[7],
         true);
         }
      }
   }

   // A host reset or reselection while the defect list was being read:
   // the command is dead - abandon it rather than format the image
   if (hostadapterReadResetFlag()) return SCSI_BUSFREE;

   // The controller decides the sectors per track (on the ACB-4000 from the
   // block size and interleave: scsiFormatGeometry)
   if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Interleave = "), interleave, true);

   uint32_t error = scsiFormatGeometry(commandDataBlock.targetLUN, interleave);
   if ((error != NO_ERROR) && (error != BAD_FORMAT)) {
      // A parameter the controller refuses: the .cfg and the image are left
      // as they were, and so is the drive - scsiFormatGeometry stopped it,
      // and the restart reads its .cfg afresh (pending MODE SELECT pages
      // stay pending) rather than leaving it "not ready" until START UNIT.
      // An image made just now for it goes again: a refused FORMAT leaves nothing
      if (noImage)
         filesystemRemoveEmptyLunImage(commandDataBlock.targetLUN);
      filesystemSetLunStatus(commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND;
      requestSenseData[commandDataBlock.targetLUN] = error;
      return SCSI_STATUS;
   }

   // Create/recreate the LUN data file according to the drive descriptor and fill
   // with the required data pattern byte:
   if ((error != NO_ERROR) || !filesystemFormatLun(commandDataBlock.targetLUN, dataPattern)) {
      // Formatting failed...
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Format failed\r\n"));

      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = BAD_FORMAT; // 1C Bad format

      // The LUN is in an unknown state... Flag the LUN as unavailable
      filesystemSetLunStatus(commandDataBlock.targetLUN, false);

      return SCSI_STATUS;
   }

   // Tell the file system to start the new LUN
   filesystemSetLunStatus(commandDataBlock.targetLUN, true);   // Needed to prevent verify looping forever

   // Indicate successful command in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good

   return SCSI_STATUS;
}

// SCSI Command (0x07) Reassign Blocks
//
// Reads the blocks sent for reassignment, but does nothing with them
//
static uint8_t scsiCommandReassignBlocks(void)
{

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: ReassignBlocks command (0x07) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

	uint8_t longlba;
	bool longlist = false;

	longlist = ((commandDataBlock.data[1] & 1) == 1);

	// If the LongLBA bit 1 is 0, LBA is 4 bytes, otherwise LBA is 8 bytes
	// (bit 0 is LongList, read above - both were being read from bit 0)
	if ((commandDataBlock.data[1] & 2) == 0)
		longlba=4;
	else
		longlba=8;

	uint8_t Buffer[4];

	// Set up the control signals ready for the data out phase
	scsiInformationTransferPhase(ITPHASE_DATAOUT);

	if (debugFlag_scsiCommands)debugString_P(PSTR("Parameter List Header ="));
	// Get the Parameter List Header;
	for (uint8_t byteCounter = 0; byteCounter < 4; byteCounter++) {
		Buffer[byteCounter] = hostadapterReadByte();
		// Show received byte value
		if (debugFlag_scsiCommands)debugStringInt16_P(PSTR(" "), Buffer[byteCounter], false);
	}
	if (debugFlag_scsiCommands)debugString_P(PSTR("\r\n"));

	uint32_t list_length;
	list_length =
	      ((uint32_t)Buffer[2] << 8) |
   	   ((uint32_t)Buffer[3]);

	if (longlist) {
		list_length =
	      ((uint32_t)Buffer[0] << 24) |
	      ((uint32_t)Buffer[1] << 16) |
			list_length;
	}

	if (debugFlag_scsiCommands)debugString_P(PSTR("Defective LBA List ="));

	for (uint32_t Counter = 0; Counter < (list_length/longlba); Counter++) {
		// list_length is built from four host-supplied bytes, so with the
		// LongList bit set it can reach 2^32: up to ~1e9 iterations of four
		// hostadapterReadByte() calls, each of which can burn its full ACK
		// timeout.  Everything shares one poll loop, so that stalls audio,
		// WiFi/AUN, USB and the 1MHz bus with it - and BREAK alone does NOT
		// recover, because hd_wait_ack() returns immediately on reset but
		// the loop still grinds through every remaining iteration.  Bail on
		// reset so the per-byte timeout composes into a per-command bound.
		if (hostadapterReadResetFlag()) return SCSI_BUSFREE;
		/* The bus bytes MUST be consumed regardless of build flavour and debug
		   flags: an earlier form had the hostadapterReadByte() calls inside
		   if (debugFlag_scsiCommands), so a DEBUG build with the flag off never
		   took the defect-list bytes off the bus and desynced the protocol -
		   against a host whose handshake has no timeout.  Read always; only
		   the display is conditional. */
		if (longlba==4)
         {
			uint32_t logicalBlockAddress =
				((uint32_t)hostadapterReadByte() << 24) |
				((uint32_t)hostadapterReadByte() << 16) |
				((uint32_t)hostadapterReadByte() << 8) |
				((uint32_t)hostadapterReadByte());

			if (debugFlag_scsiCommands)
				debugStringInt32_P(PSTR(" "), logicalBlockAddress, false);
			(void)logicalBlockAddress;
      }
		else
		{
			uint64_t logicalBlockAddress64 =
				((uint64_t)hostadapterReadByte() << 56) |
				((uint64_t)hostadapterReadByte() << 48) |
				((uint64_t)hostadapterReadByte() << 40) |
				((uint64_t)hostadapterReadByte() << 32) |
				((uint64_t)hostadapterReadByte() << 24) |
				((uint64_t)hostadapterReadByte() << 16) |
				((uint64_t)hostadapterReadByte() <<  8) |
				((uint64_t)hostadapterReadByte());

			if (debugFlag_scsiCommands) {
				debugStringInt32_P(PSTR(" "), (uint32_t)(logicalBlockAddress64 >> 32), false);
				debugStringInt32_P(PSTR(""), (uint32_t)(logicalBlockAddress64 & 0x00000000FFFFFFFF), false);
         }
			(void)logicalBlockAddress64;
      }
		}
	if (debugFlag_scsiCommands)debugString_P(PSTR("\r\n"));


   // Indicate successful command in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good

   return SCSI_STATUS;
}

// SCSI Command (0x08) Read6
//
// Adaptec ACB-4000 Manual notes:
// The READ command is used to read data from the drive to the
// host.
//
// A COMPLETION STATUS may give a check condition that leads to the
// possible errors of: Bad Argument, all class 00 errors,
// 10 ECC error, 10 address mark not found, seek error and record
// not found, plus others.
//
// If a Data ECC error occurs during the read, the controller will
// re-read the block up to four times to establish a solid error
// correction block (syndrome). Correction may occur after two
// retries are completed if the error syndrome is repeated twice
// consecutively. Correction is done directly into the controller's
// data buffer, transparent to the host.
//
// The host now expects 255 blocks of data from the drive to follow.
// These will be read from the disk, starting at logical block 0
// and continuing to 254.
static uint8_t scsiCommandRead6(void)
{
   uint32_t logicalBlockAddress = 0;
   uint32_t numberOfBlocks = 0;
   uint32_t currentBlock = 0;
   uint8_t *sectorPtr = 0;

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: READ6 command (0x08) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, false);
   }

   // Make sure the target LUN is started
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // Target LUN is not started.  If the LUN is present, then start it, otherwise
      // return an error.  Note: The original Adaptec SCSI host adapter would always
      // auto-start a LUN if it was present, so we duplicate that behavior here even
      // though it is 'more correct' (according to the specs) to return with error

      // Is the requested LUN available?
      if (debugFlag_scsiCommands) debugString_P(PSTR("\r\nSCSI Commands: Attempting to Auto-Start LUN (as it is currently STOPped)\r\n"));

      // If a host transfer (MTP/WebDAV) is rewriting this image, take it back
      // rather than refusing the Beeb: the transfer aborts itself when it
      // notices, and the LUN starts normally here. Refusing instead is what
      // hung the machine in 8d8389e.
      filesystemHostRevokeLun(commandDataBlock.targetLUN);

      // Auto-start the LUN
      if (!filesystemSetLunStatus(commandDataBlock.targetLUN, true)) {
         // Could not start LUN... return with error status
         if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Could not auto-start LUN #"), commandDataBlock.targetLUN, true);
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

         // Set request sense error globals
         requestSenseData[commandDataBlock.targetLUN] = BAD_FORMAT; // 1C Bad format

         return SCSI_STATUS;
      }

      if (debugFlag_scsiCommands) {
         debugString_P(PSTR("SCSI Commands: Requested LUN has been auto-started\r\n"));
         // Output the initial debug again (as the command debug information is appended to it)
         debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, false);
      }
   }

   // Get the starting logical block address from the CDB
   logicalBlockAddress = (((uint32_t)commandDataBlock.data[1] & 0x1F) << 16) |
   ((uint32_t)commandDataBlock.data[2] << 8) |
   ((uint32_t)commandDataBlock.data[3]);

   // Get the requested number of blocks from the CDB
   numberOfBlocks = (uint32_t)commandDataBlock.data[4];
   if (numberOfBlocks == 0) numberOfBlocks = 256; // 0 = 256 blocks according to the SCSI specification

   // Show the command debug information
   if (debugFlag_scsiCommands) {
      debugStringInt32_P(PSTR(", LBA = "), logicalBlockAddress, false);
      debugStringInt32_P(PSTR(", Blocks = "), numberOfBlocks, true);
   }

   // The LUN's block size (256 unless FORMAT laid down another)
   uint32_t blockSize = filesystemGetLunDataBlockSize(commandDataBlock.targetLUN);

   // Set up the control signals ready for the data in phase
   scsiInformationTransferPhase(ITPHASE_DATAIN);

   // Open the required LUN image for reading
   if(!filesystemOpenLunForRead(commandDataBlock.targetLUN, logicalBlockAddress, numberOfBlocks)) {
      // Opening the LUN image failed... try to recover with a little grace...
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: ERROR: Open LUN image failed!\r\n"));
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      // The LUN is in an unknown state... Stop the LUN
      filesystemCloseLunForRead(commandDataBlock.targetLUN);

      return SCSI_STATUS;
   }

   // Transfer the requested blocks from the LUN image to the host
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Transferring requested blocks to the host..."));
   for (currentBlock = 0; currentBlock < numberOfBlocks; currentBlock++) {

      // Read the requested block from the LUN image
      if(!filesystemReadNextSector(commandDataBlock.targetLUN, &sectorPtr)) {
         // Reading from the LUN image failed... try to recover with a little grace...
         if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: ERROR: Could not read next sector from LUN image!\r\n"));
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

         // Set request sense error globals
         requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

         // The LUN is in an unknown state... Stop the LUN
         filesystemCloseLunForRead(commandDataBlock.targetLUN);

         return SCSI_STATUS;
      }

      // Send the data to the host
      cli();
      DEBUG_bytesTransferred(hostadapterPerformReadDMALen(sectorPtr, blockSize));
      sei();
      hd_audio_service();              // between sectors only

      // Check for a host reset condition
      if (hostadapterReadResetFlag()) {
         sei();
         if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Read DMA interrupted by host reset at byte #"), (uint16_t)bytesTransferred, true);

         // Close the currently open LUN image
         filesystemCloseLunForRead(commandDataBlock.targetLUN);

         return SCSI_BUSFREE;
      }

      /* A short DMA means hd_wait_ack() timed out: the host stopped
         servicing REQ without asserting nRST.  The return value used to be
         discarded (the release arm of DEBUG_bytesTransferred only cast it
         to void),
         so the loop simply started the next sector - and a 256-block
         transfer then burned HD_ACK_TIMEOUT_US per sector, 25 s with the
         whole cooperative loop stopped: no 1MHz bus service, no WiFi/AUN,
         no USB.  Bail so the per-sector timeout composes into a
         per-command bound, exactly as scsiCommandReassignBlocks does. */
      if (bytesTransferred < blockSize) {
         filesystemCloseLunForRead(commandDataBlock.targetLUN);
         return SCSI_BUSFREE;
      }

      // Show debug
      if (!debugFlag_scsiBlocks) {
         if (debugFlag_scsiCommands) {
            debugStringInt32_P(PSTR(""), currentBlock, false);
            debugString_P(PSTR(" "));
         }
      } else {
         debugStringInt32_P(PSTR("Hex dump for block #"), currentBlock, true);
         debugSectorBufferHex(sectorPtr, (uint16_t)blockSize);
      }
   }
   if (debugFlag_scsiCommands || debugFlag_scsiBlocks) debugString_P(PSTR("\r\n"));

   // Close the currently open LUN image
   //filesystemCloseLunForRead(commandDataBlock.targetLUN);

   // Indicate successful transfer in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Read6 command successful\r\n"));

   // Transition to the successful state
   return SCSI_STATUS;
}

// SCSI Command (0x0A) Write6
//
// Adaptec ACB-4000 Manual notes:
// The WRITE command is used to write data from the host to the
// disk.
//
// A COMPLETION STATUS may give a check condition that leads to the
// possible errors of: Bad argument, all class 00 errors,
// ID ECC error, ID address mark not found, seek error and record
// not found, plus others.
//
// The controller now expects 255 blocks of data from the host
// adapter to follow. These will be written onto the disk, starting
// at logical block 0 and continuing to 254.
static uint8_t scsiCommandWrite6(void)
{
   uint32_t logicalBlockAddress = 0;
   uint32_t numberOfBlocks = 0;
   uint32_t currentBlock = 0;

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: WRITE6 command (0x0A) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, false);
   }

   /* VFS write-protect: LUN >= 8 is read-only LaserDisc media and the VFS
      ROM never issues WRITE6 (ADFS-multi-target HD_SCSI_VFS build emits only
      READ6/STARTSTOP/FCODE; FS-level writes die with 'Disc read only').
      Refuse at command phase - a tripwire for any unexpected writer. */
   if (commandDataBlock.targetLUN >= 8) {
      commandDataBlock.status = SCSI_STATUS_CHECK_COND;
      requestSenseData[commandDataBlock.targetLUN] = WRITE_FAULT;
      return SCSI_STATUS;
   }

   // Make sure the target LUN is started
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // Target LUN is not started.  If the LUN is present, then start it, otherwise
      // return an error.  Note: The original Adaptec SCSI host adapter would always
      // auto-start a LUN if it was present, so we duplicate that behavior here even
      // though it is 'more correct' (according to the specs) to return with error

      // Is the requested LUN available?
      if (debugFlag_scsiCommands) debugString_P(PSTR("\r\nSCSI Commands: Attempting to Auto-Start LUN (as it is currently STOPped)\r\n"));

      // If a host transfer (MTP/WebDAV) is rewriting this image, take it back
      // rather than refusing the Beeb: the transfer aborts itself when it
      // notices, and the LUN starts normally here. Refusing instead is what
      // hung the machine in 8d8389e.
      filesystemHostRevokeLun(commandDataBlock.targetLUN);

      // Auto-start the LUN
      if (!filesystemSetLunStatus(commandDataBlock.targetLUN, true)) {
         // Could not start LUN... return with error status
         if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Could not auto-start LUN #"), commandDataBlock.targetLUN, true);
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

         // Set request sense error globals
         requestSenseData[commandDataBlock.targetLUN] = BAD_FORMAT; // 1C Bad format
         return SCSI_STATUS;
      }

      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Requested LUN has been auto-started\r\n"));
   }

   // Get the starting logical block address from the CDB
   logicalBlockAddress = (((uint32_t)commandDataBlock.data[1] & 0x1F) << 16) |
   ((uint32_t)commandDataBlock.data[2] << 8) |
   ((uint32_t)commandDataBlock.data[3]);

   // Get the requested number of blocks from the CDB
   numberOfBlocks = (uint32_t)commandDataBlock.data[4];
   if (numberOfBlocks == 0) numberOfBlocks = 256; // 0 = 256 blocks according to the SCSI specification

   // Show the command debug information
   if (debugFlag_scsiCommands) {
      debugStringInt32_P(PSTR(", LBA = "), logicalBlockAddress, false);
      debugStringInt32_P(PSTR(", Blocks = "), numberOfBlocks, true);
   }

   // The LUN's block size (256 unless FORMAT laid down another)
   uint32_t blockSize = filesystemGetLunDataBlockSize(commandDataBlock.targetLUN);

   // Set up the control signals ready for the data out phase
   scsiInformationTransferPhase(ITPHASE_DATAOUT);

   // Open the required LUN image for writing
   if(!filesystemOpenLunForWrite(commandDataBlock.targetLUN, logicalBlockAddress, numberOfBlocks)) {
      // Opening the LUN image failed... try to recover with a little grace...
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Command: ERROR: Could not open LUN image for writing!\r\n"));
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      // The LUN is in an unknown state... Stop the LUN
      filesystemCloseLunForWrite(commandDataBlock.targetLUN);

      return SCSI_STATUS;
   }

   // Transfer the requested blocks from the host to the LUN image
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Transferring requested blocks from the host...\r\n"));
   for (currentBlock = 0; currentBlock < numberOfBlocks; currentBlock++) {
      static uint8_t Buffer[MAX_BLOCK_SIZE];
      // Get the data from the host
      cli();
      DEBUG_bytesTransferred(hostadapterPerformWriteDMALen(Buffer, blockSize));
      sei();
      hd_audio_service();              // between sectors only

      // Check for a host reset condition
      if (hostadapterReadResetFlag()) {
         if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Write DMA interrupted by host reset at byte #"), (uint16_t)bytesTransferred, true);

         // Close the currently open LUN image
         filesystemCloseLunForWrite(commandDataBlock.targetLUN);

         return SCSI_BUSFREE;
      }

      /* A short DMA means hd_wait_ack() timed out: the host stopped
         servicing REQ without asserting nRST.  The return value used to be
         discarded (the release arm of DEBUG_bytesTransferred only cast it
         to void),
         so the loop simply started the next sector - and a 256-block
         transfer then burned HD_ACK_TIMEOUT_US per sector, 25 s with the
         whole cooperative loop stopped: no 1MHz bus service, no WiFi/AUN,
         no USB.  Bail so the per-sector timeout composes into a
         per-command bound, exactly as scsiCommandReassignBlocks does. */
      if (bytesTransferred < blockSize) {
         filesystemCloseLunForWrite(commandDataBlock.targetLUN);
         return SCSI_BUSFREE;
      }

      // Write the requested block to the LUN image
      if(!filesystemWriteNextSector(commandDataBlock.targetLUN, Buffer)) {
         // Writing to the LUN image failed... try to recover with a little grace...
         if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: ERROR: Writing to LUN image failed!\r\n"));
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

         // Set request sense error globals

         requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

         // The LUN is in an unknown state... Stop the LUN
         filesystemCloseLunForWrite(commandDataBlock.targetLUN);

         return SCSI_STATUS;
      }

      // Show debug
      if (!debugFlag_scsiBlocks) {
         if (debugFlag_scsiCommands) {
            debugStringInt32_P(PSTR(""), currentBlock, false);
            debugString_P(PSTR(" "));
         }
      } else {
         debugStringInt32_P(PSTR("Hex dump for block #"), currentBlock, true);
         debugSectorBufferHex(Buffer, (uint16_t)blockSize);
      }
   }
   if (debugFlag_scsiCommands || debugFlag_scsiBlocks) debugString_P(PSTR("\r\n"));

   // Close the currently open LUN image
   // filesystemCloseLunForWrite(commandDataBlock.targetLUN);

   // Indicate successful transfer in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Write6 command successful\r\n"));

   // Transition to the successful state
   return SCSI_STATUS;
}

// SCSI Command (0x0B) Seek
//
// This command is reported to be used in some (unknown) ADFS utilities.  It
// doesn't serve a purpose for the file system implementation, but is included for
// completeness.  It simply returns successfully if the specified LUN is available.
static uint8_t scsiCommandSeek(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: SEEK command (0x0B) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // Check to see if the requested LUN is started
   if (filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // Indicate successful command in status and message
      commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   } else {
      // Indicate unsuccessful command in status and message
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Unavailable LUN #"), commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Failed

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      return SCSI_STATUS;
   }

   return SCSI_STATUS;
}

// SCSI Command (0x0F) Translate
//
// Adaptec ACB-4000 Manual notes:
// This command performs a logical address to physical address
// translation and returns the physical location of the requested
// block address in a cylinder, head, bytes from index format. This
// data can be used to build a defect list for the FORMAT command.
//
// Note: This translation isn't really required since
// the physical storage geometry doesn't have any head or cylinders.
// However, to emulate defect mapping (i.e. make the emulation act
// like a physical SCSI drive) it's good to include it anyway.
/* Upstream's TRANSLATE refused a stopped LUN; here it auto-starts it, as
   READ6 and WRITE6 do.  The upstream check stays behind a named gate, not
   deleted, so future BeebSCSI diffs stay clean. */
#define BEEBSCSI_TRANSLATE_REFUSES_STOPPED 0

static uint8_t scsiCommandTranslate(void)
{
   uint32_t cylinderNumber;
   uint32_t headNumber;
   uint32_t bytesFromIndex;
   uint32_t logicalBlockAddress;
   uint32_t headsPerCylinder;

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: TRANSLATE command (0x0F) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }
#if BEEBSCSI_TRANSLATE_REFUSES_STOPPED
   // Make sure the target LUN is started
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // LUN unavailable... return with error status
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Unavailable LUN #"), commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready
      return SCSI_STATUS;
   }
#endif
// Make sure the target LUN is started
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // Target LUN is not started.  If the LUN is present, then start it, otherwise
      // return an error.  Note: The original Adaptec SCSI host adapter would always
      // auto-start a LUN if it was present, so we duplicate that behavior here even
      // though it is 'more correct' (according to the specs) to return with error

      // Is the requested LUN available?
      if (debugFlag_scsiCommands) debugString_P(PSTR("\r\nSCSI Commands: Attempting to Auto-Start LUN (as it is currently STOPped)\r\n"));

      // If a host transfer (MTP/WebDAV) is rewriting this image, take it back
      // rather than refusing the Beeb: the transfer aborts itself when it
      // notices, and the LUN starts normally here. Refusing instead is what
      // hung the machine in 8d8389e.
      filesystemHostRevokeLun(commandDataBlock.targetLUN);

      // Auto-start the LUN
      if (!filesystemSetLunStatus(commandDataBlock.targetLUN, true)) {
         // Could not start LUN... return with error status
         if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Could not auto-start LUN #"), commandDataBlock.targetLUN, true);
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

         // Set request sense error globals
         requestSenseData[commandDataBlock.targetLUN] = BAD_FORMAT; // 1C Bad format

         return SCSI_STATUS;
      }

      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Requested LUN has been auto-started\r\n"));
   }

   // Get the logical block address from the CDB
   logicalBlockAddress = ((uint32_t)(commandDataBlock.data[1] & 0x1F) << 16) |
   ((uint32_t)commandDataBlock.data[2] << 8) |
   ((uint32_t)commandDataBlock.data[3]);

   // We have to translate the LBA (in the CDB) into an 8 byte response:
   // 00 Cylinder number (MSB)
   // 01 Cylinder number
   // 02 Cylinder number (LSB)
   // 03 Head number
   // 04 Bytes from index (MSB)
   // 05 Bytes from index
   // 06 Bytes from index
   // 07 Bytes from index (LSB)

   // From Wikipedia (https://en.wikipedia.org/wiki/Logical_block_addressing):
   //
   // CHS tuples can be mapped to LBA address with the following formula:
   //
   // LBA = (C x HPC + H) x SPT + (S - 1)
   //
   // where
   //
   // C, H and S are the cylinder number, the head number, and the sector number
   // LBA is the logical block address
   // HPC is the maximum number of heads per cylinder (reported by disk drive, typically 16 for 28-bit LBA)
   // SPT is the maximum number of sectors per track (reported by disk drive, typically 63 for 28-bit LBA)
   //
   // LBA addresses can be mapped to CHS tuples with the following formula ("mod" is the modulo operation,
   // i.e. the remainder, and "/" is integer division, i.e. the quotient of the division where any fractional
   // part is discarded):
   //
   // C = LBA / (HPC * SPT)
   // H = (LBA / SPT) mod HPC
   // S = (LBA mod SPT) + 1

   // Note: The 'bytes from index' is the number of bytes from the start of the track (to the defect)

   // Read the geometry description for the LUN into the sector buffer

   if (!filesystemReadLunDescriptor(commandDataBlock.targetLUN)) {
      // Unable to read drive descriptor! Exit with error status
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      debugString_P(PSTR("SCSI Commands: ERROR: Could not read geometry from LUN descriptor\r\n"));

      // Transition to the STATUS state
      return SCSI_STATUS;
   }

   // Get the number of heads per cylinder (HPC)
   headsPerCylinder = filesystemGetheadspercylinder(commandDataBlock.targetLUN); // Data head count
	uint32_t sectorsperTrack = filesystemGetLunSPTSize(commandDataBlock.targetLUN);

   // Convert LBA to CHS
   cylinderNumber = logicalBlockAddress / (headsPerCylinder * sectorsperTrack);
   headNumber = (logicalBlockAddress / sectorsperTrack) % headsPerCylinder;
   bytesFromIndex = ((logicalBlockAddress % sectorsperTrack) + 1) * filesystemGetLunBlockSize(commandDataBlock.targetLUN); // Sector number * block size

   if (debugFlag_scsiCommands) {
      debugStringInt32_P(PSTR("SCSI Commands:   LBA = "), logicalBlockAddress, true);
      debugStringInt32_P(PSTR("SCSI Commands:   Heads/Cyl = "), headsPerCylinder, true);
      debugStringInt32_P(PSTR("SCSI Commands:   Cylinder = "), cylinderNumber, true);
      debugStringInt32_P(PSTR("SCSI Commands:   Sectors per Track = "), sectorsperTrack, true);
      debugStringInt32_P(PSTR("SCSI Commands:   Head = "), headNumber, true);
      debugStringInt32_P(PSTR("SCSI Commands:   Bytes = "), bytesFromIndex, true);
   }

   // Set up the control signals ready for the data in phase
   scsiInformationTransferPhase(ITPHASE_DATAIN);

   // Send the translation data to the host
   hostadapterWriteByte((uint8_t)((cylinderNumber & 0xFF0000) >> 16));     // Cylinder number MSB
   hostadapterWriteByte((uint8_t)((cylinderNumber & 0xFF00) >> 8));     	// Cylinder number
   hostadapterWriteByte((uint8_t)(cylinderNumber & 0xFF));              	// Cylinder number LSB

   hostadapterWriteByte((uint8_t)headNumber);                        		// Head number

   hostadapterWriteByte((uint8_t)((bytesFromIndex & 0xFF000000) >> 24));   // Bytes from index MSB
   hostadapterWriteByte((uint8_t)((bytesFromIndex & 0x00FF0000) >> 16));   // Bytes from index
   hostadapterWriteByte((uint8_t)((bytesFromIndex & 0x0000FF00) >> 8)); 	// Bytes from index
   hostadapterWriteByte((uint8_t)( bytesFromIndex & 0x000000FF));       	// Bytes from index LSB

   // Indicate successful command in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good

   return SCSI_STATUS;
}

// MODE SELECT on an Adaptec board: the flat parameter list of OEM manual
// 6.3.1.11 - a 4-byte header (byte 3 = 8), the 8-byte extent descriptor
// (density 0, block size 256, 512 or 1024) and the 10-byte drive parameter
// list (list format 01, cylinders, heads, reduced write current, write
// precompensation, landing zone, step rate); 24 bytes with list format 02
// (hard-sectored), and 12 with no drive parameters at all.  Anything else
// is error 24.  The block size waits for FORMAT (scsiFormatGeometry lays it
// down); until then the data path keeps the image's own block size.
static uint32_t scsiAdaptecModeSelect(const uint8_t *list, uint8_t length)
{
   uint8_t lun = commandDataBlock.targetLUN;

   if (((length != 12) && (length != 22) && (length != 24)) || (list[3] != 8))
      return BAD_ARG;
   uint32_t blockSize = ((uint32_t)list[9] << 16) | ((uint32_t)list[10] << 8) | list[11];
   if ((list[4] != 0) || ((blockSize != 256) && (blockSize != 512) && (blockSize != 1024)))
      return BAD_ARG;
   if (length > 12) {
      uint16_t cylinders = (uint16_t)((list[13] << 8) | list[14]);
      bool format = (list[12] == 0x01) ? (length == 22) : (list[12] == 0x02) && (length == 24);
      if (!format || (cylinders == 0) || (cylinders > ACB_MAX_CYLINDERS) || (list[15] == 0) ||
          (list[15] > ACB_MAX_HEADS))
         return BAD_ARG;
   }

   if (debugFlag_scsiCommands) debugString_C(PSTR("SCSI Commands: Writing MODE SELECT cfg\r\n"), DEBUG_SUCCESS);
   scsiSetPendingBlockSize(lun, blockSize);
   if (length > 12) {
      // the drive parameter list is page 0 (the hard-sector bytes of a
      // 24-byte list are not kept: Pi1MHz has no hard-sectored drive)
      filesystemWriteModePageData(lun, 0, 10, &list[12]);
      // update Page 4 NB we should do page 3 but that never changes
      filesystemCopyPage0toPage4(lun);
   }
   return NO_ERROR;
}

// MODE SELECT on a drive with its own controller, as a CCS drive's own
// firmware takes it:
// - The 4-byte header.  A drive with no block descriptor (ModeParamHeader
//   byte 3 = 0) wants it all zero: anything in bytes 1-3 is
//   refused, ASC 22h.  A drive with one takes a descriptor of 8 bytes, whose
//   block length FORMAT lays down.
// - Then the pages.  A page code with bit 6 set, a page the drive does not
//   hold, or one sent longer than the drive's own: ASC 26h.  Bit 7 with a
//   page length of 0 puts the page back to its factory values (FDefPage).
//   Otherwise the page is merged in under its mask (a cut-short page keeps
//   the rest; with no mask, a zero numeric field of page 3 or 4 keeps the
//   drive's value).
// - Pages 3 and 4 are checked (scsiCheckModePage) and wait for FORMAT;
//   the other pages take effect at once.
// The whole list is checked before any of it is used.
static uint32_t scsiDriveModeSelect(uint8_t *list, uint8_t length)
{
   uint8_t lun = commandDataBlock.targetLUN;
   size_t headerLength = 0;
   const char *header = filesystemGetModeParamHeaderData(lun, &headerLength);
   bool hasDescriptor = (header != NULL) && (headerLength > 3) && ((uint8_t)header[3] != 0);
   uint32_t descriptorBlockSize = 0;
   uint8_t start = 4;

   if (!hasDescriptor) {
      if ((list[1] | list[2] | list[3]) != 0)
         return ILLEGAL_FUNCTION;
   } else if (list[3] != 0) {
      if ((list[3] != 8) || (length < 12))
         return BAD_PARAM_FIELD;
      // block length 0: the drive's own
      descriptorBlockSize = ((uint32_t)list[9] << 16) | ((uint32_t)list[10] << 8) | list[11];
      if ((descriptorBlockSize != 0) &&
          ((descriptorBlockSize < MIN_BLOCK_SIZE) || (descriptorBlockSize > MAX_BLOCK_SIZE)))
         return ILLEGAL_FUNCTION;
      start = 12;
   }

   // Pass 1: check every page, and work out pages 3 and 4 as they will be
   static uint8_t pending[2][256];
   size_t pendingLength[2] = { 0, 0 };
   bool pendingSet[2] = { false, false };
   for (unsigned int at = start; at + 2u <= length; ) {
      uint8_t code = list[at];
      uint8_t page = code & 0x3F;
      uint8_t sent = list[at + 1];
      size_t storedLength = 0;
      const char *stored = filesystemGetModePageData(lun, page, &storedLength);
      if ((code & 0x40) || (stored == NULL) || (storedLength < 2))
         return BAD_PARAM_FIELD;
      if ((code & 0x80) && (sent == 0)) {
         if ((page == 3) || (page == 4)) {
            // back to the factory values (none given: the current ones)
            size_t factoryLength = 0;
            const char *factory = filesystemGetFDefPageData(lun, page, &factoryLength);
            if ((factory == NULL) || (factoryLength == 0)) {
               factory = stored;
               factoryLength = storedLength;
            }
            memcpy(pending[page == 4], factory, factoryLength);
            pendingLength[page == 4] = factoryLength;
            pendingSet[page == 4] = true;
         }
         at += 2u;
         continue;
      }
      if (sent > (uint8_t)stored[1])
         return BAD_PARAM_FIELD;
      /* A page may arrive shorter than its length byte says: FServFmt sends
         len+4 bytes where a whole page needs len+6.  The drive takes what
         arrives and keeps the rest, as a real drive's firmware does. */
      unsigned int have = (unsigned int)length - at;
      unsigned int want = (unsigned int)sent + 2u;
      if (have > want)
         have = want;
      if ((page == 3) || (page == 4)) {
         int i = (page == 4);
         size_t baseLength = 0;
         const char *base = pendingSet[i] ? (const char *)pending[i]
                                          : scsiGetPendingModePageData(lun, page, &baseLength);
         if (pendingSet[i])
            baseLength = pendingLength[i];
         uint8_t after[256];
         list[at] = page;
         size_t afterLength = filesystemModePageAfterSelect(lun, page, base, baseLength,
                                                            (uint8_t)want, (uint8_t)have, &list[at], after);
         uint32_t check = scsiCheckModePage(lun, page, &list[at], (uint8_t)have, after, afterLength);
         if (check != NO_ERROR)
            return check;
         memcpy(pending[i], after, afterLength);
         pendingLength[i] = afterLength;
         pendingSet[i] = true;
      }
      at += have;
   }

   // Pass 2: the other pages take effect now
   for (unsigned int at = start; at + 2u <= length; ) {
      uint8_t code = list[at];
      uint8_t page = code & 0x3F;
      uint8_t sent = list[at + 1];
      if ((code & 0x80) && (sent == 0)) {
         size_t factoryLength = 0;
         const char *factory = filesystemGetFDefPageData(lun, page, &factoryLength);
         if ((page != 3) && (page != 4) && (factory != NULL) && (factoryLength != 0))
            filesystemSetModePageRaw(lun, page, (const uint8_t *)factory, factoryLength);
         at += 2u;
         continue;
      }
      unsigned int have = (unsigned int)length - at;
      unsigned int want = (unsigned int)sent + 2u;
      if (have > want)
         have = want;
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI commands: Writing MODE SELECT Page "), page, true);
      // a page header with no data changes nothing
      if ((page != 3) && (page != 4) && (have > 2u)) {
         list[at] = page;
         filesystemMergeModePageData(lun, page, (uint8_t)want, (uint8_t)have, &list[at], true);
      }
      at += have;
   }
   for (int i = 0; i < 2; i++)
      if (pendingSet[i])
         scsiSetPendingModePageData(lun, (uint8_t)(3 + i), pending[i], pendingLength[i]);
   if (descriptorBlockSize != 0)
      scsiSetPendingBlockSize(lun, descriptorBlockSize);
   return NO_ERROR;
}

// MODE SELECT under Mixed (the default), as Pi1MHz has always taken it: a
// 22-byte list with an 8-byte block descriptor is the Adaptec's, whose drive
// parameter list goes into page 0 (and page 4); any other list is pages,
// each merged into the page the drive holds.
static uint32_t scsiMixedModeSelect(uint8_t *Buffer, uint8_t length)
{
   // we skip the 4 byte header
   uint8_t start = 4;

   // if the length is 22 and byte is 8 then the drive descriptor is being written
   if ((length == 22) && Buffer[3]== 8 )
   {
     if (debugFlag_scsiCommands) debugString_C(PSTR("SCSI Commands: Writing MODE SELECT cfg\r\n"), DEBUG_SUCCESS);
     // we can skip LBADescriptor 8 bytes
     start = (uint8_t)(start + 8);
     // update  ModePage0
     filesystemWriteModePageData(commandDataBlock.targetLUN, 0, 22-4-8, &Buffer[start] );
     // update  Page 4 NB we should do page 3 but that never changes
     filesystemCopyPage0toPage4(commandDataBlock.targetLUN);
   }
   else
   {
      // MODE SELECT Parameters : 0 0 0 0 37 6 40  67  41  65 13 30
      //                                  38 6 99 111 114 110 13  0

      // we are updating a standard page here

      // then we have page number and length
      // we might have more that one page so loop if there is more data
      while ( start+2 < length )
      {
         uint8_t page = Buffer[start] & 0x3F ; //
         Buffer[start] &= 0x3F ;
         uint8_t len = Buffer[start+1];

         if (debugFlag_scsiCommands)debugStringInt16_P(PSTR("SCSI commands: Writing MODE SELECT Page "), page, true);
         /* The Acorn FileStore tools (FServFmt, FSFormat) send a 3000-series
            drive its pages two bytes short.  A real drive takes what arrives
            and keeps the rest of the page. */
         unsigned int have = (unsigned int)length - start;
         unsigned int want = (unsigned int)len + 2;
         if (have > want)
            have = want;
         filesystemMergeModePageData(commandDataBlock.targetLUN, page, (uint8_t)want, (uint8_t)have, &Buffer[start], false);
         start = (uint8_t)(start + have );
         if ( page == 4 )
            {
            // update Page 0 NB we should do page 3 but that never changes
            filesystemCopyPage4toPage0(commandDataBlock.targetLUN);
            }
      }
   }
   return NO_ERROR;
}

// SCSI Command (0x15) ModeSelect6
//
// Adaptec ACB-4000 Manual notes:
// This command is used in the ACB-4000 Series Controllers to
// specify format parameters and should always precede the FORMAT
// command. When a blown format error (code 1C) is detected
// due to the controller being unable to read the drive
// information from a drive already formatted, the user should use
// this command to inform the controller about the drive
// information. Once initialized, most data on the drive will be
// recoverable. The information can then be recovered and the drive
// reformatted, and writes to the drive will not be permitted.
//
// Note: This function writes the LUN descriptor to the file system
// containing the drive geometry information
static uint8_t scsiCommandModeSelect6(void)
{
   uint8_t byteCounter;
	uint8_t length = commandDataBlock.data[4];

   if (debugFlag_scsiCommands) {
      debugString_C("SCSI Commands: MODESELECT6 command (0x15) received\r\n", DEBUG_SCSI_COMMAND);
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   if (length < 4  ) {
      commandDataBlock.status = SCSI_STATUS_CHECK_COND;
      requestSenseData[commandDataBlock.targetLUN] = BAD_ARG;
      return SCSI_STATUS;
   }

   uint8_t Buffer[256]; // length is a uint8_t so 256 always suffices (avoids a VLA)

   // Make sure the target LUN is started (the state owner, not the image
   // check it delegates to - a MODE SELECT with no MOUNT before it must leave
   // the LUN in exactly the state a START would).  Not a VFS LUN (>= 8): its
   // list is read below and discarded.
   if ((commandDataBlock.targetLUN < 8) && !filesystemSetLunStatus(commandDataBlock.targetLUN, true)) {
      // If the target LUN is unavailable then the host is probably attempting to MODESELECT
      // a LUN for which no descriptor exists.  So here we create the LUN descriptor
      if(!filesystemCreateLunDescriptor(commandDataBlock.targetLUN)) {
         // LUN descriptor is unavailable and cannot be created... return with error status
         if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Create descriptor failed LUN #"), commandDataBlock.targetLUN, true);
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

         // Set request sense error globals

         requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

         return SCSI_STATUS;
      }

      // LUN files created
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Created descriptor for LUN #"), commandDataBlock.targetLUN, true);
   }

	// Set up the control signals ready for the data out phase
	scsiInformationTransferPhase(ITPHASE_DATAOUT);

   if (debugFlag_scsiCommands)debugString_P(PSTR("MODE SELECT Parameters :"));

	// Get the incoming bytes;
	for (byteCounter = 0; byteCounter < length; byteCounter++) {
		Buffer[byteCounter] = hostadapterReadByte();
		// Show received byte value
		if (debugFlag_scsiCommands)debugStringInt16_P(PSTR(" "), Buffer[byteCounter], false);
	}
	if (debugFlag_scsiCommands)debugString_P(PSTR("\r\n"));

   // A host reset or reselection mid-transfer leaves stale bus bytes in
   // Buffer: abandon the command rather than save them to the .cfg
   if (hostadapterReadResetFlag()) return SCSI_BUSFREE;

   /* A VFS LUN (>= 8) is read-only media: the list is accepted and dropped -
      no descriptor made, nothing stored - and the host gets GOOD, never an
      error its handshake could hang on. */
   if (commandDataBlock.targetLUN >= 8) {
      commandDataBlock.status = SCSI_STATUS_OK;
      return SCSI_STATUS;
   }

   // The list is the controller's: the Adaptec's flat list, a SCSI drive's
   // header, block descriptor and pages, or Pi1MHz's own mixed list
   uint32_t error = scsiControllerIsAdaptec() ? scsiAdaptecModeSelect(Buffer, length)
                  : scsiControllerIsDrive()   ? scsiDriveModeSelect(Buffer, length)
                                              : scsiMixedModeSelect(Buffer, length);
   if (error != NO_ERROR) {
      commandDataBlock.status = SCSI_STATUS_CHECK_COND;
      requestSenseData[commandDataBlock.targetLUN] = error;
      return SCSI_STATUS;
   }

   filesystemConfigToLunGeometry(commandDataBlock.targetLUN);

   if (!filesystemWriteAttributes(commandDataBlock.targetLUN))
   {
      // Unable to write attributes! Exit with error status
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      debugString_P(PSTR("SCSI Commands: ERROR: Could not write attributes\r\n"));

      // Transition to the STATUS state
      return SCSI_STATUS;
   }

   // Indicate successful command in status and message
   if (debugFlag_scsiCommands) debugString_C(PSTR("SCSI Commands: Writing MODE SELECT successful\r\n"), DEBUG_SUCCESS);
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   return SCSI_STATUS;
}

// One mode page's bytes of one kind (0 current, 1 changeable, 2 factory)
// into out; 0 if the drive does not hold the page or it does not fit.
// No MaskPage line: a full-length all-zero mask.  No FDefPage line: the
// current values.  Page 0 (the ACB-4000 parameter list) has current values only.
static size_t scsiModePageOfKind(uint8_t lun, uint8_t page, int kind, char * out, size_t room)
{
   size_t len = 0, alt = 0;
   const char * cur = filesystemGetModePageData(lun, page, &len);
   if ((cur == NULL) || (len == 0) || (len > room))
      return 0;
   if (page == 0 && kind != 0 && !scsiControllerIsDrive())
      return 0;      // the Adaptec list has current values only
   const char * src = cur;
   if (kind == 2) {
      const char * f = filesystemGetFDefPageData(lun, page, &alt);
      if ((f != NULL) && (alt > 0) && (alt <= room)) { src = f; len = alt; }
   } else if (kind == 1) {
      // The mask over the page's own length; the page code and length bytes
      // are the page's own (a MaskPage mask may hold 00 00 there).
      const char * w = filesystemGetMaskPageData(lun, page, &alt);
      memset(out, 0, len);
      if ((w != NULL) && (alt > 0))
         memcpy(out, w, (alt < len) ? alt : len);
      out[0] = cur[0];
      if (len > 1) out[1] = cur[1];
      return len;
   }
   memcpy(out, src, len);
   return len;
}

// MODE SENSE on an Adaptec board: the same flat list MODE SELECT takes,
// whatever page is asked for - the 4-byte header (byte 3 = 8), the 8-byte
// extent descriptor with the block size, and the 10-byte drive parameter
// list (OEM manual 6.3.1.11; ACB-4000A Users Manual 5.3.13): 22 bytes, cut
// to the allocation length.  SuperForm asks for it on LUN 0 with
// 1A 00 00 00 16 00.  These are the bytes Pi1MHz has always sent for page 0.
static uint8_t scsiAdaptecModeSense(const char *header, size_t headerLength,
                                    const char *extent, size_t extentLength)
{
   uint8_t lun = commandDataBlock.targetLUN;
   uint8_t list[22];
   size_t pageLength = 0;
   const char *drive = filesystemGetModePageData(lun, 0, &pageLength);

   memset(list, 0, sizeof list);
   // Byte 0 is reserved (the mode data length of later SCSI is not sent)
   if (headerLength > 1) list[1] = (uint8_t)header[1];
   if (headerLength > 2) list[2] = (uint8_t)header[2];
   list[3] = 8;                         // the extent descriptor's length
   memcpy(&list[4], extent, (extentLength < 8) ? extentLength : 8);
   if ((drive != NULL) && (pageLength > 0)) {
      memcpy(&list[12], drive, (pageLength < 10) ? pageLength : 10);
   } else if (scsiGeometryFromCfgOnly() && (filesystemGetModePageData(lun, 4, &pageLength) == NULL)) {
      // DriveGeometry=CFG and no list in the .cfg (page 0, or page 4, whose cylinders
      // and heads are the geometry then and are sent below): as a disc without the
      // board's parameters on cylinder 0 ("blown format", OEM manual App. A)
      commandDataBlock.status = SCSI_STATUS_CHECK_COND;
      requestSenseData[lun] = BAD_FORMAT;
      return SCSI_STATUS;
   } else {
      // No list in the .cfg: the geometry it was given (the defaults of
      // filesystemLunToconfigGeometry)
      uint32_t cylinders = filesystemGetLunCylinders(lun);
      list[12] = 0x01;
      list[13] = (uint8_t)(cylinders >> 8);
      list[14] = (uint8_t)cylinders;
      list[15] = (uint8_t)filesystemGetheadspercylinder(lun);
      list[17] = 0x80;
      list[19] = 0x80;
      list[21] = 0x01;
   }

   uint8_t length = commandDataBlock.data[4];
   if (length > sizeof list)
      length = sizeof list;
   if (length != 0) {
      scsiInformationTransferPhase(ITPHASE_DATAIN);
      for (uint8_t i = 0; i < length; i++)
         hostadapterWriteByte(list[i]);
   }
   commandDataBlock.status = SCSI_STATUS_OK;
   return SCSI_STATUS;
}

// SCSI Command (0x1A) ModeSense6
//
// Adaptec ACB-4000 Manual notes:
// This command is used to interrogate the ACB-4000A and ACB-4070
// device parameter table to determine the specific characteristics
// of any disk drive currently attached. The attached drive must
// have been formatted by an ACB-4000A or ACB-4070 for this to be a
// legal command.
static uint8_t scsiCommandModeSense6(void)
{

   uint8_t sizerequested = commandDataBlock.data[4];

   if (debugFlag_scsiCommands) {
      debugString_C(PSTR("SCSI Commands: MODESENSE6 command (0x1A) received\r\n"),DEBUG_SCSI_COMMAND);
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
	   debugStringInt16_P(PSTR("SCSI Commands: Mode Page requested = "), commandDataBlock.data[2], true);
	}

	// We do not check if the LUN is available since there (at this point) may only be a descriptor
	// file for the LUN.  If the descriptor cannot be read we assume that the LUN is completely unavailable
   // so create a dummy descriptor if needed


   if (!filesystemReadLunDescriptor(commandDataBlock.targetLUN)) {
      // Unable to read drive descriptor! Exit with error status
   /*
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      debugString_P(PSTR("SCSI Commands: ERROR: Could not read geometry from LUN descriptor\r\n"));

      // Transition to the STATUS state
      return SCSI_STATUS;
   */
   }

   const char * headerptr, *LBAptr, *modeptr;
   size_t headerlen_sz, LBAlen_sz, modelen_sz;

   headerptr = filesystemGetModeParamHeaderData(commandDataBlock.targetLUN, &headerlen_sz);
   LBAptr = filesystemGetLBADescriptorData(commandDataBlock.targetLUN, &LBAlen_sz);

   if ( (headerptr == NULL) || (LBAptr == NULL)) {
      // Unable to read drive descriptor! Exit with error status
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      debugString_P(PSTR("SCSI Commands: ERROR: Could not read geometry from LUN descriptor\r\n"));

      // Transition to the STATUS state
      return SCSI_STATUS;
   }

   if (scsiControllerIsAdaptec() && (commandDataBlock.targetLUN < 8)) {
      // The board has no page codes: bytes 1 (bits 4-0), 2, 3 and 5 of the
      // CDB are reserved and must be 0 (OEM manual 6.2.1, 6.3.1.12), so a
      // host asking for a page gets bad argument, as from the real board.
      if (((commandDataBlock.data[1] & 0x1F) | commandDataBlock.data[2] |
           commandDataBlock.data[3] | commandDataBlock.data[5]) != 0) {
         commandDataBlock.status = SCSI_STATUS_CHECK_COND;
         requestSenseData[commandDataBlock.targetLUN] = BAD_ARG;
         return SCSI_STATUS;
      }
      return scsiAdaptecModeSense(headerptr, headerlen_sz, LBAptr, LBAlen_sz);
   }

   // Which values, and which pages.  An early CCS drive (PageControl=CCS)
   // has no page control field: it refuses page control 1-3,
   // and pages 3D, 3E and 3F give every page's factory, changeable and
   // current values.  Otherwise page control 1 is the changeable mask, 2 the
   // factory values, 3 the saved (= current) values, and page 3F every page.
   uint8_t lun = commandDataBlock.targetLUN;
   uint8_t pageControl = (uint8_t)(commandDataBlock.data[2] >> 6);
   uint8_t pageCode = (uint8_t)(commandDataBlock.data[2] & 0x3F);
   bool ccs = filesystemPageControlCCS(lun);
   int kind = 0;                          // 0 current, 1 changeable, 2 factory
   bool allPagesWanted = false;
   if (ccs) {
      if (pageControl != 0) {
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad
         requestSenseData[lun] = BAD_ARG; // 0x24 Bad argument
         return SCSI_STATUS;
      }
      if (pageCode >= 0x3D) {
         allPagesWanted = true;
         kind = (pageCode == 0x3D) ? 2 : (pageCode == 0x3E) ? 1 : 0;
      }
   } else {
      kind = (pageControl == 1) ? 1 : (pageControl == 2) ? 2 : 0;
      allPagesWanted = (pageCode == 0x3F);
   }

   static char modeReply[256];
   size_t total = 0;
   // Page 0 is an ordinary page here (some drives hold it, others do not,
   // and a page not held is refused below); the all-pages reply leaves it
   // out, as a CCS drive's specification says
   if (allPagesWanted) {
      uint8_t last = ccs ? 0x3C : 0x3E;
      for (uint8_t page = 0x01; page <= last; page++)
         total += scsiModePageOfKind(lun, page, kind, &modeReply[total], sizeof modeReply - total);
      modeptr = modeReply;
   } else {
      total = scsiModePageOfKind(lun, pageCode, kind, modeReply, sizeof modeReply);
      modeptr = total ? modeReply : NULL;
   }
   modelen_sz = total;

   if (modeptr == NULL) {
      // A page this drive does not have: the request is invalid, the drive
      // is not "not ready" (a host would wait for it to become ready).
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad
      requestSenseData[lun] = BAD_ARG; // 0x24 Bad argument
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: MODESENSE6: page not held\r\n"));
      return SCSI_STATUS;
   }

   if (sizerequested == 0) {
      // Allocation length of zero: transfer no data (not an error)
      commandDataBlock.status = SCSI_STATUS_OK;
      return SCSI_STATUS;
   }

   // Set up the control signals ready for the data in phase
   scsiInformationTransferPhase(ITPHASE_DATAIN);
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Sending Page Mode data to host\r\n"));

   // The getters report their sizes as size_t; the transfer arithmetic below
   // works in int (values are a handful of bytes, so the narrowing is safe).
   int headerlen = (int)headerlen_sz;
   int LBAlen = (int)LBAlen_sz;
   // The block descriptor goes out only when the header says it does (byte 3,
   // the descriptor length): a drive without one sends 0.
   if ((headerlen > 3) && ((uint8_t)headerptr[3] < LBAlen))
      LBAlen = (uint8_t)headerptr[3];
   int modelen = (int)modelen_sz;
   int length = headerlen+LBAlen+modelen;

   if (sizerequested < length) {
       length = sizerequested;
   }

   if (commandDataBlock.data[2] == 0x00) {
      // send length ( page zero is reserved)
      hostadapterWriteByte(0);
      if (debugFlag_scsiCommands)debugStringInt16_P(PSTR(" "), 0, false);
   } else {
      // send length
      hostadapterWriteByte((uint8_t)(length - 1 ));
      if (debugFlag_scsiCommands)debugStringInt16_P(PSTR(" "), (uint16_t)(length - 1), false);
   }

   // send header, without exceeding the allocation length (the length
   // byte above stands in for headerptr[0] and already used one byte)
   int sendlen = headerlen - 1;
   if (sendlen > length - 1)
      sendlen = length - 1;
   for (int i = 1; i <= sendlen; i++)
   {
      hostadapterWriteByte(headerptr[i]);
      if (debugFlag_scsiCommands)debugStringInt16_P(PSTR(" "), headerptr[i], false);
   }

   length -= sendlen + 1;

   if (length < LBAlen)
         LBAlen = length;

   for (int i = 0; i < LBAlen; i++)
   {
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR(" "), LBAptr[i], false);
      hostadapterWriteByte(LBAptr[i]);
   }

   length -= LBAlen;
   if (length < modelen)
         modelen = length;

   for (int i = 0; i < modelen; i++)
   {
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR(" "), modeptr[i], false);
      hostadapterWriteByte(modeptr[i]);
   }
   length -= modelen;

   for (int i = 0; i < length; i++)
      hostadapterWriteByte(0); // pad with zeros
   // Show the command debug information
    debugString_P(PSTR("SCSI Commands: MODESENSE6 command complete\r\n"));
   // Indicate successful command in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good

   return SCSI_STATUS;
}

// SCSI Command (0x1B) StartStop
//
// Adaptec ACB-4000 Manual notes:
// Byte 04, bit 00 of this command should be set to 01 if this is a
// START command, and 00 for a STOP command.
//
// This command is designed for use on drives with a designated
// shipping or landing zone.
//
// A STOP command will position the head to the landing zone
// position. See the MODE SELECT command for description of the
// landing zone value.
static uint8_t scsiCommandStartStop(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: STARTSTOP command (0x1B) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // Is this a START or STOP command?
   if ((commandDataBlock.data[4] & 0x01) == 0) {
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Stopping LUN\r\n"));

      // Make the target LUN unavailable
      filesystemSetLunStatus(commandDataBlock.targetLUN, false);

      // SCSI eject (LoEj, byte 4 bit 1) on a VFS LUN = the AIV disc flip.
      // fcode.c owns that rule, including the tray-open state a host's
      // post-eject poll waits for - this used to re-derive the partner
      // arithmetic here and skip the tray state entirely.
      if ((commandDataBlock.data[4] & 0x02) && commandDataBlock.targetLUN >= 8)
         fcode_disc_flip();
   } else {
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Starting LUN\r\n"));

      // *MOUNT arrives here rather than through the auto-start route, so it
      // needs the same hand-back: take the LUN off any host transfer, then
      // start it as normal.
      filesystemHostRevokeLun(commandDataBlock.targetLUN);

      // Start the LUN
      if (!filesystemSetLunStatus(commandDataBlock.targetLUN, true)) {
         // Could not start LUN... return with error status
         if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Could not start LUN #"), commandDataBlock.targetLUN, true);
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

         // Set request sense error globals
         requestSenseData[commandDataBlock.targetLUN] = BAD_ARG; // Bad argument

         return SCSI_STATUS;
      }
   }

   // Exit with success
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good

   // Transition to the successful state
   return SCSI_STATUS;
}

// SCSI Command (0x2F) Verify
//
// Adaptec ACB-4000 Manual notes:
//
// This command is similar to the previous WRITE AND VERIFY except
// that it verifies the ECC of an already existing set of data
// blocks. No write Operation is performed. It is up to the host
// to provide data for rewriting and correcting if an ECC error
// is detected.
//
// Note: Group 1 commands accept an extended LBA (4 bytes) and
//       a 16-bit 'number of blocks' (i.e. up to 65536 blocks)
//
// Note: This function is used by the *VERIFY command provided in the
//       library directory of the BBC Master welcome disc
//
// This implementation only verifies the LUN is available, and the LBAs
// given are within range on the LUN.
//
static uint8_t scsiCommandVerify(void)
{
   uint32_t logicalBlockAddress = 0;
   uint32_t lunSizeInSectors = 0;

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: VERIFY command (0x2F) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // Make sure the target LUN is started
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // LUN unavailable... return with error status
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("\r\nSCSI Commands: Unavailable LUN #"), commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad
      requestSenseData[commandDataBlock.targetLUN] = scsiUnitNotReady(); // not ready

      return SCSI_STATUS;
   }

   // Get the logical block address from the CDB (note: this is different from G0 commands
   // as 4 bytes of LBA are provided)
   logicalBlockAddress =
      ((uint32_t)commandDataBlock.data[2] << 24) |
      ((uint32_t)commandDataBlock.data[3] << 16) |
      ((uint32_t)commandDataBlock.data[4] << 8) |
      ((uint32_t)commandDataBlock.data[5]);

   // Show the command debug information
   if (debugFlag_scsiCommands) {
      // Get the requested number of blocks
      // Get the requested number of blocks from the CDB

      // A length of 0 verifies nothing: only the start address is checked
      debugStringInt32_P(PSTR(", LBA = "), logicalBlockAddress, false);
      debugStringInt32_P(PSTR(", number of blocks = "),
        ((uint32_t)commandDataBlock.data[7] << 8) | ((uint32_t)commandDataBlock.data[8])
      , true);
   }

   // A start address at or past the end is error 21 with the address valid
   // (A1, the address in the sense bytes) - SuperForm verifies with length
   // 0 in steps of &10000 blocks and stops only on error 21 or 23.

	// Read the cached parameters
	lunSizeInSectors = filesystemGetLunTotalSectors(commandDataBlock.targetLUN);

   // Check that the LBA is within range of the LUN size
   if (logicalBlockAddress >= lunSizeInSectors) {
      // Out of range
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: ERROR: Requested LBA is out-of-range for the LUN size - Verify failed\r\n"));

      // Set error status
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = ILLEGAL_ADDR | (logicalBlockAddress & 0x00FFFFFF); // Illegal block address

      return SCSI_STATUS;
   } else {
      // In range

      // Indicate successful command in status and message
      commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   }

   return SCSI_STATUS;
}

// SCSI Command (0x25) Read Capacity
//
// Adaptec ACB-4000 Manual notes:
//
// If the Partial Media Indicator (PMI) is 00 hex, this command will
// return the address of the last block on the unit. It is not
// necessary to specify a starting block address in this command
// mode. If PMI is 01 hex, this command will return the
// address of the block (after the specified starting address)
// at which a substantial delay of time in data transfer will be
// encountered (e.g., a cylinder boundary). Any value other than
// 00 hex or 01 hex in byte 08 will cause Check Status with an
// error code of 24 hex for an invalid argument.
//
// In both cases, an eight-byte data field is returned. The first
// four bytes are defined as the block address and the last four
// bytes are the block size.
//
static uint8_t scsiCommandReadCapacity(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: Read Capacity command (0x25) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // Make sure the target LUN is started
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // LUN unavailable... return with error status
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("\r\nSCSI Commands: Unavailable LUN #"), commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad
      requestSenseData[commandDataBlock.targetLUN] = scsiUnitNotReady(); // not ready
      return SCSI_STATUS;
   }

   // The ACB-4000 manual requires PMI (CDB byte 8) to be 0x00 or 0x01;
   // any other value must be rejected with a Bad Argument (0x24) error
   if (commandDataBlock.data[8] != 0x00 && commandDataBlock.data[8] != 0x01) {
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: ERROR: Invalid PMI byte in READ CAPACITY\r\n"));
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad
      requestSenseData[commandDataBlock.targetLUN] = BAD_ARG; // 0x24 Bad argument
      return SCSI_STATUS;
   }

   uint32_t lunsize = filesystemGetLunTotalSectors(commandDataBlock.targetLUN)-1;

   // Set up the control signals ready for the data in phase
   scsiInformationTransferPhase(ITPHASE_DATAIN);

   // Send the Read Capacity data to the host
   // 4 bytes last block address
   hostadapterWriteByte((uint8_t)((lunsize & 0xFF000000) >> 24));    // Last block MSB
   hostadapterWriteByte((uint8_t)((lunsize & 0x00FF0000) >> 16));    // Last block
   hostadapterWriteByte((uint8_t)((lunsize & 0x0000FF00) >>  8));    // Last block
   hostadapterWriteByte((uint8_t)(lunsize & 0x000000FF));            // Last block LSB

	// four bytes block size
   uint32_t blocksize = filesystemGetLunBlockSize(commandDataBlock.targetLUN);
   hostadapterWriteByte((uint8_t)((blocksize & 0xFF000000) >> 24));  // Bytes from index MSB
   hostadapterWriteByte((uint8_t)((blocksize & 0x00FF0000) >> 16));  // Bytes from index
   hostadapterWriteByte((uint8_t)((blocksize & 0x0000FF00) >>  8));  // Bytes from index
   hostadapterWriteByte((uint8_t)( blocksize & 0x000000FF));         // Bytes from index LSB

   // Indicate successful command in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   return SCSI_STATUS;
}

// SCSI Command (0x37) Read Defect Data10
//
// The READ DEFECT DATA (10) command requests that the device server transfers
// the medium defect data to the data-in buffer.
// In our case this will always be no defects.
//
static uint8_t scsiCommandReadDefectData10(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: Read Defect Data(10) command (0x37) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // Make sure the target LUN is started
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN)) {
      // LUN unavailable... return with error status
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("\r\nSCSI Commands: Unavailable LUN #"), commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad
      requestSenseData[commandDataBlock.targetLUN] = scsiUnitNotReady(); // not ready

      return SCSI_STATUS;
   }

	// return no defects

	// Set up the control signals ready for the data in phase
	scsiInformationTransferPhase(ITPHASE_DATAIN);

	// send back a 4 byte header
	hostadapterWriteByte(0);									// Reserved
	hostadapterWriteByte(commandDataBlock.data[2]);		// b7-b4 Reserved | b4 PLISTV | b3 GLISTV | b2-b0 Defect List Format
	hostadapterWriteByte(0);									// Defect List Length N-3 MSB
	hostadapterWriteByte(0);									// Defect List Length N-3 MSB

   // Indicate successful command in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   return SCSI_STATUS;
}

// SCSI Command (0x12) Inquiry
//
static uint8_t scsiCommandInquiry(void)
{
   if (debugFlag_scsiCommands) {
      debugString_C(PSTR("SCSI Commands: CommandInquiry (0x12) received\r\n"), DEBUG_SCSI_COMMAND);
//      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
//      debugStringInt16_P(PSTR("SCSI Commands: Length = "),commandDataBlock.data[4], true);
#ifdef DEBUG
		/* Debug builds only: the sprintf is real work even though the
		   debugString_C below compiles away, and debugFlag_scsiCommands is a
		   runtime flag the Beeb can flip - a release build must not pay a
		   100-byte format per INQUIRY for output that goes nowhere. */
		char msg[100];
		sprintf(msg, "SCSI Commands: Target LUN = %d  Length = %d\r\n", commandDataBlock.targetLUN, commandDataBlock.data[4]);
		debugString_C(msg, DEBUG_INFO);
#endif
   }
   size_t inqLen;
   const char * buf = filesystemGetInquiryData(commandDataBlock.targetLUN,&inqLen);
   if (buf) {
      // Set up the control signals ready for the data in phase
		scsiInformationTransferPhase(ITPHASE_DATAIN);

		// send back the inquiry data
		// The data the descriptor holds, cut to the allocation length: a drive
		// stops when its data ends (SCSI-1 7.1.3 "whichever is less"), no padding.
		for (size_t i = 0; (i < commandDataBlock.data[4]) && (i < inqLen); i++)
			hostadapterWriteByte((uint8_t)buf[i]);

		// Indicate successful command in status and message
		commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
	}
   else {
		if (debugFlag_scsiCommands) debugString_C(PSTR("SCSI Commands: scsiCommandInquiry: Error reading Inquiry data\r\n"), DEBUG_ERROR);

		// Indicate unsuccessful command in status and message
		commandDataBlock.status = SCSI_STATUS_CHECK_COND;      // 0x02 = Bad

		// Set request sense error globals: no descriptor loaded means the
		// LUN was never started - as TEST UNIT READY reports it
		requestSenseData[commandDataBlock.targetLUN] = scsiUnitNotReady(); // Unit not ready
	}

   return SCSI_STATUS;
}

// SCSI Command (0x1D) Send Diagnostic
//
// No need to implement hard drive diagnostics. Just return command completed successfully
static uint8_t scsiCommandSendDiagnostic(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: SEND DIAGNOSTIC command (0x1D) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Allocation Length = "), commandDataBlock.data[4], true);
   }

   // Set up the control signals ready for the data out phase
   scsiInformationTransferPhase(ITPHASE_DATAOUT);

   /* The count is two host-supplied bytes, so it reaches 65535.  With no
      bail-out that was up to 65535 x HD_ACK_TIMEOUT_US - about 1.8 hours with
      the poll loop dead - and BREAK did NOT shorten it, because hd_wait_ack()
      returns immediately on reset while the loop still grinds through every
      remaining iteration.  A corrupted CDB is enough to get here with a
      non-zero byte 3: hostadapterReadByte() returns whatever is on the bus
      when its ACK wait gives up.
      Bail on reset, as scsiCommandReassignBlocks does, so the per-byte
      timeout composes into a per-command bound. */
   int drainLength = (commandDataBlock.data[3] << 8) | commandDataBlock.data[4];
   for (int byteCounter = 0; byteCounter < drainLength; byteCounter++) {
      if (hostadapterReadResetFlag()) return SCSI_BUSFREE;
      hostadapterReadByte();
     }

   // Indicate successful command in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good

   return SCSI_STATUS;
}

// LV-DOS specific group 6 commands -----------------------------------------------------------------------------------

// SCSI Command Write F-Code (group 6 - command 0x0A)
//
// Notes from the VP415 manual:
// This command allows the initiator to write an F-code to a specific logic
// unit. A data out phase is required, in which the F-code command is
// sent and terminated by a 'CR' character and null padded until the end
// of the block.
static uint8_t scsiWriteFCode(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: WRITE F-Code command (G6 0x0A) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // The F-codes drive the video player, which only needs the VFS volume
   // (its video.pvf): a video-only disc has no scsi0.dat, so no startable
   // LUN, yet play/pause/goto must still work from the menu.
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN) &&
       !(commandDataBlock.targetLUN >= 8 && (filesystemVFSVolumePresent() || filesystemVFSDatPresent()))) {
      // LUN unavailable... return with error status
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("\r\nSCSI Commands: Unavailable LUN #"), commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals

      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      return SCSI_STATUS;
   }

   // Set up the control signals ready for the data out phase
   scsiInformationTransferPhase(ITPHASE_DATAOUT);

   // Transfer a single block from the file system to the host
   // Note: Since VFS is slower than ADFS we do not disable interrupts here as
   // disabling interrupts can cause incoming serial bytes to be lost
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Transferring F-Code buffer from the host...\r\n"));
   DEBUG_bytesTransferred(hostadapterPerformWriteDMA(scsiFcodeBuffer));

   // Check for a host reset condition
   if (hostadapterReadResetFlag()) {
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Write DMA interrupted by host reset at byte #"), (uint16_t)bytesTransferred, true);
      return SCSI_BUSFREE;
   }

   /* A short DMA means the host stopped ACKing part-way: the tail of
      scsiFcodeBuffer still holds the previous F-code, so running it would
      execute half the new command and half the old.  Bail as WRITE6 does. */
   if (bytesTransferred < 256) {
      return SCSI_BUSFREE;
   }

   // Write the requested F-Code to the Laser Video Disc Player
   fcodeWriteBuffer(commandDataBlock.targetLUN);

   // Indicate successful transfer in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Write F-Code command successful\r\n"));

   // Transition to the successful state
   return SCSI_STATUS;
}

// SCSI Command Read F-Code (group 6 - command 0x08)
//
// Notes from the VP415 manual:
// This command allows the initiator to read the reply code from the reply
// code buffer for the specified logic unit in LV-DOS. If there is a reply
// code from the player then the reply code is sent; it is terminated by a
// 'CR' character and null padded until the end of the block. If there is no
// reply code the first character of the block is a 'CR' character. A data in
// phase is necessary.
static uint8_t scsiReadFCode(void)
{
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: READ F-Code command (G6 0x08) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // The F-codes drive the video player, which only needs the VFS volume
   // (its video.pvf): a video-only disc has no scsi0.dat, so no startable
   // LUN, yet play/pause/goto must still work from the menu.
   if (!filesystemReadLunStatus(commandDataBlock.targetLUN) &&
       !(commandDataBlock.targetLUN >= 8 && (filesystemVFSVolumePresent() || filesystemVFSDatPresent()))) {
      // LUN unavailable... return with error status
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("\r\nSCSI Commands: Unavailable LUN #"), commandDataBlock.targetLUN, true);
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      return SCSI_STATUS;
   }

   // Set up the control signals ready for the data in phase
   scsiInformationTransferPhase(ITPHASE_DATAIN);

   // Ensure the buffer is ready for the host to read
   fcodeReadBuffer();

   // Send the data to the host
   // Note: Since VFS is slower than ADFS we do not disable interrupts here as
   // disabling interrupts can cause incoming serial bytes to be lost
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Transferring F-Code buffer to the host...\r\n"));
   DEBUG_bytesTransferred(hostadapterPerformReadDMA(scsiFcodeBufferRX));

   fcodeClearBuffer();

   // Check for a host reset condition
   if (hostadapterReadResetFlag()) {
      sei();
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Read DMA interrupted by host reset at byte #"), (uint16_t) bytesTransferred, true);

      return SCSI_BUSFREE;
   }

   // Indicate successful transfer in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Read F-Code command successful\r\n"));

   // Transition to the successful state
   return SCSI_STATUS;
}

// BeebSCSI specific group 6 commands -----------------------------------------------------------------------------------

// SCSI Command BeebSCSI Sense (group 6 - command 0x10)
//
// This is a BeebSCSI vendor specific command.
static uint8_t scsiBeebScsiSense(void)
{
   uint8_t lunStatus = 0;

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: BSSENSE command (G6 0x10) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   // This command does not use the LUN number

   // The parameter list must be 8 bytes
      debugStringInt16_P(PSTR("SCSI Commands: List length = "), commandDataBlock.data[4], true);
   }
   if (commandDataBlock.data[4] != 8) {
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Bad Argument error\r\n"));
      // Indicate unsuccessful command in status and message
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = BAD_ARG; // Bad argument

      return SCSI_STATUS;
   }

   // Set up the control signals ready for the data in phase
   scsiInformationTransferPhase(ITPHASE_DATAIN);

   // Transfer the BSSENSE status bytes
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Sending BSSENSE descriptor to host\r\n"));

   // Byte 0 shows the START/STOP status of each LUN (1 = started)
   if (filesystemReadLunStatus(0)) lunStatus |= (1 << 0);
   if (filesystemReadLunStatus(1)) lunStatus |= (1 << 1);
   if (filesystemReadLunStatus(2)) lunStatus |= (1 << 2);
   if (filesystemReadLunStatus(3)) lunStatus |= (1 << 3);
   if (filesystemReadLunStatus(4)) lunStatus |= (1 << 4);
   if (filesystemReadLunStatus(5)) lunStatus |= (1 << 5);
   if (filesystemReadLunStatus(6)) lunStatus |= (1 << 6);
   if (filesystemReadLunStatus(7)) lunStatus |= (1 << 7);
   hostadapterWriteByte(lunStatus);

   if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: LUN status byte = "), lunStatus, true);

   // Byte 1 shows the current LUN directory number
   hostadapterWriteByte(filesystemGetLunDirectory());

   if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: LUN directory number = "), filesystemGetLunDirectory(), true);

   // Byte 2 shows if we are in fixed (0) or VP415 emulation mode (1)
   if (scsiHostID != 16) {
      hostadapterWriteByte(0x00);
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Emulation mode fixed (0x00)\r\n"));
   } else {
      hostadapterWriteByte(0x01);
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Emulation mode LV-DOS (0x01)\r\n"));
   }

   // Bytes 3 and 4 show the major and minor firmware version numbers
   hostadapterWriteByte(FIRMWARE_MAJOR);
   hostadapterWriteByte(FIRMWARE_MINOR);

   // Bytes 5 to 7 are for future use
   hostadapterWriteByte(0x00);
   hostadapterWriteByte(0x00);
   hostadapterWriteByte(0x00);

   // Indicate successful command in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good

   return SCSI_STATUS;
}

/* The swap itself, with no started-LUN guard.  Two callers want two
   different policies and they must not be confused:

     - BSSELECT / *SCSIJUKE goes through scsiJukebox() below, which REFUSES
       while any LUN of the directory being swapped is started - the host
       is asking mid-session and an ADFS holding an open catalogue must not
       have the disc changed under it.
     - the *FX147,65,n poke (hd_juke_service) deliberately stops the swapped
       directory's LUNs first (filesystemStopDirectoryLuns) and then swaps
       unconditionally.  It used to call scsiJukebox(), but those LUNs are
       already stopped by then, so the guard could never fire and its bool
       return was dropped - a safety check that read as live and was not.
       It calls this directly now, so the difference is stated rather than
       accidental. */
void scsiJukeboxSwap (uint8_t lun) {
   uint8_t old_vfs = filesystemGetLunDirectoryVFS();
   filesystemSetLunDirectory(scsiHostID, lun);
   // The video lives in the VFS jukebox directory: notify only when THAT
   // directory changed - an ADFS *SCSIJUKE (internal host) or reselecting
   // the same directory must not rewind a playing video to picture 1.
   if (filesystemGetLunDirectoryVFS() != old_vfs)
      videoplayer_media_changed();
   if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Jukeboxing successful - LUN directory set to "), lun, true);
}

bool scsiJukebox (uint8_t lun) {
   // Check only the LUNs of the directory being swapped: an ADFS host
   // (ID < 16) swaps /BeebSCSI<n>, which holds LUNs 0-7, and the VFS host
   // swaps /BeebVFS<n>, LUNs 8-15 (filesystemSetLunDirectory splits the
   // same way).  Guarding all 16 refused an ADFS *SCSIJUKE while the
   // LaserDisc was mounted, and a disc flip while an ADFS LUN was.
   // "< 16" is filesystemSetLunDirectory's test; scsiTransformLUNid adds 8
   // only for ID == 16, the one VFS host ID in use, so the two agree.
   uint8_t firstLun = (scsiHostID < 16) ? 0 : 8;
   for (uint8_t lunNumber = firstLun; lunNumber < firstLun + 8; lunNumber++)
      if (filesystemReadLunStatus(lunNumber)) return false;

   // Only jukebox if none of those LUNs is in the started state
   scsiJukeboxSwap(lun);
   return true;
}

// SCSI Command BeebSCSI Select (group 6 - command 0x11)
//
// This is a BeebSCSI vendor specific command.
static uint8_t scsiBeebScsiSelect(void)
{
   uint8_t byteCounter;
   uint8_t Buffer[8];

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: BSSELECT command (G6 0x11) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // Expect 8 bytes of data
   if (commandDataBlock.data[4] != 8) {
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Bad Argument error\r\n"));
      // Indicate unsuccessful command in status and message
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = BAD_ARG; // Bad argument

      return SCSI_STATUS;
   }

   // Set up the control signals ready for the data out phase
   scsiInformationTransferPhase(ITPHASE_DATAOUT);

   // Read the 8 bytes from the host
   for (byteCounter = 0; byteCounter < commandDataBlock.data[4]; byteCounter++) {
      Buffer[byteCounter] = hostadapterReadByte();
      if (debugFlag_scsiCommands) {
         debugStringInt16_P(PSTR("SCSI Commands: Received byte "), (uint16_t)byteCounter, false);
         debugStringInt16_P(PSTR(" = "), (uint16_t)Buffer[byteCounter], true);
        }
     }

   // A host reset or reselection mid-transfer leaves stale bus bytes in
   // Buffer: abandon the command rather than jukebox to them
   if (hostadapterReadResetFlag()) return SCSI_BUSFREE;

   // Only jukebox if none of that directory's LUNs is started
   if (!scsiJukebox(Buffer[0])) {
      // One or more of its LUNs are started... cannot perform jukeboxing
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Error - cannot jukebox if LUNs are started\r\n"));
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = scsiUnitNotReady(); // Unit not ready

      return SCSI_STATUS;
   }

   // Indicate successful command in status and message
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: BSSELECT command successful\r\n"));
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good

   return SCSI_STATUS;
}


// Vendor-specific FAT file manipulation functions --------------

// SCSI Command BeebSCSI FATPATH (group 6 - command 0x12)
//
// This is a BeebSCSI vendor specific command.
//
static uint8_t scsiBeebScsiFatPath(void)
{
   uint8_t Buffer[256];
   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: BSFATPATH command (G6 0x12) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // This command does not use the LUN number

   // Set up the control signals ready for the data out phase
   scsiInformationTransferPhase(ITPHASE_DATAOUT);

   // Transfer a single block (the path) from the host
   // Note: cli()/sei() are empty on the Pi (cpuspecific.h) - the bracket is
   // the AVR original's, and disables nothing here
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Transferring FAT path buffer from the host...\r\n"));
   cli();
   DEBUG_bytesTransferred(hostadapterPerformWriteDMA(Buffer));
   sei();

   // Check for a host reset condition
   if (hostadapterReadResetFlag()) {
      sei();
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Write DMA interrupted by host reset at byte #"), (uint16_t)bytesTransferred, true);
      return SCSI_BUSFREE;
   }

   // Ensure the host-supplied path string is terminated
   Buffer[255] = 0;

   // Change the filesystem's FAT transfer directory
   filesystemSetFatDirectory(Buffer);

   // Show debug
   if (debugFlag_scsiBlocks) {
      debugString_P(PSTR("Hex dump for FAT path block:\r\n"));
      debugSectorBufferHex(Buffer, 256);
   }

   // Indicate successful transfer in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: BSFATPATH command successful\r\n"));

   // Transition to the successful state
   return SCSI_STATUS;
}

// SCSI Command BeebSCSI FATINFO (group 6 - command 0x13)
//
// This is a BeebSCSI vendor specific command.
//
// This function accepts a FAT file identification number (in the 5th field of the command)
// and returns a 256 byte buffer containing information about the FAT file.
//
// The buffer format is as follows:
// Byte 0: Status of file (0 = does not exist, 1 = file exists, 2 = directory)
// Byte 1 - 4: Size of file in number of bytes (32-bit)
// Byte 5 - 126: Reserved (0)
// Byte 127- 255: File name string terminated with 0x00 (NULL)
static uint8_t scsiBeebScsiFatInfo(void)
{
   uint32_t fatFileId = 0;
   uint8_t Buffer[256] = {0}; // zeroed so unused bytes don't leak stack contents to the host

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: BSFATINFO command (G6 0x13) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // Get the requested FAT file ID
   fatFileId = (uint32_t)commandDataBlock.data[5];

   // Show the command debug information
   if (debugFlag_scsiCommands) debugStringInt32_P(PSTR("SCSI Commands: FAT file ID = "), fatFileId, true);

   // Set up the control signals ready for the data in phase
   scsiInformationTransferPhase(ITPHASE_DATAIN);

   // Get the FAT file information into the buffer for the host to read
   if(!filesystemGetFatFileInfo(fatFileId, Buffer)) {
      // Reading from the FAT image failed... try to recover with a little grace...
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: ERROR: Could not read FAT file information!\r\n"));
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      return SCSI_STATUS;
   }

   // Send the data to the host
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Transferring FAT info buffer to the host...\r\n"));
   cli();
   DEBUG_bytesTransferred(hostadapterPerformReadDMA(Buffer));
   sei();

   // Check for a host reset condition
   if (hostadapterReadResetFlag()) {
      sei();
      if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Read DMA interrupted by host reset at byte #"),(uint16_t) bytesTransferred, true);

      return SCSI_BUSFREE;
   }

   // Show debug
   if (debugFlag_scsiBlocks) {
      debugString_P(PSTR("Hex dump for FAT info block:\r\n"));
      debugSectorBufferHex(Buffer, 256);
   }

   // Indicate successful transfer in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: BSFATINFO command successful\r\n"));

   // Transition to the successful state
   return SCSI_STATUS;
}

// SCSI Command BeebSCSI FATREAD (group 6 - command 0x14)
//
// This is a BeebSCSI vendor specific command.
//
// This function accepts a FAT file identification number (in the 5th field of the command),
// a block offset (the block to start reading from) and a number of blocks.
//
// The requested number of blocks from the FAT file are transferred to the host
//
static uint8_t scsiBeebScsiFatRead(void)
{
   uint32_t blockOffset = 0;
   uint32_t numberOfBlocks = 0;
   uint32_t fatFileId = 0;
   uint8_t Buffer[256];

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: BSFATREAD command (G6 0x14) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
   }

   // This command does not use the LUN number

   // Get block offset
   blockOffset = (((uint32_t)commandDataBlock.data[1] & 0x1F) << 16) |
   ((uint32_t)commandDataBlock.data[2] << 8) |
   ((uint32_t)commandDataBlock.data[3]);

   // Get the requested FAT file ID
   fatFileId = (uint32_t)commandDataBlock.data[5];

   // Get the number of blocks requested (1-255)
   numberOfBlocks = (uint32_t)commandDataBlock.data[4];

   // Show the command debug information
   if (debugFlag_scsiCommands) {
      debugStringInt32_P(PSTR("SCSI Commands: FAT file ID = "), fatFileId, true);
      debugStringInt32_P(PSTR("SCSI Commands: Block offset = "), blockOffset, true);
      debugStringInt32_P(PSTR("SCSI Commands: Number of requested blocks = "), numberOfBlocks, true);
   }
   // Set up the control signals ready for the data in phase
   scsiInformationTransferPhase(ITPHASE_DATAIN);

   // Transfer the requested blocks from the FAT file to the host
   if(!filesystemOpenFatForRead(fatFileId, blockOffset)) {
      // Reading from the FAT image failed... try to recover with a little grace...
      if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: ERROR: Could not read next block from FAT image!\r\n"));
      commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

      // Set request sense error globals
      requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

      return SCSI_STATUS;
   }

   for (uint32_t currentBlock = 0; currentBlock < numberOfBlocks; currentBlock++) {
       if(!filesystemReadNextFatBlock(Buffer)) {
         // Could not read block from FAT file system!
         sei();
         if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: Failed to read new FAT block"));
         filesystemCloseFatForRead();

         /* Report it as READ6 does.  BUS FREE here, mid data-in, left the
            host waiting on REQ for a byte that never came. */
         commandDataBlock.status = SCSI_STATUS_CHECK_COND; // 0x02 = Bad

         // Set request sense error globals
         requestSenseData[commandDataBlock.targetLUN] = DRIVE_NOT_READY; // Drive not ready

         return SCSI_STATUS;
      }
      // Send the data to the host
      cli();
      DEBUG_bytesTransferred(hostadapterPerformReadDMA(Buffer));
      sei();

      // Check for a host reset condition
      if (hostadapterReadResetFlag()) {
         sei();
         if (debugFlag_scsiCommands) debugStringInt16_P(PSTR("SCSI Commands: Read DMA interrupted by host reset at byte #"), (uint16_t)bytesTransferred, true);

         filesystemCloseFatForRead();
         return SCSI_BUSFREE;
      }

      /* A short DMA means hd_wait_ack() timed out: the host stopped
         servicing REQ without asserting nRST.  The return value used to be
         discarded (the release arm of DEBUG_bytesTransferred only cast it
         to void),
         so the loop simply started the next sector - and a 256-block
         transfer then burned HD_ACK_TIMEOUT_US per sector, 25 s with the
         whole cooperative loop stopped: no 1MHz bus service, no WiFi/AUN,
         no USB.  Bail so the per-sector timeout composes into a
         per-command bound, exactly as scsiCommandReassignBlocks does. */
      if (bytesTransferred < 256) {
         filesystemCloseFatForRead();
         return SCSI_BUSFREE;
      }

      // Show debug
     if (debugFlag_scsiBlocks) {
         debugStringInt32_P(PSTR("Hex dump for block #"), currentBlock, true);
         debugSectorBufferHex(Buffer, 256);
     } else {
        if (debugFlag_scsiCommands) {
           debugStringInt32_P(PSTR(""), currentBlock, false);
           debugString_P(PSTR(" "));
        }
      }
   }
   filesystemCloseFatForRead();
   if (debugFlag_scsiCommands || debugFlag_scsiBlocks) debugString_P(PSTR("\r\n"));

   // Indicate successful transfer in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good
   if (debugFlag_scsiCommands) debugString_P(PSTR("SCSI Commands: BSFATREAD command successful\r\n"));

   // Transition to the successful state
   return SCSI_STATUS;
}


// SCSI Command Certify (group 7 - command 0xE2)
//
// This is a RODIME vendor specific command: the drive's own surface test, run
// by its firmware on the platters.  A 6 byte cmd block is passed,
// &E2, LUN , 0 , cycles , 0 , 0
// Some drives implement it although their INQUIRY command list leaves it
// out; such a drive ignores a non-zero byte 2, and cycles 0 still runs
// (measured on a real drive).  An emulated disc has no surface and no defects, so it is
// answered GOOD at once and the image is not touched.  FServFmt only sizes
// its timeout from it: moretime%=cycles*60*18*cyl%*heads%*sects_trk%/&3D850
static uint8_t scsiCommandCertify(void)
{

   if (debugFlag_scsiCommands) {
      debugString_P(PSTR("SCSI Commands: RODIME Certify command (G7 0xE2) received\r\n"));
      debugStringInt16_P(PSTR("SCSI Commands: Target LUN = "), commandDataBlock.targetLUN, true);
      debugStringInt16_P(PSTR("SCSI Commands: Number of cycles = "), commandDataBlock.data[3], true);
   }

   // Indicate successful command in status and message
   commandDataBlock.status = SCSI_STATUS_OK; // 0x00 = Good

   return SCSI_STATUS;
}


// The BBC and Master ADFS appear to use:
// Drive 0 : Host Identifier = 1 LUN = 0
// Drive 1 : Host Identifier = 1 LUN =32
// Drive 2 : Host Identifier = 1 LUN =64
// Drive 3 : Host Identifier = 1 LUN =96
//
// Other systems use
// Drive 0 : Host Identifier = 1 LUN =0
// Drive 1 : Host Identifier = 2 LUN =0
// Drive 2 : Host Identifier = 4 LUN =0
// Drive 3 : Host Identifier = 8 LUN =0
//
// BeebSCSI works on the numbering system for
// the BBC and Master ADFS
//
// VFS 2.00 uses host ID 16

static uint32_t scsiTransformLUNid(uint8_t LUN)
{
	// ensure the lower bits b0-b4 are kept as they contain other information

	// Host Identifier holds the drive selected 1, 2, 4, 8 (0, 1, 2, 3)
	switch (scsiHostID) {
	case 1:			// Drive 0 is selected
		break;
	case 2:			// Drive 1 is selected
		return (32 | (LUN & 0x1F));	// Set LUN 1
		break;
	case 4:			// Drive 2 is selected
		return (64 | (LUN & 0x1F));	// Set LUN 2
		break;
	case 8:			// Drive 3 is selected
		return (96 | (LUN & 0x1F));  // Set LUN 3
		break;
   case 16:	      // VFS host id
      return (LUN | 256);          // Set LUN = LUN + 8
         break;
	default:
		break;
	}

	return (LUN);
}
