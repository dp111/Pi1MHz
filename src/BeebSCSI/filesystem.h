/************************************************************************
	filesystem.h

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

#ifndef FILESYSTEM_H_
#define FILESYSTEM_H_

// Read/Write sector buffer (must be 256 bytes minimum)
// Testing shows that this is optimal when it matches the sector size of
// the SD card (which is 512 bytes).
#define SECTOR_BUFFER_SIZE	 (16*1024)

#define MAX_LUNS 16

// Calculate the length of the sector buffer in 256 byte sectors
#define SECTOR_BUFFER_LENGTH	(SECTOR_BUFFER_SIZE / 256)

// The default '33' is from the ACB-4000 which uses MFM encoding. The later
// ACB-4070 which Acorn used in the FileStore product uses RLL encoding and
// allows for a higher density of sectors per track.
#define DEFAULT_SECTORS_PER_TRACK 33
#define DEFAULT_BLOCK_SIZE 256


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

// External prototypes
void filesystemInitialise(uint8_t scsijuke);
void filesystemInitialiseVFS(uint8_t vfsjuke);
void filesystemReset(void);

bool filesystemMount(void);
bool filesystemDismount(void);

bool filesystemCheckLunImage(uint8_t lunNumber);

void filesystemSetLunDirectory(uint8_t scsiHostID, uint8_t lunDirectoryNumber);
uint8_t filesystemGetLunDirectory(void);
uint8_t filesystemGetLunDirectoryVFS(void);

// Indexes into the scsiattributes key table (and each LUN's parsed
// keyvalues) - must match the table order in filesystem.c
enum parserkeyvalueenum {
    TITLE,
    DESCRIPTION,
    INQUIRY,
    MODEPARAMHEADER,
    LBADESCRIPTOR,
    MODEPAGE0,
    MODEPAGE1,
    MODEPAGE3,
    WRITPAGE3,
    MODEPAGE4,
    MODEPAGE32,
    MODEPAGE33,
    MODEPAGE35,
    MODEPAGE36,
    MODEPAGE37,
    MODEPAGE38,
    LDUSERCODE,
    LDVIDEOXOFFSET
};

/* Read one value (TITLE or DESCRIPTION) of the mounted VFS disc from
   its already-parsed attributes into out (NUL-terminated, maxLen incl
   NUL). False if not mounted / key absent. For the disc menu. */
bool filesystemReadVFSCfgTextDir(uint8_t dir, enum parserkeyvalueenum key, char *out, uint32_t maxLen);
uint8_t filesystemVFSDirType(uint8_t dir);   /* 2 = scsi0.dat, 1 = video.pvf, 0 = none */

/* True when the current /BeebVFS<n> holds a video.pvf - the volume-present
   marker. Cheap (cached); used by the F-code path so a video-only disc
   (no scsi0.dat) can still be driven by the player F-codes. */
bool filesystemVFSVolumePresent(void);
bool filesystemVFSDatPresent(void);
bool filesystemVFSDirPresent(uint8_t dir);

bool filesystemSetLunStatus(uint8_t lunNumber, bool lunStatus);
bool filesystemReadLunStatus(uint8_t lunNumber);
bool filesystemTestLunStatus(uint8_t lunNumber);
void filesystemReadLunUserCode(uint8_t lunNumber, uint8_t userCode[5]);

void filesystemGetUserCodeFromUcd(uint8_t lunDirectoryNumber, uint8_t lunNumber);
bool filesystemCheckExtAttributes( uint8_t lunNumber);
void filesystemConfigToLunGeometry(uint8_t lunNumber);
void filesytemdattoconfigGeometry(uint8_t lunNumber);
void filesystemLunToconfigGeometry(uint8_t lunNumber);

void filesystemCopyPage0toPage4(uint8_t lunNumber);
void filesystemCopyPage4toPage0(uint8_t lunNumber);

void filesystemGetCylHeads( uint8_t lunNumber, uint8_t *returnbuf);
uint32_t filesystemGetLunBlockSize(uint8_t lunNumber);
uint32_t filesystemGetheadspercylinder(uint8_t lunNumber);
uint32_t filesystemGetLunSPTSize( uint8_t lunNumber);
uint32_t filesystemGetLunTotalSectors(uint8_t lunNumber);
uint32_t filesystemGetLunTotalBytes(uint8_t lunNumber);

bool filesystemCreateLunImage(uint8_t lunNumber);
bool filesystemCreateLunDescriptor(uint8_t lunNumber);
bool filesystemReadLunDescriptor(uint8_t lunNumber);
bool filesystemWriteAttributes(uint8_t lunNumber);
bool filesystemFormatLun(uint8_t lunNumber, uint8_t dataPattern);

// Host-side (MTP / WebDAV) write interlock -- see filesystem.c
bool   filesystemHostPathBusy(const char *path);
int8_t filesystemLunFromHostPath(const char *path);
void   filesystemHostLockLun(int8_t lunNumber, bool lock);
bool   filesystemLunHostLocked(uint8_t lunNumber);
void   filesystemHostRevokeLun(uint8_t lunNumber);
bool   filesystemHostLunRevoked(uint8_t lunNumber);

bool filesystemOpenLunForRead(uint8_t lunNumber, uint32_t startSector, uint32_t requiredNumberOfSectors);
bool filesystemReadNextSector(uint8_t lunNumber, uint8_t **buffer);
bool filesystemCloseLunForRead(uint8_t lunNumber);
bool filesystemOpenLunForWrite(uint8_t lunNumber, uint32_t startSector, uint32_t requiredNumberOfSectors);
bool filesystemWriteNextSector(uint8_t lunNumber, uint8_t const buffer[]);
bool filesystemCloseLunForWrite(uint8_t lunNumber);

char * filesystemGetInquiryData(uint8_t lunNumber, size_t * length);
char * filesystemGetModeParamHeaderData(uint8_t lunNumber, size_t * length);
char * filesystemGetLBADescriptorData(uint8_t lunNumber, size_t * length);
char * filesystemGetModePageData(uint8_t lunNumber, uint8_t page, size_t * length);
int filesystemWriteModePageData(uint8_t lunNumber, uint8_t page, uint8_t len, const uint8_t * Buffer);

bool filesystemSetFatDirectory(const uint8_t * buffer);
bool filesystemGetFatFileInfo(uint32_t fileNumber, uint8_t *buffer);
bool filesystemOpenFatForRead(uint32_t fileNumber, uint32_t blockNumber);
bool filesystemReadNextFatBlock(uint8_t *buffer);
bool filesystemCloseFatForRead(void);

uint32_t filesystemReadFile(const char * filename, uint8_t **address, unsigned int max_size);

/* Attach a FatFs fast-seek cluster link map to an open file, sized to the
   file's real fragmentation (FatFs's CREATE_LINKMAP size-negotiation is
   wrapped here so callers never touch cltbl[] themselves). The map lives
   in *map/ *entries, which the caller owns across reopens; returns false
   if the file must fall back to slow seeks. (Only declared for callers
   that already include ff.h - most filesystem.h users do not.) */
#ifdef FF_DEFINED
bool filesystemAttachLinkMap(FIL *file, DWORD **map, uint32_t *entries);
/* The mounted FatFs object (read-only geometry access for the webserver's
   incremental free-space scan), or NULL when no card is mounted. */
FATFS *filesystemGetFsObject(void);
#endif
uint32_t filesystemWriteFile(const char * filename, const uint8_t *address, uint32_t max_size);
#endif /* FILESYSTEM_H_ */