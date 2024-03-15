/************************************************************************
    debug.h

    BeebSCSI serial debug functions
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

#ifndef DEBUG_H_
#define DEBUG_H_

/* External globals  // these should be volatile as they are set in the int handler
   gcc7 8 appear to optimise all the code away if they are set to volatile */
extern  bool debugFlag_filesystem;
extern  bool debugFlag_scsiCommands;
extern  bool debugFlag_scsiBlocks;
extern  bool debugFlag_scsiFcodes;
extern  bool debugFlag_scsiState;
extern  bool debugFlag_fatfs;

#ifdef DEBUG
#include "cpuspecfic.h"
/* Function prototypes */
void debugString_P(const char *addr);
void debugString(const char *string);
void debugStringInt8Hex_P(const char *addr, uint8_t integerValue, bool newLine);
void debugStringInt16_P(const char *addr, uint16_t integerValue, bool newLine);
void debugStringInt32_P(const char *addr, uint32_t integerValue, bool newLine);

void debugSectorBufferHex(const uint8_t *buffer, uint16_t numberOfBytes);
void debugLunDescriptor(const uint8_t *buffer);
#else
#define debugString_P(...) {}
#define debugString(...) {}
#define debugStringInt8Hex_P(...) {}
#define debugStringInt16_P(...) {}
#define debugStringInt32_P(...) {}
#define debugSectorBufferHex(...) {}
#define debugLunDescriptor(...) {}

#endif

#endif /* DEBUG_H_ */
