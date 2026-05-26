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
#include "ext_attributes.h"
#include "fatfs/ff.h"
#include "filesystem.h"
#include "../rpi/rpi.h"

#define SZ_TBL 64

// Minimum geometry details needed to operate the LUN
// This saves looking them up again in the future
struct HDGeometry
{
	uint32_t BlockSize;				// This will almost always be 256
	uint32_t Cylinders;
	uint8_t  Heads;
	uint16_t SectorsPerTrack;		// default is 33
	uint16_t Interleave;				// doesn't really matter for BeebSCSI
};

// File system state structure
NOINIT_SECTION static struct filesystemStateStruct
{
   FATFS fsObject;         // FAT FS file system object
   FIL fileObject[MAX_LUNS];         // FAT FS file objects
   DWORD clmt[MAX_LUNS][SZ_TBL];

   bool fsMountState;      // File system mount state (true = mounted, false = dismounted)

   uint8_t lunDirectory;               // Current LUN directory ID
   uint8_t lunDirectoryVFS;   // Current LUN directory ID for VFS
   bool fsLunStatus[MAX_LUNS];         // LUN image availability flags for the currently selected LUN directory (true = started, false = stopped)
   uint8_t fsLunUserCode[MAX_LUNS][5];  // LUN 5-byte User code (used for F-Code interactions - only present for laser disc images)
	bool fsLunHasExtendedAttributes[MAX_LUNS];	// flag to show if extended attributes are available
	struct HDGeometry fsLunGeometry[MAX_LUNS];   // Keep the geometry details for each LUN
} filesystemState;

NOINIT_SECTION static char fileName[255];       // String for storing LFN filename
NOINIT_SECTION static char fatDirectory[255];      // String for storing FAT directory (for FAT transfer operations)

NOINIT_SECTION static uint8_t sectorBuffer[SECTOR_BUFFER_SIZE];   // Buffer for reading sectors

// Globals for multi-sector reading
static uint8_t sectorsInBuffer = 0;
static uint8_t currentBufferSector = 0;
static uint32_t sectorsRemaining = 0;

NOINIT_SECTION static FIL fileObjectFAT;

static bool filesystemMount(void);
static bool filesystemDismount(void);
static bool filesystemCheckLunDirectory(uint8_t lunDirectory, uint8_t lunNumber);

//static bool filesystemCreateDscFromLunImage(uint8_t lunDirectory, uint8_t lunNumber, uint32_t lunFileSize);

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
      debugString_P(PSTR("FR_NO_FILE\\r\n"));
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
void filesystemInitialise(uint8_t scsijuke, uint8_t vfsjuke)
{
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemInitialise(): Initialising file system\r\n"));
   filesystemState.lunDirectory = scsijuke;      // Default to LUN directory 0
   filesystemState.lunDirectoryVFS = vfsjuke;      // Default to LUN directory 0
   filesystemState.fsMountState = false;  // FS default state is unmounted
}

// Reset the file system (called when the host signals reset)
void filesystemReset(void)
{
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReset(): Resetting file system\r\n"));

   // Reset the default FAT transfer directory
   sprintf(fatDirectory, "/Transfer");

   // Set all LUNs to stopped
   for( uint8_t i=0 ; i < MAX_LUNS; i++ )
      filesystemState.fsLunStatus[i] = false;

   // ensure the file-system is closed on reset
   filesystemDismount();
   // Now Mount the filesystem
   filesystemMount();
}

// File system mount and dismount functions --------------------------------------------------------------------

// Function to mount the file system
static bool filesystemMount(void)
{
   FRESULT fsResult;
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

   // Note: ADFS does not send a SCSI STARTSTOP command on reboot... it assumes that LUN 0 is already started.
   // This is theoretically incorrect... the host should not assume anything about the state of a SCSI LUN.
   // However, in order to support this buggy implementation we have to start LUN 0 here.
  // filesystemSetLunStatus(0, true);

   return true;
}

// Function to dismount the file system
static bool filesystemDismount(void)
{

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemDismount(): Dismounting file system\r\n"));
/*
   // Is the file system mounted?
   if (filesystemState.fsMountState == false) {
      // Nothing to do...
      debugString_P(PSTR("File system: filesystemDismount(): No file system to dismount\r\n"));
      return false;
   }
*/
   // Set all LUNs to stopped
   for( uint8_t i=0 ; i < MAX_LUNS; i++ )
      filesystemSetLunStatus(i, false);

/*   // Dismount the SD card
     FRESULT fsResult;
 //  fsResult = f_mount(&filesystemState.fsObject, "", 0);

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
*/
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemDismount(): Successful\r\n"));
   filesystemState.fsMountState = false;
   return true;
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
   userCode[0] = filesystemState.fsLunUserCode[lunNumber][0];
   userCode[1] = filesystemState.fsLunUserCode[lunNumber][1];
   userCode[2] = filesystemState.fsLunUserCode[lunNumber][2];
   userCode[3] = filesystemState.fsLunUserCode[lunNumber][3];
   userCode[4] = filesystemState.fsLunUserCode[lunNumber][4];

   userCode[0] = 0x31; // fixed code for now *** TODO ****** ( 1=066) ( 1=986)
   userCode[1] = 0x3D;
   userCode[2] = 0x30;
   userCode[3] = 0x36;
   userCode[4] = 0x36;
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
   if (lunNumber < 8 )
   {
      sprintf(fileName, "/BeebSCSI%d", lunDirectory);
   }
   else
   {
      sprintf(fileName, "/BeebVFS%d", lunDirectory);
   }

   fsResult = f_opendir(&dirObject, fileName);

   // Did a directory exist?
   if (fsResult == FR_NO_PATH) {

      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): f_opendir returned FR_NO_PATH - Directory does not exist\r\n"));
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
         return false;
      }
   }
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunDirectory(): LUN directory found\r\n"));

   return true;
}

// Function to scan for SCSI LUN image file on the mounted file system
// and check the image is valid.
bool filesystemCheckLunImage(uint8_t lunNumber)
{
   uint32_t lunFileSize;
   FRESULT fsResult;
   if (filesystemState.fsLunStatus[lunNumber])
   {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): Lun already open\r\n"));
      return true;
   }

   // Attempt to open the LUN image
   if (lunNumber < 8 )
   {
      sprintf(fileName, "/BeebSCSI%d/scsi%d.dat", filesystemState.lunDirectory, lunNumber);
   }
   else
   {
      sprintf(fileName, "/BeebVFS%d/scsi%d.dat", filesystemState.lunDirectoryVFS, lunNumber & 7);
   }

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
   filesystemState.clmt[lunNumber][0] = SZ_TBL;
   ((FIL*)(&filesystemState.fileObject[lunNumber]))->cltbl = filesystemState.clmt[lunNumber];
   if (f_lseek(&filesystemState.fileObject[lunNumber], CREATE_LINKMAP) != FR_OK ){
       // if f_lseek fails then file is very fragmented.
       // fall back to slow seek.
       ((FIL*)(&filesystemState.fileObject[lunNumber]))->cltbl = 0;
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

   FIL fileObject;
   // Check if the LUN descriptor file (.dsc) is present
   if (lunNumber < 8 )
   {
      sprintf(fileName, "/BeebSCSI%d/scsi%d.dsc", filesystemState.lunDirectory, lunNumber);
   }
   else
   {
      // this isn't expected to exist
      sprintf(fileName, "/BeebVFS%d/scsi%d.dsc", filesystemState.lunDirectoryVFS, lunNumber & 7);
   }

   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemCheckLunImage(): Checking for (.dsc) LUN descriptor "), (uint16_t)lunNumber, 1);
   fsResult = f_open(&fileObject, fileName, FA_READ);

   if (fsResult != FR_OK) {
      // LUN descriptor file is not found
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): LUN descriptor not found\r\n"));
#if 0
   // don't create a dsc file as it is not needed
      // Automatically create a LUN descriptor file for the LUN image
      if (filesystemCreateDscFromLunImage(filesystemState.lunDirectory, lunNumber, lunFileSize)) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): Automatically created .dsc for LUN image\r\n"));
      } else {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): ERROR: Automatically creating .dsc for LUN image failed\r\n"));
      }
#endif
   } else {
      // LUN descriptor file is present
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): LUN descriptor found\r\n"));
      f_close(&fileObject);

      // Calculate the LUN size from the descriptor file
      uint32_t lunDscSize = filesystemGetLunSizeFromDsc(lunNumber);
      if (debugFlag_filesystem) debugStringInt32_P(PSTR("File system: filesystemCheckLunImage(): LUN size in bytes (according to .dsc) = "), lunDscSize, 1);

      // Are the file size and DSC size consistent?
      if (lunDscSize != lunFileSize) {
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): WARNING: File size and DSC parameters are NOT consistent\r\n"));
      }
   }
   //  .ucd files have been superseded
#if 0
   // this isn't expected to exist
   // Check if the LUN user code descriptor file (.ucd) is present
   if (lunNumber < 8 )
   {

      sprintf(fileName, "/BeebSCSI%d/scsi%d.ucd", filesystemState.lunDirectory, lunNumber);
   }
   else
   {
      // this isn't expected to exist
      sprintf(fileName, "/BeebVFS%d/scsi%d.ucd", filesystemState.lunDirectoryVFS, lunNumber & 7);
   }

   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): Checking for (.ucd) LUN user code descriptor\r\n"));
   fsResult = f_open(&fileObject, fileName, FA_READ);

   if (fsResult != FR_OK) {
      // LUN descriptor file is not found
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): LUN user code descriptor not found\r\n"));

      // Set the user code descriptor to the default (probably not a laser disc image)
      filesystemState.fsLunUserCode[lunNumber][0] = 0x00;
      filesystemState.fsLunUserCode[lunNumber][1] = 0x00;
      filesystemState.fsLunUserCode[lunNumber][2] = 0x00;
      filesystemState.fsLunUserCode[lunNumber][3] = 0x00;
      filesystemState.fsLunUserCode[lunNumber][4] = 0x00;
   } else {
      // LUN user code descriptor file is present
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): LUN user code descriptor found\r\n"));

      // Close the .ucd file
      f_close(&fileObject);

      // Read the user code from the .ucd file
      filesystemGetUserCodeFromUcd(filesystemState.lunDirectory, lunNumber);
   }
#endif
	filesystemCheckExtAttributes(lunNumber);

	// Calculate the LUN size from the extended attributes file
	uint32_t lunDscSize = filesystemGetLunTotalBytes(lunNumber);
	if (debugFlag_filesystem) debugStringInt32_P(PSTR("File system: filesystemCheckLunImage(): LUN size in bytes (according to .ext) = "), lunDscSize, 1);

	// Are the file size and DSC size consistent?
	if (lunDscSize != lunFileSize) {
		if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): WARNING: File size and EXT parameters are NOT consistent\r\n"));
	}

   // Exit with success
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckLunImage(): Successful\r\n"));
   return true;
}

// Function to calculate the LUN image size from the LUN descriptor file parameters
uint32_t filesystemGetLunSizeFromDsc( uint8_t lunNumber)
{
   uint32_t lunSize = 0;
   UINT fsCounter;
   FRESULT fsResult;
   FIL fileObject;

	// pointer to the geometry data of the currently selected LUN
	struct HDGeometry* ptr = &filesystemState.fsLunGeometry[lunNumber];

   // Assemble the DSC file name
   if (lunNumber < 8 )
   {
      sprintf(fileName, "/BeebSCSI%d/scsi%d.dsc", filesystemState.lunDirectory, lunNumber);
   }
   else
   {
      // this isn't expected to exist
      sprintf(fileName, "/BeebVFS%d/scsi%d.dsc", filesystemState.lunDirectoryVFS, lunNumber & 7);
   }

   fsResult = f_open(&fileObject, fileName, FA_READ);
   if (fsResult == FR_OK) {
      uint8_t Buffer[22];
      // Read the DSC data
      fsResult = f_read(&fileObject, Buffer, 22, &fsCounter);

      // Check that the file was read OK and is the correct length
      if (fsResult != FR_OK  || fsCounter != 22) {
         // Something went wrong
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemGetLunSizeFromDsc(): ERROR: Could not read .dsc file\r\n"));
         f_close(&fileObject);
         return 0;
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
      lunSize = filesystemGetLunTotalBytes(lunNumber);

      f_close(&fileObject);
   }

   return lunSize;
}

// Function to return the LUN image Block size in bytes from the stored geometry
//
uint32_t filesystemGetLunBlockSize( uint8_t lunNumber)
{
	return filesystemState.fsLunGeometry[lunNumber].BlockSize;
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

	// Tracks = Cylinders * Heads
	// Total Sectors = Tracks * Sectors per Track
	return (((*ptr).Cylinders * (*ptr).Heads) * (*ptr).SectorsPerTrack);
}

// Function to return the cylinders and heads from the LUN descriptor file parameters
// into the buffer
//
void filesystemGetCylHeads( uint8_t lunNumber, uint8_t *returnbuf)
{
	returnbuf[3] = ((uint8_t)((filesystemState.fsLunGeometry[lunNumber].Cylinders & 0x0000FF00) >> 8));
	returnbuf[4] = ((uint8_t) (filesystemState.fsLunGeometry[lunNumber].Cylinders & 0x000000FF));
	returnbuf[5] = filesystemState.fsLunGeometry[lunNumber].Heads;
}

#if 0
// Function to automatically create a DSC file based on the file size of the LUN image
// Note, this function is specific to the BBC Micro and the ACB-4000 host adapter card
// If the DSC is inaccurate then, for the BBC Micro, it's not that important, since the
// host only looks at its own file system data (Superform and other formatters use the DSC
// information though... so beware).
static bool filesystemCreateDscFromLunImage(uint8_t lunDirectory, uint8_t lunNumber, uint32_t lunFileSize)
{
   uint32_t cylinders;
   uint32_t heads;
   UINT fsCounter;
   FRESULT fsResult;
   uint8_t Buffer[22];
   FIL fileObject;

   // Calculate the LUN file size in tracks (33 sectors per track, 256 bytes per sector)

   // Check that the LUN file size is actually a size which ADFS can support (the number of sectors is limited to a 21 bit number)
   // i.e. a maximum of 0x1FFFFF or 2,097,151 (* 256 - DEFAULT_BLOCK_SIZE - bytes per sector = 512Mb = 536,870,656 bytes)
   if (lunFileSize > ((1<<21)*DEFAULT_BLOCK_SIZE) ) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateDscFromLunImage(): WARNING: The LUN file size is greater than 512MBytes\r\n"));
   }

   // Check that the LUN file size is actually a size which the ACB-4000 card could have supported (given that the
   // block and track sizes were fixed to 256 and a default of 33 respectively)
   if (lunFileSize % (DEFAULT_BLOCK_SIZE * DEFAULT_SECTORS_PER_TRACK) != 0) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateDscFromLunImage(): WARNING: The LUN file size could not be supported by an ACB-4000 card. Not divisible by 256.\r\n"));
   }
   lunFileSize = lunFileSize / (DEFAULT_BLOCK_SIZE * DEFAULT_SECTORS_PER_TRACK);

   // The lunFileSize (in tracks) should be evenly divisible by the head count and the head count should be
   // 16 or less.
   heads = 16;
   while ((lunFileSize % heads != 0) && heads > 1) heads--;
   cylinders = lunFileSize / heads;

   if (debugFlag_filesystem) {
      debugStringInt32_P(PSTR("File system: filesystemCreateDscFromLunImage(): Number of heads = "), heads, true);
      debugStringInt32_P(PSTR("File system: filesystemCreateDscFromLunImage(): Number of cylinders = "), cylinders, true);
      debugStringInt32_P(PSTR("File system: filesystemCreateDscFromLunImage(): Number of sectors per track = "), DEFAULT_SECTORS_PER_TRACK, true);
      debugStringInt32_P(PSTR("File system: filesystemCreateDscFromLunImage(): LUN size in tracks = "), heads*cylinders, true);
      debugStringInt32_P(PSTR("File system: filesystemCreateDscFromLunImage(): LUN size in sectors = "), heads*cylinders*DEFAULT_SECTORS_PER_TRACK, true);
      debugStringInt32_P(PSTR("File system: filesystemCreateDscFromLunImage(): LUN size in bytes (total sectors * 256 bytes) = "), heads*cylinders*DEFAULT_SECTORS_PER_TRACK*DEFAULT_BLOCK_SIZE, true);
   }
   // The first 4 bytes are the Mode Select Parameter List (ACB-4000 manual figure 5-18)
   Buffer[ 0] = 0;      // Reserved (0)
   Buffer[ 1] = 0;      // Reserved (0)
   Buffer[ 2] = 0;      // Reserved (0)
   Buffer[ 3] = 8;      // Length of Extent Descriptor List (8)

   // The next 8 bytes are the Extent Descriptor list (there can only be one of these
   // and it's always 8 bytes) (ACB-4000 manual figure 5-19)
   Buffer[ 4] = 0;      // Density code
   Buffer[ 5] = 0;      // Reserved (0)
   Buffer[ 6] = 0;      // Reserved (0)
   Buffer[ 7] = 0;      // Reserved (0)
   Buffer[ 8] = 0;      // Reserved (0)
   Buffer[ 9] = 0;      // Block size MSB
   Buffer[10] = 1;      // Block size
   Buffer[11] = 0;      // Block size LSB = 256

   // The next 12 bytes are the Drive Parameter List (ACB-4000 manual figure 5-20)
   Buffer[12] = 1;      // List format code
   Buffer[13] = (uint8_t)((cylinders & 0x0000FF00) >> 8); // Cylinder count MSB
   Buffer[14] = (uint8_t)( cylinders & 0x000000FF); // Cylinder count LSB
   Buffer[15] = (uint8_t)(heads & 0x000000FF); // Data head count
   Buffer[16] = 0;      // Reduced write current cylinder MSB
   Buffer[17] = 128;    // Reduced write current cylinder LSB = 128
   Buffer[18] = 0;      // Write pre-compensation cylinder MSB
   Buffer[19] = 128;    // Write pre-compensation cylinder LSB = 128
   Buffer[20] = 0;      // Landing zone position
   Buffer[21] = 1;      // Step pulse output rate code

   // Assemble the DSC file name
   sprintf(fileName, "/BeebSCSI%d/scsi%d.dsc", lunDirectory, lunNumber);

   fsResult = f_open(&fileObject, fileName, FA_CREATE_NEW | FA_WRITE);
   if (fsResult == FR_OK) {
      // Write the DSC data
      fsResult = f_write(&fileObject, Buffer, 22, &fsCounter);
      if (fsResult != FR_OK ) {
          debugString_P(PSTR("File system: filesystemCreateDscFromLunImage(): ERROR: f_write on LUN .dsc returned "));
          filesystemPrintfserror(fsResult);
          return false;
      }

      if (fsCounter != 22) {
         debugString_P(PSTR("File system: filesystemCreateDscFromLunImage(): ERROR: .dsc create failed\r\n"));
         f_close(&fileObject);
         return false;
      }
   } else {
      // Something went wrong
      if (debugFlag_filesystem) {
         debugString_P(PSTR("File system: filesystemCreateDscFromLunImage(): ERROR: f_open on LUN .dsc returned "));
         filesystemPrintfserror(fsResult);
      }

      return false;
   }

   // Descriptor write OK
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateDscFromLunImage(): .dsc file created\r\n"));
   f_close(&fileObject);

   return true;
}

// Function to read the user code data from the LUN user code descriptor file (.ucd)
void filesystemGetUserCodeFromUcd(uint8_t lunDirectoryNumber, uint8_t lunNumber)
{
   UINT fsCounter;
   FRESULT fsResult;
   FIL fileObject;

   // Assemble the UCD file name
   sprintf(fileName, "/BeebSCSI%d/scsi%d.ucd", lunDirectoryNumber, lunNumber);

   fsResult = f_open(&fileObject, fileName, FA_READ);
   if (fsResult == FR_OK) {
      // Read the UCD data
      fsResult = f_read(&fileObject, filesystemState.fsLunUserCode[lunNumber], 5, &fsCounter);
      f_close(&fileObject);

      // Check that the file was read OK and is the correct length
      if (fsResult != FR_OK  || fsCounter != 5) {
         // Something went wrong
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemGetUserCodeFromUcd(): ERROR: Could not read .ucd file\r\n"));
         return;
      }

      if (debugFlag_filesystem) {
         debugStringInt16_P(PSTR("File system: filesystemGetUserCodeFromUcd(): User code bytes (from .ucd): "), (uint16_t)filesystemState.fsLunUserCode[lunNumber][0], false);
         debugStringInt16_P(PSTR(", "), (uint16_t)filesystemState.fsLunUserCode[lunNumber][1], false);
         debugStringInt16_P(PSTR(", "), (uint16_t)filesystemState.fsLunUserCode[lunNumber][2], false);
         debugStringInt16_P(PSTR(", "), (uint16_t)filesystemState.fsLunUserCode[lunNumber][3], false);
         debugStringInt16_P(PSTR(", "), (uint16_t)filesystemState.fsLunUserCode[lunNumber][4], true);
      }
   }
}
#endif
// Function to set the current LUN directory (for the LUN jukeboxing functionality)
void filesystemSetLunDirectory(uint8_t scsiHostID, uint8_t lunDirectoryNumber)
{
   // Change the current LUN directory number
   if (scsiHostID == 0)
      filesystemState.lunDirectory = lunDirectoryNumber;
   else
      filesystemState.lunDirectoryVFS = lunDirectoryNumber;
}

// Function to read the current LUN directory (for the LUN jukeboxing functionality)
uint8_t filesystemGetLunDirectory(void)
{
   return filesystemState.lunDirectory;
}

// Functions for creating LUNs and LUN descriptors ---------------------------------------------------------------------------

// Function to create a new LUN image (makes an empty .dat file)
bool filesystemCreateLunImage(uint8_t lunNumber)
{
   FRESULT fsResult;
   FIL fileObject;

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
   sprintf(fileName, "/BeebSCSI%d/scsi%d.dat", filesystemState.lunDirectory, lunNumber);

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

// Function to create a new LUN descriptor (makes an empty .dsc file)
bool filesystemCreateLunDescriptor(uint8_t lunNumber)
{
   FRESULT fsResult;
   FIL fileObject;

   if (lunNumber >7)
   {
      // VFS doesn't support creating .dsc files
      return false;
   }

   // Assemble the .dsc file name
   sprintf(fileName, "/BeebSCSI%d/scsi%d.dsc", filesystemState.lunDirectory, lunNumber);

   fsResult = f_open(&fileObject, fileName, FA_READ);
   if (fsResult == FR_OK) {
      // File opened ok - which means it already exists...
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunDescriptor(): .dsc already exists - ignoring request to create a new .dsc\r\n"));
      f_close(&fileObject);
      return true;
   }

   // Create a new .dsc file
   fsResult = f_open(&fileObject, fileName, FA_CREATE_NEW);
   if (fsResult != FR_OK) {
      // Create .dsc file failed
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunDescriptor(): ERROR: Could not create new .dsc file!\r\n"));
      return false;
   }

   // LUN DSC file created successfully
   f_close(&fileObject);
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunDescriptor(): Successful\r\n"));
   return true;
}

// Function to read a LUN descriptor
bool filesystemReadLunDescriptor(uint8_t lunNumber, uint8_t buffer[])
{
   FRESULT fsResult;
   FIL fileObject;

   if (lunNumber >7)
   {
      // VFS doesn't support .dsc files
      return false;
   }

   // Assemble the .dsc file name
   sprintf(fileName, "/BeebSCSI%d/scsi%d.dsc", filesystemState.lunDirectory, lunNumber);

   fsResult = f_open(&fileObject, fileName, FA_READ);
   if (fsResult == FR_OK) {
      UINT fsCounter;
      // Read the .dsc data
      fsResult = f_read(&fileObject, buffer, 22, &fsCounter);

      // Check that the file was read OK and is the correct length
      if (fsResult != FR_OK  && fsCounter == 22) {
         // Something went wrong
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadLunDescriptor(): ERROR: Could not read .dsc file for LUN\r\n"));
         f_close(&fileObject);
         return false;
      }
   } else {
      // Looks like the .dsc file is not present on the file system
      debugStringInt16_P(PSTR("File system: filesystemReadLunDescriptor(): ERROR: Could not open .dsc file for LUN "), lunNumber, true);
      return false;
   }

   // Descriptor read OK
   f_close(&fileObject);
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemReadLunDescriptor(): Successful\r\n"));
   return true;
}

// Function to write a LUN descriptor
bool filesystemWriteLunDescriptor(uint8_t lunNumber, uint8_t const buffer[])
{
   FRESULT fsResult;
   FIL fileObject;

   if (lunNumber >7)
   {
      // VFS doesn't support .dsc files
      return false;
   }

   // Assemble the .dsc file name
   sprintf(fileName, "/BeebSCSI%d/scsi%d.dsc", filesystemState.lunDirectory, lunNumber);

   fsResult = f_open(&fileObject, fileName, FA_READ | FA_WRITE);
   if (fsResult == FR_OK) {
      UINT fsCounter;
      // Write the .dsc data
      fsResult = f_write(&fileObject, buffer, 22, &fsCounter);

      // Check that the file was written OK and is the correct length
      if (fsResult != FR_OK  && fsCounter == 22) {
         // Something went wrong
         if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteLunDescriptor(): ERROR: Could not write .dsc file for LUN\r\n"));
         f_close(&fileObject);
         return false;
      }
   } else {
      // Looks like the .dsc file is not present on the file system
      debugStringInt16_P(PSTR("File system: filesystemWriteLunDescriptor(): ERROR: Could not open .dsc file for LUN "), lunNumber, true);
      return false;
   }

   // Descriptor write OK
   f_close(&fileObject);
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteLunDescriptor(): Successful\r\n"));
   return true;
}

// Function to format a LUN image
bool filesystemFormatLun(uint8_t lunNumber, uint8_t dataPattern)
{
   FIL fileObject;
   FRESULT fsResult;

   if (lunNumber >7)
   {
      // VFS doesn't support .dsc files
      return false;
   }

   if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemFormatLun(): Formatting LUN image "), lunNumber, true);

   filesystemSetLunStatus(lunNumber, false );

   if (debugFlag_filesystem) debugStringInt32_P(PSTR("File system: filesystemFormatLun(): Sectors required = "), filesystemGetLunTotalSectors(lunNumber), true);

   // Assemble the .dat file name
   sprintf(fileName, "/BeebSCSI%d/scsi%d.dat", filesystemState.lunDirectory, lunNumber);

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

// Functions for creating and using LUN extended attributes-------------------------------------------------------------------

// Function to create a LUN Extended attributes tmp file for writing (makes an empty .tmp file)
bool filesystemCreateLunExtAttributes_tmp(uint8_t lunNumber)
{
	FRESULT fsResult;
   FIL fileObject;

   // Assemble the .external attributes as a temp file
   sprintf(fileName, "/BeebSCSI%d/scsi%d.tmp", filesystemState.lunDirectory, lunNumber);

   fsResult = f_open(&fileObject, fileName, FA_READ);
   if (fsResult == FR_OK) {
      // File opened ok - which means it already exists...
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunExtAttributes_tmp: .tmp already exists - ignoring request to create a new .tmp\r\n"));
      f_close(&fileObject);
      return true;
   }

   // Create a new .tmp Extended attributes file
   fsResult = f_open(&fileObject, fileName, FA_CREATE_NEW);
   if (fsResult != FR_OK) {
      // Create .ext file failed
      if (debugFlag_filesystem) {
			debugString_P(PSTR("File system: filesystemCreateLunExtAttributes_tmp: ERROR: Could not create new .tmp file!\r\n"));
      	debugStringInt16_P(PSTR("File system: filesystemCreateLunExtAttributes_tmp: ERROR = "), fsResult, true);
		}
		return false;
   }

   // LUN .tmp external attributes file created successfully
   f_close(&fileObject);
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunExtAttributes(): Successful\r\n"));
   return true;
}

// Function to rename a LUN Extended attributes .tmp file to a .ext file
bool filesystemCreateLunExtAttributes_Rename(uint8_t lunNumber)
{
	FRESULT fsResult;
	char fileName_source[255];

   // Assemble the names of the source and destination file
   sprintf(fileName_source, "/BeebSCSI%d/scsi%d.tmp", filesystemState.lunDirectory, lunNumber);
   sprintf(fileName, "/BeebSCSI%d/scsi%d.ext", filesystemState.lunDirectory, lunNumber);

	fsResult = f_rename (fileName_source, fileName);

   if (fsResult != FR_OK) {
      // File did not rename successfully
      if (debugFlag_filesystem) {
			debugString_P(PSTR("File system: filesystemCreateLunExtAttributes_Rename(): File Not Copied\r\n"));
      	debugStringInt16_P(PSTR("File system: filesystemCreateDscFromLunImage(): ERROR = "), fsResult, true);
		}
      return false;
   }

   // LUN external attributes file created successfully
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunExtAttributes_Rename(): Successful\r\n"));

	filesystemState.fsLunHasExtendedAttributes[lunNumber] = true;

	return true;
}


static uint8_t filesystemWriteline(FIL* fileObject, uint8_t *outString) {

	FRESULT fsResult;
	UINT fsCounter;
	size_t len;

	len = strlen((char *) outString);

	fsResult = f_write(fileObject, outString, len, &fsCounter);

	// Check that the file was written OK and is the correct length
	if (fsResult != FR_OK  && fsCounter == len) {
		// Something went wrong
		if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemWriteline: ERROR: writing Mode Page value\r\n"));
		return 1;
	}

	return 0;

}

// Function to write all values to to a .ext tmp file
bool filesystemCreateLunExtAttributes_WriteValues(uint8_t lunNumber)
{
	FRESULT fsResult_ext;
	FRESULT fsResult_tmp;
   FIL fileObject_ext;
   FIL fileObject_tmp;
	char fileName_tmp[255];
	uint8_t newdata[512];
	bool usedefaults = false;

	// if the .tmp file cannot be opened (it doesn't exist), create it
	if (!filesystemCreateLunExtAttributes_tmp(lunNumber)) {
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunExtAttributes_WriteValues: Major Error. .tmp file not accessible\r\n"));
		return false;
	}

   // Assemble the file name of the extended attributes temp file
   sprintf(fileName, "/BeebSCSI%d/scsi%d.ext", filesystemState.lunDirectory, lunNumber);
   sprintf(fileName_tmp, "/BeebSCSI%d/scsi%d.tmp", filesystemState.lunDirectory, lunNumber);

	// if an .ext file cannot be opened (it doesn't exist), flag to use defaults values
   fsResult_ext = f_open(&fileObject_ext, fileName, FA_READ);
   if (fsResult_ext != FR_OK) {
      // File didn't open, use stored default values ...
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunExtAttributes_WriteValues: No existing .ext file for the LUN. Using default values\r\n"));
		usedefaults = true;
      f_close(&fileObject_ext);
   }
	else
	{
     f_close(&fileObject_ext);
	}

	// Open .tmp file for writing
   fsResult_tmp = f_open(&fileObject_tmp, fileName_tmp, FA_WRITE);
	if (fsResult_tmp == FR_OK) {

		// copy all stored defalult values
		if (usedefaults) {

			// set up the list of default pages

			uint8_t default_NonModePageTokens[3] = {TOKEN_INQUIRY, TOKEN_MPHEADER, TOKEN_LBADESCRIPTOR};
			uint8_t default_ModePageTokens[9] = {1,3,4,32,33,35,36,37,38};
			uint8_t buff[2];

			// loop through the default Mode Pages that need creating
			buff[1] = 0; // length (0=default values)

			// loop through the default Non Mode Pages that need creating
			for (uint8_t i = 0; i < sizeof(default_NonModePageTokens); i++) {
				if (createNonModePage(usedefaults, default_NonModePageTokens[i], buff, newdata) == 0)
					filesystemWriteline(&fileObject_tmp, newdata);
				else
					if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemCreateLunExtAttributes_WriteValues(): ERROR: No value returned for non page mode token "), default_NonModePageTokens[i], true);
			}

			// loop through the default Mode Pages that need creating
			for (uint8_t i = 0; i < sizeof(default_ModePageTokens); i++) {
				buff[0] = default_ModePageTokens[i];

				if (createModePage(lunNumber, buff, newdata) == 0)
					filesystemWriteline(&fileObject_tmp, newdata);
				else
					if (debugFlag_filesystem) debugStringInt16_P(PSTR("File system: filesystemCreateLunExtAttributes_WriteValues(): ERROR: No Mode Page value returned for page "), buff[0], true);

			}

		}
		else {
			// .ext file open for reading. Loop through and copy all values
			f_close(&fileObject_ext);
		}
	}

	// close .tmp file
   f_close(&fileObject_tmp);

   // LUN external attributes written to temp file successfully
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCreateLunExtAttributes_WriteValues(): Attributes written to tmp file successfully\r\n"));

	// rename .tmp file to .ext file
	return filesystemCreateLunExtAttributes_Rename(lunNumber);

}

// Check an extended attributes file is available for that LUN
// and register it
bool filesystemCheckExtAttributes( uint8_t lunNumber)
{

   FRESULT fsResult;
   FIL fileObject;

   sprintf(extAttributes_fileName, "/BeebSCSI%d/scsi%d.ext", filesystemState.lunDirectory, lunNumber);

   // Check if the LUN extended attributes file (.ext) is present
   if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckExtAttributes: Checking for (.ext) LUN extended attributes file\r\n"));

   fsResult = f_open(&fileObject, extAttributes_fileName, FA_READ);

   if (fsResult != FR_OK) {
      // LUN extended properties file is not found
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckExtAttributes: LUN extended attributes file not found\r\n"));


		filesystemCreateLunExtAttributes_WriteValues(lunNumber);


		// continue using values from .dsc file and other generic defaults
		// filesystemState.fsLunHasExtendedAttributes[lunNumber] = false;

   } else {
      // LUN extended attributes file is present
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemCheckExtAttributes: LUN extended attributes file found\r\n"));

      // Read the extended attributes from the .ext file
		filesystemState.fsLunHasExtendedAttributes[lunNumber] = true;

   }

	// Update the cached geomety for this LUN from the extended attributes
   uint8_t Buffer[24];

	// Try read page 3
	if (readModePage(lunNumber, 3, 24, Buffer) !=0) {
		filesystemState.fsLunGeometry[lunNumber].BlockSize = (uint32_t)(((Buffer[9] << 16) + (Buffer[10] << 8) + Buffer[11]) & 0x00FFFFFF);
		filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack = (uint32_t)(((Buffer[22] << 8) + Buffer[23]) & 0x0000FFFF);
	}
	else
	{
		filesystemState.fsLunGeometry[lunNumber].SectorsPerTrack = DEFAULT_SECTORS_PER_TRACK;
		// write with mode select
	}

	if (readModePage(lunNumber, 4, 18, Buffer) !=0) {
		// Calculate the number of (block size) byte sectors required to fulfill the drive geometry
		// tracks = heads * cylinders
		// sectors = tracks * sectorsperTrack
		filesystemState.fsLunGeometry[lunNumber].BlockSize = (uint32_t)(((Buffer[9] << 16) + (Buffer[10] << 8) + Buffer[11]) & 0x00FFFFFF);
		filesystemState.fsLunGeometry[lunNumber].Cylinders = (uint32_t)(((Buffer[9] << 14) + (Buffer[15] << 8) + Buffer[16]) & 0x00FFFFFF);
		filesystemState.fsLunGeometry[lunNumber].Heads = (uint8_t)(Buffer[17]);
	}
	else
	{
		filesystemState.fsLunGeometry[lunNumber].BlockSize = DEFAULT_BLOCK_SIZE;
		// try and calculate cylinders / heads automatically

		// write with mode select
	}

	// Close the .ext file
	f_close(&fileObject);

	return filesystemState.fsLunHasExtendedAttributes[lunNumber];
}

// Check if a LUN has registered extended attributes when it was started
// and ensure the extAttributes filename is correct for the LUN
bool filesystemHasExtAttributes( uint8_t lunNumber)
{
   sprintf(extAttributes_fileName, "/BeebSCSI%d/scsi%d.ext", filesystemState.lunDirectory, lunNumber);
	return filesystemState.fsLunHasExtendedAttributes[lunNumber];
}





// Functions for reading and writing LUN images --------------------------------------------------------------------

// Function to open a LUN ready for reading
// Note: The read functions use a multi-sector buffer to lower the number of required
// reads from the physical media.  This is to allow more efficient (larger) reads of data.
bool filesystemOpenLunForRead(uint8_t lunNumber, uint32_t startSector, uint32_t requiredNumberOfSectors)
{
   // Move to the correct point in the DAT file
   // Check that the file seek was OK

   if (f_lseek(&filesystemState.fileObject[lunNumber], startSector * 256) != FR_OK) {
      // Something went wrong with seeking, do not retry
      if (debugFlag_filesystem) debugString_P(PSTR("File system: filesystemOpenLunForRead(): ERROR: Unable to seek to required sector in LUN image file!\r\n"));
      return false;
   }
   sectorsRemaining = requiredNumberOfSectors;
   sectorsInBuffer = 0;

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

      sectorsInBuffer = (uint8_t)sectorsToRead;
      currentBufferSector = 0;
      sectorsRemaining = sectorsRemaining - sectorsInBuffer;
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
   return false;
}

// Function to open a LUN ready for writing
bool filesystemOpenLunForWrite(uint8_t lunNumber, uint32_t startSector, uint32_t requiredNumberOfSectors)
{
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

      // Check that the file was written OK
      if (fsResult != FR_OK) {
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
   return false;
}


// Functions for FAT Transfer support --------------

// Change the filesystem's FAT transfer directory
bool filesystemSetFatDirectory(const uint8_t *buffer)
{
   sprintf(fatDirectory, "%s", buffer);
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
         char tempfileName[512];
         // Assemble the full path name and file name for the requested file
         sprintf(tempfileName, "%s/%s", fatDirectory, fsInfo.fname);
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

   return true;
}

// Close a FAT file previously opened for reading
bool filesystemCloseFatForRead(void)
{
   f_close(&fileObjectFAT);
   return true;
}

// read a file buffer can be malloced if the address is NULL

uint32_t filesystemReadFile(const char * filename, unsigned char **address, unsigned int max_size)
{
   UINT byteCounter;
   FRESULT fsResult;
   FIL fileObject;
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
   if (*address == NULL) {
      *address = malloc(f_size(&fileObject));
      if (*address == NULL) {
         f_close(&fileObject);
         return 0;
      }
      max_size = f_size(&fileObject);
   }
   f_read(&fileObject, address, max_size, &byteCounter);
   f_close(&fileObject);
   return f_size(&fileObject);
}
