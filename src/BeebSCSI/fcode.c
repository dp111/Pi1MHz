/************************************************************************
	fcode.c

	BeebSCSI F-Code emulation functions
    BeebSCSI - BBC Micro SCSI Drive Emulator
    Copyright (C) 2018-2020 Simon Inns

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

// Global includes
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Local includes
#include "uart.h"
#include "debug.h"
#include "filesystem.h"
#include "fcode.h"
#include "../rpi/screen.h"

// Global SCSI (LV-DOS) F-Code buffer (256 bytes)
uint8_t scsiFcodeBuffer[256];
uint8_t scsiFcodeBufferRX[256];


static char VPmode;
// Function to handle F-Code buffer write actions
void fcodeWriteBuffer(uint8_t lunNumber)
{
	uint16_t fcodeLength = 0;
	uint16_t byteCounter;

	// Clear the serial read buffer (as we are sending a new F-Code)
	//uartFlush(); // Flushes the UART Rx buffer

	// Output the F-Code bytes to debug
	FCdebugString_P(PSTR("F-Code: Received bytes:"));

	// Write out the buffer until a CR character is found
	for (byteCounter = 0; byteCounter < 256; byteCounter++) {
		FCdebugStringInt8Hex_P(PSTR(" "), scsiFcodeBuffer[byteCounter], false);
		if (scsiFcodeBuffer[byteCounter] == 0x0D) break;
		fcodeLength++;
	}
	FCdebugString_P(PSTR("\r\n"));

	// F-Code decoding for debug output

		// Display the F-Code command value
		FCdebugStringInt8Hex_P(PSTR("F-Code: Received F-Code "), scsiFcodeBuffer[0], false);

		// Display the F-Code command function
		switch(scsiFcodeBuffer[0]) {
			case 0x21: // !xy
			FCdebugString_P(PSTR(" = Sound insert (beep)\r\n"));
			break;

			case 0x23: // #xy
			FCdebugString_P(PSTR(" = RC-5 command out via A/V EUROCONNECTOR\r\n"));
			break;

			case 0x24: // $0, $1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Replay switch disable\r\n")); // VFS sends this
                scsiFcodeBufferRX[0] = 'A'; // Open ?
                scsiFcodeBufferRX[1] = 0x0D;
				break;

				case '1':
				FCdebugString_P(PSTR(" = Replay switch enable\r\n"));
				break;

				default:
					FCdebugString_P(PSTR(" = Replay switch (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x27: // ' // VFS sends this
			FCdebugString_P(PSTR(" = Eject (open the front-loader tray)\r\n"));
            // Response O when open
			break;

			case 0x29: // )0, )1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Transmission delay off\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Transmission delay on\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Transmission delay (invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x2A: // * // VFS sends this
			switch(scsiFcodeBuffer[1]) {
				case 0x0D:
				// No parameter, assume default
				FCdebugString_P(PSTR(" = Halt (still mode)\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Repetitive halt and jump\r\n"));
				break;
			}
			break;

			case 0x2B: // + yy ( yy = 1..50) // VFS sends this
			FCdebugString_P(PSTR(" = Instant jump forwards\r\n"));
			break;

			case 0x2C: // ,
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Standby (unload)\r\n"));
				break;

				case '1': // response S or O if tray open
				FCdebugString_P(PSTR(" = On (load)\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Standby/On (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x2D: // - yy ( yy = 1..50)
			FCdebugString_P(PSTR(" = Instant jump backwards\r\n"));
			break;

			case 0x2F: // /
			FCdebugString_P(PSTR(" = Pause (halt + all muted)\r\n"));
			break;

			case 0x3A: // :
			FCdebugString_P(PSTR(" = Reset to default values\r\n"));
			break;

			case 0x3F: // ?
			switch(scsiFcodeBuffer[1]) {
				case 'F': // response Fxxxxx X if not available O if open
				FCdebugString_P(PSTR(" = Picture number request\r\n"));
				break;

				case 'C': // response C xx X if not available O if open
				FCdebugString_P(PSTR(" = Chapter number request\r\n"));
				break;

				case 'D':
				FCdebugString_P(PSTR(" = Disc program status request\r\n"));
				break;

				case 'P':
				FCdebugString_P(PSTR(" = Player status request\r\n"));
				break;

				case 'U':
				FCdebugString_P(PSTR(" = User code request\r\n"));

                // If the F-Code is a user code request (?U)

				scsiFcodeBufferRX[0] = 'U';
				// Get the user code for the target LUN
                filesystemReadLunUserCode(lunNumber, &scsiFcodeBufferRX[1]);
                scsiFcodeBufferRX[6] = 0x0D; // terminator
				break;

				case '=':
				FCdebugString_P(PSTR(" = Revision level request\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Request (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x41: // A0, A1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Audio-1 off\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Audio-1 on\r\n"));
                    scsiFcodeBufferRX[0] = 'A';
                    scsiFcodeBufferRX[1] = 0x0D;
				break;

				default:
				FCdebugString_P(PSTR(" = Audio-1 (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x42: // B0, B1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Audio-2 off\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Audio-2 on\r\n"));
                    scsiFcodeBufferRX[0] = 'A';
                    scsiFcodeBufferRX[1] = 0x0D;
				break;

				default:
				FCdebugString_P(PSTR(" = Audio-2 (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x43: // C0, C1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Chapter number display off\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Chapter number display on\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Chapter number display (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x44: // D0, D1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Picture number/time code display off\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Picture number/time code display on\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Picture number/time code display (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x45: // E // VFS sends this
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Video off\r\n"));
				screen_plane_enable(0, false);
				break;

				case '1':
				FCdebugString_P(PSTR(" = Video on\r\n"));
				screen_plane_enable(0, true);
				break;

				default:
				FCdebugString_P(PSTR(" = Video (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x46: // F
			{
				uint32_t pictureNumber = 0;
				char op;
				FCdebugString_P(PSTR(" = Load/Goto picture number : "));

				for (byteCounter = 0; byteCounter < 32; byteCounter++) {
					if (scsiFcodeBuffer[byteCounter] == 0x0D) break;
					if ((char)scsiFcodeBuffer[byteCounter] <= '9') {
						pictureNumber = pictureNumber * 10 + (scsiFcodeBuffer[byteCounter] - '0');
					}
					else
						{
							op = (char) scsiFcodeBuffer[byteCounter];
						}
					}
				scsiFcodeBuffer[byteCounter] = 0;
				FCdebugStringInt32_P(PSTR(""), pictureNumber, false);
				FCdebugString_P(PSTR(" op: "));
				FCdebugString_P(PSTR((char *)&scsiFcodeBuffer[byteCounter-1]));

				switch(scsiFcodeBuffer[byteCounter-1]) {
					case 'I':
					FCdebugString_P(PSTR(" = Load Picture Info register\r\n"));
					scsiFcodeBufferRX[0] = 'A';
					scsiFcodeBufferRX[1] = '3'; // when passed
					break;

					case 'S':
					FCdebugString_P(PSTR(" = Stop Register\r\n"));
					scsiFcodeBufferRX[0] = 'A';
					scsiFcodeBufferRX[1] = '2'; // when stops
					break;

					case 'R':
					FCdebugString_P(PSTR(" = Still picture\r\n"));
					scsiFcodeBufferRX[0] = 'A';
					scsiFcodeBufferRX[1] = '0';

					break;

					case 'N':
					FCdebugString_P(PSTR(" = Goto Picture and play normally\r\n"));
					scsiFcodeBufferRX[0] = 'A';
					scsiFcodeBufferRX[1] = '1'; // when complete

					break;

					case 'Q':
					FCdebugString_P(PSTR(" = Goto Picture and play in previous mode\r\n"));
					scsiFcodeBufferRX[0] = 'A';
					scsiFcodeBufferRX[1] = '0';

					break;

					default:
					FCdebugString_P(PSTR(" = (Invalid parameter)\r\n"));
					scsiFcodeBufferRX[0] = 'A';
					scsiFcodeBufferRX[1] = '0';
					break;

				}
				// Display frame!
				scsiFcodeBufferRX[2] = 0x0D;
			}
			break;

			case 0x48: // H
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Remote control not routed to computer\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Remote control routed to computer\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Remote control routed (Invalid parameter)\r\n"));
				break;
			}

			break;

			case 0x49: // I // Domesday sends this
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Local front panel buttons disabled\r\n"));
                scsiFcodeBufferRX[0] = 'A';
                scsiFcodeBufferRX[1] = 0x0D;
				break;

				case '1':
				FCdebugString_P(PSTR(" = Local front panel buttons enabled\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Local front panel buttons (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x4A: // J // Domesday sends this
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Remote control disabled for player control\r\n"));
                scsiFcodeBufferRX[0] = 'A';
                scsiFcodeBufferRX[1] = 0x0D;
				break;

				case '1':
				FCdebugString_P(PSTR(" = Remote control enabled for player control\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Remote control for player control (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x4C: // L // VFS sends this
			FCdebugString_P(PSTR(" = Still forward\r\n"));
			break;

			case 0x4D: // M // VFS sends this
			FCdebugString_P(PSTR(" = Still reverse\r\n"));
			break;

			case 0x4E: // N // VFS sends this
			FCdebugString_P(PSTR(" = Play forward\r\n"));
			break;

			case 0x4F: // O // VFS sends this
			FCdebugString_P(PSTR(" = Play reverse\r\n"));
			break;

			case 0x51: // Q // VFS sends this
			FCdebugString_P(PSTR(" = Goto chapter and halt/play\r\n"));
			break;

			case 0x52: // R // VFS sends this
			FCdebugString_P(PSTR(" = Slow/Fast read\r\n"));
			break;

			case 0x53: // S // VFS sends this
			FCdebugString_P(PSTR(" = Set fast/slow speed value\r\n"));
			break;

			case 0x54: // T
			FCdebugString_P(PSTR(" = Goto/Load time code register\r\n"));
			break;

			case 0x55: // U // VFS sends this
			FCdebugString_P(PSTR(" = Slow motion forward\r\n"));
			break;

			case 0x56: // V, VP // VFS sends this
			switch(scsiFcodeBuffer[1]) {
				case 'P':
				VPmode = scsiFcodeBuffer[1];
				switch(scsiFcodeBuffer[2]) {
					case '1':
					FCdebugString_P(PSTR(" = Video overlay mode 1 (LaserVision video only)\r\n"));
					screen_plane_enable(0, true);
					screen_plane_enable(1, false);
					screen_plane_enable(2, false);

					break;

					case '2':
					FCdebugString_P(PSTR(" = Video overlay mode 2 (External (computer) RGB only)\r\n"));
					screen_set_palette( 1, 0, 3 );
					screen_plane_enable(0, false);
					screen_plane_enable(1, true);
					screen_plane_enable(2, true);

					break;

					case '3':
					FCdebugString_P(PSTR(" = Video overlay mode 3 (Hard-keyed)\r\n"));
					screen_set_palette( 1, 0, 2 );
					screen_plane_enable(0, true);
					screen_plane_enable(1, true);
					screen_plane_enable(2, true);

					break;

					case '4':
					FCdebugString_P(PSTR(" = Video overlay mode 4 (Mixed)\r\n"));
					break;

					case '5':
					FCdebugString_P(PSTR(" = Video overlay mode 5 (Enhanced)\r\n"));
					break;

					case 'X':
					FCdebugString_P(PSTR(" = Video overlay mode request\r\n")); // Domesday sends this
					scsiFcodeBufferRX[0] = 'V';
                    scsiFcodeBufferRX[1] = 'P';
                    scsiFcodeBufferRX[2] = VPmode;
                    scsiFcodeBufferRX[3] = 0x0D;
					break;

					default:
					FCdebugString_P(PSTR(" = Video overlay mode (Invalid parameter)\r\n"));
					break;
				}
				break;

				case 0x0D:
				FCdebugString_P(PSTR(" = Slow motion reverse\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Slow motion reverse (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x57: // W // VFS sends this
			FCdebugString_P(PSTR(" = Fast forward\r\n"));
			break;

			case 0x58: // X
			FCdebugString_P(PSTR(" = Clear\r\n"));
			break;

			case 0x5A: // Z // VFS sends this
			FCdebugString_P(PSTR(" = Fast reverse\r\n"));
			break;

			case 0x5B: // [0, [1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Audio-1 from internal\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Audio-1 from external\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Audio-1 from (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x5C: // '\'
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Video from internal\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Video from external\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Video from (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x5D: // ]0, ]1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Audio-2 from internal\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Audio-2 from external\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Audio-2 from (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x5F: // _0, _1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Teletext from disc off\r\n"));
				break;

				case '1':
				FCdebugString_P(PSTR(" = Teletext from disc on\r\n"));
				break;

				default:
				FCdebugString_P(PSTR(" = Teletext from disc (Invalid parameter)\r\n"));
				break;
			}
			break;

			default:
			FCdebugString_P(PSTR("Unknown!\r\n"));
			break;
		}

	// Send the F-Code to the serial UART
//	printf("<FCODE>");
//	for (byteCounter = 0; byteCounter < fcodeLength; byteCounter++)
//		printf("%c", scsiFcodeBuffer[byteCounter]);
//	printf("</FCODE>\r\n");
}

// Function to copy the UART serial buffer into the fcodeBuffer
void fcodeReadBuffer(void)
{
    FCdebugString_P(PSTR("fcodeReadBuffer\r\n"));
    for (uint16_t byteCounter = 0; byteCounter < 7; byteCounter++) {
		FCdebugStringInt8Hex_P(PSTR(" "), scsiFcodeBufferRX[byteCounter], false);
    }
#if 0
	uint16_t byteCounter = 0;
	uint16_t availableBytes = 0;

	// Clear the F-code buffer
	for (byteCounter = 0; byteCounter < 256; byteCounter++) scsiFcodeBuffer[byteCounter] = 0;

	// Get the number of available bytes in the UART Rx buffer
	availableBytes = uartAvailable();

	if (debugFlag_scsiFcodes) debugStringInt16_P(PSTR("F-Code: Serial UART bytes waiting =  "), availableBytes, true);

	// Ensure we have a full F-code response terminated with
	// 0x0D (CR) before we send it to the host
	if (uartPeekForString()) {
		if (debugFlag_scsiFcodes) FCdebugString_P(PSTR("F-Code: Transmitting F-Code bytes: "));

		// Copy the UART Rx buffer to the F-Code buffer
		for (byteCounter = 0; byteCounter < availableBytes; byteCounter++) {
			scsiFcodeBuffer[byteCounter] = (char)(uartRead() & 0xFF);
			if (debugFlag_scsiFcodes) debugStringInt8Hex_P(PSTR(" "), scsiFcodeBuffer[byteCounter], false);
		}
		if (debugFlag_scsiFcodes) FCdebugString_P(PSTR("\r\n"));
	}
	// If there is nothing to send we should reply with only a CR according
	// to page 40 of the VP415 operating instructions (C8H Read F-code reply)
	else {
		if (debugFlag_scsiFcodes) FCdebugString_P(PSTR("F-Code: No response from host; sending empty CR terminated response.\r\n"));
		scsiFcodeBuffer[0] = 0x0D;
	}
#endif
}

void fcodeClearBuffer(void)
{
    FCdebugString_P(PSTR(" fcodeClearBuffer\r\n"));
    scsiFcodeBufferRX[0] = 0x0D;
}