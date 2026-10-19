/************************************************************************
	scsi.h

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

#ifndef SCSI_H_
#define SCSI_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Global for the emulation mode (fixed or removable drive)
// Note: The fixed mode emulates SCSI-1 compliant hard drives for the Beeb
// The removable mode emulates the Laser Video Disc Player (LV-DOS) for Domesday
//extern uint8_t emulationMode;

// SCSI emulation bus states
#define SCSI_BUSFREE	0
#define SCSI_COMMAND	1
#define SCSI_STATUS		2
#define SCSI_MESSAGE	3

// SCSI emulation command states (Group 0 commands)
#define SCSI_TESTUNITREADY	10
#define SCSI_REZEROUNIT		11
#define SCSI_REQUESTSENSE	12
#define SCSI_FORMAT			13
#define SCSI_READ6			14
#define SCSI_WRITE6			15
#define SCSI_SEEK			16
#define SCSI_TRANSLATE		17
#define SCSI_MODESELECT6	18
#define SCSI_MODESENSE6		19
#define SCSI_STARTSTOP		20
#define SCSI_INQUIRY		21
#define SCSI_SENDDIAGNOSTIC	22
#define SCSI_REASSIGNBLOCKS 23

// SCSI emulation command states (Group 1 commands)
#define SCSI_VERIFY			24
#define SCSI_READCAPACITY	25
#define SCSI_READDEFECTDATA10 26

// SCSI emulation command states (Group 7 commands)
// vendor unique
#define SCSI_CERTIFY		29

// SCSI emulation command states (LV-DOS Group 6 commands)
#define SCSI_WRITE_FCODE	30
#define SCSI_READ_FCODE		31

// SCSI emulation command states (BeebSCSI Group 6 commands)
#define SCSI_BEEBSCSI_SENSE		40
#define SCSI_BEEBSCSI_SELECT	41
#define SCSI_BEEBSCSI_FATPATH	42
#define SCSI_BEEBSCSI_FATINFO	43
#define SCSI_BEEBSCSI_FATREAD	44

// SCSI Information transfer phases
#define ITPHASE_DATAOUT		0
#define ITPHASE_DATAIN		1
#define ITPHASE_COMMAND		2
#define ITPHASE_STATUS		3
#define ITPHASE_MESSAGEOUT	4
#define ITPHASE_MESSAGEIN	5

// Emulation mode (fixed / LV-DOS)
#define FIXED_EMULATION		0
#define LVDOS_EMULATION		1

// The controller behind the Acorn host adaptor (DriveController= in
// Pi1MHz.cfg, one for all drives).  It decides the commands a drive
// answers, its sense format and its FORMAT rules; Pi1MHz's own extensions
// (LUNs 2-7, the VFS LUNs, the group 6 commands, the jukebox) work under
// every type.  The default, Mixed, is Pi1MHz's own set as it has always
// been; the others are opt-in strict emulations.
typedef enum {
   SCSI_CONTROLLER_MIXED = 0,     // Pi1MHz's own mixed set, the default
   SCSI_CONTROLLER_ACB4000,       // Adaptec ACB-4000/4000A/4010 (MFM)
   SCSI_CONTROLLER_ACB4070,       // Adaptec ACB-4070 (RLL)
   SCSI_CONTROLLER_SASI,          // Xebec/DTC/OMTI-style boards (for now ACB-4000 rules)
   SCSI_CONTROLLER_SCSI           // a drive with its own controller (CCS-era SCSI)
} scsiController_t;

// Function prototypes
void scsiInitialise(void);
void scsiSetController(scsiController_t controller);
scsiController_t scsiGetController(void);
bool scsiSetControllerName(const char *name);
// A SCSI drive's pages 3 and 4 (and an Adaptec board's block size) wait
// for FORMAT: the page as FORMAT will make it, and dropping what waits
void scsiClearPendingPages(uint8_t lun);
const char * scsiGetPendingModePageData(uint8_t lun, uint8_t page, size_t * length);
// DriveGeometry= in Pi1MHz.cfg: what to do with a drive .cfg that has no
// geometry page.  Auto (the default) deduces it from the image size and
// publishes it; CFG takes the geometry from the .cfg only.
void scsiSetGeometryFromCfgOnly(bool cfgOnly);
bool scsiGeometryFromCfgOnly(void);
bool scsiSetGeometryName(const char *name);
void scsiReset(uint8_t scsiid);

void scsiJukeboxSwap(uint8_t lun);
bool scsiJukebox (uint8_t lun);

extern uint8_t scsiHostID;   /* last SELECT host id (16+ = VFS) */
void scsiProcessEmulation(void);
uint8_t scsiDiagState(void);
extern uint8_t scsiDiagCdb[6];
extern uint32_t scsiDiagXfer;
extern uint32_t scsiDiagCmdCount;

#endif /* SCSI_H_ */