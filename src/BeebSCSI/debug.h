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
extern  bool debugFlag_extended_attributes;

#ifdef DEBUG
#include "cpuspecfic.h"
/* Function prototypes */
void debugString_P(const char *addr);
void debugString_C(const char *addr, uint8_t style);
void debugString(const char *string);
void debugStringInt8Hex_P(const char *addr, uint8_t integerValue, bool newLine);
void debugStringInt16_P(const char *addr, uint16_t integerValue, bool newLine);
void debugStringInt32_P(const char *addr, uint32_t integerValue, bool newLine);

void debugSectorBufferHex(const uint8_t *buffer, uint16_t numberOfBytes);
void debugLunDescriptor(const uint8_t *buffer);
#define FCdebugString_P(...) { if (debugFlag_scsiFcodes) debugString_P(__VA_ARGS__); }
#define FCdebugString(...) { if (debugFlag_scsiFcodes) debugString(__VA_ARGS__); }
#define FCdebugStringInt8Hex_P(...) { if (debugFlag_scsiFcodes) debugStringInt8Hex_P(__VA_ARGS__); }
#define FCdebugStringInt16_P(...) { if (debugFlag_scsiFcodes) debugStringInt16_P(__VA_ARGS__); }
#define FCdebugStringInt32_P(...) { if (debugFlag_scsiFcodes) debugStringInt32_P(__VA_ARGS__); }

#else
#define debugString_P(...) {}
#define debugString_C(...) {}
#define debugString(...) {}
#define debugStringInt8Hex_P(...) {}
#define debugStringInt16_P(...) {}
#define debugStringInt32_P(...) {}
#define debugSectorBufferHex(...) {}
#define debugLunDescriptor(...) {}

#define FCdebugString_P(...) {}
#define FCdebugString(...) {}
#define FCdebugStringInt8Hex_P(...) {}
#define FCdebugStringInt16_P(...) {}
#define FCdebugStringInt32_P(...) {}
#endif

#define DEBUG_SUCCESS   0
#define DEBUG_INFO      1
#define DEBUG_WARNING   2
#define DEBUG_ERROR     3
#define DEBUG_SCSI_COMMAND 4

//Regular text
#define BLK "\033[0;30m"
#define RED "\033[0;31m"
#define GRN "\033[0;32m"
#define YEL "\033[0;33m"
#define BLU "\033[0;34m"
#define MAG "\033[0;35m"
#define CYN "\033[0;36m"
#define WHT "\033[0;37m"

//Regular bold text
#define BBLK "\033[1;30m"
#define BRED "\033[1;31m"
#define BGRN "\033[1;32m"
#define BYEL "\033[1;33m"
#define BBLU "\033[1;34m"
#define BMAG "\033[1;35m"
#define BCYN "\033[1;36m"
#define BWHT "\033[1;37m"

//Regular underline text
#define UBLK "\033[4;30m"
#define URED "\033[4;31m"
#define UGRN "\033[4;32m"
#define UYEL "\033[4;33m"
#define UBLU "\033[4;34m"
#define UMAG "\033[4;35m"
#define UCYN "\033[4;36m"
#define UWHT "\033[4;37m"

//Regular background
#define BLKB "\033[40m"
#define REDB "\033[41m"
#define GRNB "\033[42m"
#define YELB "\033[43m"
#define BLUB "\033[44m"
#define MAGB "\033[45m"
#define CYNB "\033[46m"
#define WHTB "\033[47m"

//High intensty background
#define BLKHB "\033[0;100m"
#define REDHB "\033[0;101m"
#define GRNHB "\033[0;102m"
#define YELHB "\033[0;103m"
#define BLUHB "\033[0;104m"
#define MAGHB "\033[0;105m"
#define CYNHB "\033[0;106m"
#define WHTHB "\033[0;107m"

//High intensty text
#define HBLK "\033[0;90m"
#define HRED "\033[0;91m"
#define HGRN "\033[0;92m"
#define HYEL "\033[0;93m"
#define HBLU "\033[0;94m"
#define HMAG "\033[0;95m"
#define HCYN "\033[0;96m"
#define HWHT "\033[0;97m"

//Bold high intensity text
#define BHBLK "\033[1;90m"
#define BHRED "\033[1;91m"
#define BHGRN "\033[1;92m"
#define BHYEL "\033[1;93m"
#define BHBLU "\033[1;94m"
#define BHMAG "\033[1;95m"
#define BHCYN "\033[1;96m"
#define BHWHT "\033[1;97m"

//Reset
#define COLOUR_RESET "\033[0m"

#endif /* DEBUG_H_ */
