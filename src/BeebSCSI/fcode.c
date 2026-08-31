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
#include "../harddisc_emulator.h"   /* hd_juke_request - the eject/disc-flip swap */
#include <stdint.h>
#include <stdio.h>

// Local includes
#include "uart.h"
#include "debug.h"
#include "filesystem.h"
#include "fcode.h"
#include "../rpi/systimer.h"   /* timestamps for the F-code history */
#include "../rpi/screen.h"
#include "../videoplayer.h"

// Global SCSI (LV-DOS) F-Code buffer (256 bytes)
uint8_t scsiFcodeBuffer[256];
uint8_t scsiFcodeBufferRX[256];

/* Last exchange, for /status: the VFS ROM's *FCODE never shows replies,
   so this is the only way to see one without writing OSWORD &62 code */
/* Acknowledgements the VP415 gives when the ACTION completes, not when the
   command is accepted.  The manual is explicit for both cases used here:
   FxxxxxS - "the player halts at the specified picture number when it is
   reached ... and the acknowledgement is then given"; ' (eject) - "Response:
   O when the tray is opened".  Answering on receipt told the host a
   60-second play had finished in 17 ms, so it cut the audio and moved on,
   and told it a disc side was loaded before the jukebox had switched.
   While one is outstanding the reply buffer is empty, which is what a host
   polling a real player sees. */
typedef enum { FC_ACK_IDLE = 0, FC_ACK_JUKE, FC_ACK_STOP } fc_ack_t;
static fc_ack_t fc_ack_wait;
static char     fc_ack_reply[2];

static void fcode_ack_when(fc_ack_t on_what, char r0, char r1)
{
   fc_ack_wait     = on_what;
   fc_ack_reply[0] = r0;
   fc_ack_reply[1] = r1;
   scsiFcodeBufferRX[0] = 0x0D;      /* nothing to report yet */
}

/* Called from the SCSI poll, beside hd_juke_service. */
void fcodePoll(void)
{
   bool done;

   if (fc_ack_wait == FC_ACK_IDLE)
      return;

   switch (fc_ack_wait) {
      case FC_ACK_JUKE: done = !hd_juke_busy();                 break;
      case FC_ACK_STOP: done = videoplayer_take_stop_reached(); break;
      default:          done = true;                            break;
   }
   if (!done)
      return;

   scsiFcodeBufferRX[0] = (uint8_t)fc_ack_reply[0];
   if (fc_ack_reply[1] != 0) {
      scsiFcodeBufferRX[1] = (uint8_t)fc_ack_reply[1];
      scsiFcodeBufferRX[2] = 0x0D;
   } else {
      scsiFcodeBufferRX[1] = 0x0D;
   }
   fc_ack_wait = FC_ACK_IDLE;
}

/* Recent F-code exchanges, newest last, for diagnosing a sequence the Beeb
   drives (a play range, a jukebox change) where the last exchange alone says
   nothing.  Written once per exchange - the Beeb sends these at human rate,
   so the cost is a memcpy per command, not a hot path.  Read out over
   /fcodes rather than a /status row, because those share one 144-byte
   buffer and truncate silently. */
#define FCODE_HIST 320
typedef struct { uint32_t ms; char tx[20]; char rx[28]; } fcode_hist_t;
static fcode_hist_t fcode_hist[FCODE_HIST];
static uint16_t fcode_hist_count;      /* total ever recorded; index = %FCODE_HIST */

static void fcode_hist_record(void)
{
   fcode_hist_t *h = &fcode_hist[fcode_hist_count % FCODE_HIST];
   size_t o = 0u;
   h->ms = RPI_GetSystemTime() / 1000u;
   for (int i = 0; i < 24 && scsiFcodeBuffer[i] != 0x0D && scsiFcodeBuffer[i]; i++)
      if (o < sizeof h->tx - 1u) h->tx[o++] = (char)scsiFcodeBuffer[i];
   h->tx[o] = 0;
   o = 0u;
   for (int i = 0; i < 48 && scsiFcodeBufferRX[i] != 0x0D && scsiFcodeBufferRX[i]; i++)
      if (o < sizeof h->rx - 1u) h->rx[o++] = (char)scsiFcodeBufferRX[i];
   h->rx[o] = 0;
   fcode_hist_count++;
}

/* Oldest first, one "<ms> <tx> -> <rx>" per line.  Returns bytes written. */
size_t fcodeHistoryText(char *out, size_t max)
{
   size_t o = 0u;
   uint16_t n = (fcode_hist_count < FCODE_HIST) ? fcode_hist_count : FCODE_HIST;
   uint16_t first = (fcode_hist_count < FCODE_HIST)
                  ? 0u : (uint16_t)(fcode_hist_count % FCODE_HIST);
   for (uint16_t k = 0u; k < n && o < max - 1u; k++) {
      const fcode_hist_t *h = &fcode_hist[(first + k) % FCODE_HIST];
      o += (size_t)snprintf(out + o, max - o, "%lu %s -> %s\n",
                            (unsigned long)h->ms, h->tx, h->rx);
   }
   return o;
}

const char *fcodeLastExchange(void)
{
    static char buf[96];
    size_t o = 0;
    o += (size_t)snprintf(buf + o, sizeof buf - o, "tx ");
    for (int i = 0; i < 24 && scsiFcodeBuffer[i] != 0x0D && scsiFcodeBuffer[i]; i++)
        if (o < sizeof buf - 2) buf[o++] = (char)scsiFcodeBuffer[i];
    o += (size_t)snprintf(buf + o, sizeof buf - o, " rx ");
    for (int i = 0; i < 48 && scsiFcodeBufferRX[i] != 0x0D && scsiFcodeBufferRX[i]; i++)
        if (o < sizeof buf - 2) buf[o++] = (char)scsiFcodeBufferRX[i];
    buf[o] = 0;
    return buf;
}


static char VPmode;

/* Latched by their own F-codes purely so ?P can report them (the ROM's
   player-status bits 4.4/4.3/4.2 and 5.3). Nothing else consumes them. */
static bool fc_delay_on;      /* ) transmission delay */
static bool fc_rc_computer;   /* H remote control routed to computer */
static bool fc_remote_on;     /* J remote control enabled */
static bool fc_teletext_on;   /* _ teletext from disc */

/* ?Vnn / ?Tnn / ?Ynn - query side nn WITHOUT jukeboxing to it. A jukebox is
   a remount, and the menu's rescan was paying one per side purely to read
   its name. Missing or malformed digits are a bad request, answered 'X'. */
static bool fcodeDirParam(uint8_t *dir)
{
	uint32_t v = 0;
	int digits = 0;
	for (int i = 2; i < 5; i++) {
		char c = (char)scsiFcodeBuffer[i];
		if (c < '0' || c > '9') break;
		v = (v * 10u) + (uint32_t)(c - '0');
		digits++;
	}
	if (!digits || v > 255u)
		return false;
	*dir = (uint8_t)v;
	return true;
}
// Function to handle F-Code buffer write actions
void fcodeWriteBuffer(uint8_t lunNumber)
{
	//uint16_t fcodeLength = 0;
	uint16_t byteCounter;

	// Clear the serial read buffer (as we are sending a new F-Code)
	//uartFlush(); // Flushes the UART Rx buffer

	// Output the F-Code bytes to debug
	FCdebugString_P(PSTR("F-Code: Received bytes:"));

	// Write out the buffer until a CR character is found
	for (byteCounter = 0; byteCounter < 256; byteCounter++) {
		FCdebugStringInt8Hex_P(PSTR(" "), scsiFcodeBuffer[byteCounter], false);
		if (scsiFcodeBuffer[byteCounter] == 0x0D) break;
		//fcodeLength++;
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
				/* No reply: the VP415 command set lists none for $0/$1. The
				   'A' that used to go here was a guess. */
				FCdebugString_P(PSTR(" = Replay switch disable\r\n")); // VFS sends this
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
			{
				/* The AIV disc-flip: eject means "turn the disc over", so
				   swap to the partner side's directory (odd <-> even:
				   1<->2, 3<->4, ...). Directory 0 is the menu - no partner.
				   The switch itself runs in hd_juke_service on the next
				   SCSI poll, exactly like a jukebox poke. */
				uint8_t cur = (uint8_t)filesystemGetLunDirectoryVFS();
				if (cur >= 1) {
					uint8_t partner = (cur & 1u) ? (uint8_t)(cur + 1u)
					                             : (uint8_t)(cur - 1u);
					/* Single-sided disc: no partner directory - the tray
					   opens and the same side is "reinserted". */
					if (filesystemVFSDirPresent(partner))
						hd_juke_request(partner);
				}
				/* "Response: O when the tray is opened" - the swap runs in
				   hd_juke_service on the next SCSI poll, so the host must not
				   be told the new side is in until it has. */
				fcode_ack_when(FC_ACK_JUKE, 'O', 0);
			}
			break;

			case 0x29: // )0, )1
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Transmission delay off\r\n"));
				fc_delay_on = false;
				break;

				case '1':
				FCdebugString_P(PSTR(" = Transmission delay on\r\n"));
				fc_delay_on = true;
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
				videoplayer_halt();
				break;

				default:
				FCdebugString_P(PSTR(" = Repetitive halt and jump\r\n"));
				videoplayer_halt();
				break;
			}
			break;

			case 0x2B: // + yy ( yy = 1..50) // VFS sends this
			FCdebugString_P(PSTR(" = Instant jump forwards\r\n"));
			{
				int jump = 0;
				for (byteCounter = 1; byteCounter < 3; byteCounter++) { // yy = 1..50, 2 digits max
					char c = (char)scsiFcodeBuffer[byteCounter];
					if (c < '0' || c > '9') break;
					jump = jump * 10 + (c - '0');
				}
				if (jump && videoplayer_active())
					videoplayer_goto(videoplayer_picture_number() + (uint32_t)jump, 'Q');
			}
			break;

			case 0x2C: // ,
			switch(scsiFcodeBuffer[1]) {
				case '0':
				FCdebugString_P(PSTR(" = Standby (unload)\r\n"));
				break;

				case '1': // response S or O if tray open
				FCdebugString_P(PSTR(" = On (load)\r\n"));
				scsiFcodeBufferRX[0] = 'S';   /* loaded and standing by */
				scsiFcodeBufferRX[1] = 0x0D;
				break;

				default:
				FCdebugString_P(PSTR(" = Standby/On (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x2D: // - yy ( yy = 1..50)
			FCdebugString_P(PSTR(" = Instant jump backwards\r\n"));
			{
				int jump = 0;
				for (byteCounter = 1; byteCounter < 3; byteCounter++) { // yy = 1..50, 2 digits max
					char c = (char)scsiFcodeBuffer[byteCounter];
					if (c < '0' || c > '9') break;
					jump = jump * 10 + (c - '0');
				}
				uint32_t pic = videoplayer_picture_number();
				if (jump && videoplayer_active() && pic > (uint32_t)jump)
					videoplayer_goto(pic - (uint32_t)jump, 'Q');
			}
			break;

			case 0x2F: // /
			FCdebugString_P(PSTR(" = Pause (halt + all muted)\r\n"));
			videoplayer_pause();
			break;

			case 0x3A: // :
			FCdebugString_P(PSTR(" = Reset to default values\r\n"));
			videoplayer_clear();
			break;

			case 0x3F: // ?
			switch(scsiFcodeBuffer[1]) {
				case 'F': // response Fxxxxx X if not available O if open
				FCdebugString_P(PSTR(" = Picture number request\r\n"));
				if (videoplayer_active()) {
					uint32_t pic = videoplayer_picture_number();
					scsiFcodeBufferRX[0] = 'F';
					for (int digit = 5; digit >= 1; digit--) {
						scsiFcodeBufferRX[digit] = (uint8_t)('0' + (pic % 10));
						pic /= 10;
					}
					scsiFcodeBufferRX[6] = 0x0D;
				} else {
					/* VP415: 'X' = function not available (no disc playing) */
					scsiFcodeBufferRX[0] = 'X';
					scsiFcodeBufferRX[1] = 0x0D;
				}
				break;

				case 'C': // response C xx X if not available O if open
				/* No reply: the ROM uses 'X' for ?F and ?D only, and ?P
				   already reports "chapter numbers exist" = 0, which is how
				   the host is meant to know not to ask. */
				FCdebugString_P(PSTR(" = Chapter number request\r\n"));
				break;

				case 'D':
				FCdebugString_P(PSTR(" = Disc program status request\r\n"));
				scsiFcodeBufferRX[0] = 'X';   /* not known */
				scsiFcodeBufferRX[1] = 0x0D;
				break;

				case 'P':
				/* Five status characters, each '@' with bits OR-ed in, per
				   the VP415 ROM's command set. Replay is reported off and
				   the picture always frame-locked (owner's call); chapters
				   and CLV do not apply - our discs are CAV. */
				FCdebugString_P(PSTR(" = Player status request\r\n"));
				{
					uint8_t st[5];
					bool loaded = filesystemVFSVolumePresent() ||
					              filesystemVFSDatPresent();
					st[0] = (uint8_t)('@'
					      | (loaded ? 0x20u : 0u)                  /* disc loaded */
					      /* bits 1-0 are the goto-action field; the ROM
					         notes record no encoding for it, so this
					         asserts only "a goto is outstanding" */
					      | (videoplayer_seeking() ? 0x01u : 0u));
					st[1] = (uint8_t)('@' | 0x01u);                /* CAV, no chapters */
					st[2] = (uint8_t)('@' | 0x01u);                /* frame lock, replay off */
					st[3] = (uint8_t)('@'
					      | (fc_delay_on    ? 0x10u : 0u)
					      | (fc_remote_on   ? 0x08u : 0u)
					      | (fc_rc_computer ? 0x04u : 0u));        /* local never set */
					st[4] = (uint8_t)('@'
					      | (videoplayer_audio_enabled(1) ? 0x20u : 0u)
					      | (videoplayer_audio_enabled(0) ? 0x10u : 0u)
					      | (fc_teletext_on ? 0x08u : 0u));
					scsiFcodeBufferRX[0] = 'P';
					for (int i = 0; i < 5; i++)
						scsiFcodeBufferRX[1 + i] = st[i];
					scsiFcodeBufferRX[6] = 0x0D;
				}
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
				/* No reply: the real player returns its firmware revision
				   here and 'X' is not documented for it - answering either
				   would be inventing a version number. */
				FCdebugString_P(PSTR(" = Revision level request\r\n"));
				break;

				/* BeebSCSI extensions for the disc menu: ?T / ?Y return the
				   mounted disc's Title / Description (from the scsi0.cfg
				   parsed at mount) as 'T<text>' / 'Y<text>', or 'X' when
				   absent - the menu jukeboxes to each directory, remounts
				   and queries. */
				/* ?V - disc type for the menu scan: 'V1' = full disc
				   (scsi0.dat in the jukeboxed directory), 'V0' = video-only
				   volume (video.pvf, no image), 'X' = no disc. Replaces the
				   menu's OSWORD sector-0 probe, which needed a LUN start and
				   trusted VFS's error reporting. */
				case 'V':
				{
					uint8_t dir;
					uint8_t type = fcodeDirParam(&dir) ? filesystemVFSDirType(dir) : 0u;
					if (type) {
						scsiFcodeBufferRX[0] = 'V';
						scsiFcodeBufferRX[1] = (type == 2u) ? '1' : '0';
					} else {
						scsiFcodeBufferRX[0] = 'X';
						scsiFcodeBufferRX[1] = 0x0D;
					}
					scsiFcodeBufferRX[2] = 0x0D;
				}
				break;

				case 'T':
				case 'Y':
				{
					char text[240];
					enum parserkeyvalueenum key =
					        (scsiFcodeBuffer[1] == 'T') ? TITLE : DESCRIPTION;
					uint8_t dir = 0;
					bool haveDir = fcodeDirParam(&dir);
					bool ok = haveDir &&
					          filesystemReadVFSCfgTextDir(dir, key, text, sizeof(text));
					/* A video-only volume (video.pvf, no scsi0.dat/cfg) still
					   exists: reply 'T'/'Y' with empty text so the menu can
					   tell "video-only disc" from "no disc" ('X'). Only worth
					   the two f_stats when the cfg gave us nothing - an
					   f_stat is the LFN directory walk we are avoiding. */
					if (!ok && haveDir && filesystemVFSDirType(dir) != 0u) {
						text[0] = '\0';
						ok = true;
					}
					if (ok) {
						scsiFcodeBufferRX[0] = scsiFcodeBuffer[1];
						uint16_t n = 0;
						while (n < 240 && text[n]) {
							scsiFcodeBufferRX[1 + n] = (uint8_t)text[n];
							n++;
						}
						scsiFcodeBufferRX[1 + n] = 0x0D;
					} else {
						scsiFcodeBufferRX[0] = 'X';
						scsiFcodeBufferRX[1] = 0x0D;
					}
				}
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
				videoplayer_audio_enable(0, false);
				break;

				case '1':
				FCdebugString_P(PSTR(" = Audio-1 on\r\n"));
				videoplayer_audio_enable(0, true);
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
				videoplayer_audio_enable(1, false);
				break;

				case '1':
				FCdebugString_P(PSTR(" = Audio-2 on\r\n"));
				videoplayer_audio_enable(1, true);
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
				videoplayer_show_picture_number(false);
				break;

				case '1':
				FCdebugString_P(PSTR(" = Picture number/time code display on\r\n"));
				videoplayer_show_picture_number(true);
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
				videoplayer_set_video(false);
				break;

				case '1':
				FCdebugString_P(PSTR(" = Video on\r\n"));
				/* Same rule as the VP modes: with no video open (data-only
				   side) the plane stays off - E1 on a stale VideoCore buffer
				   painted noise over the whole screen. The player's first
				   real frame enables the plane. */
				videoplayer_set_video(true);
				break;

				default:
				FCdebugString_P(PSTR(" = Video (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x46: // F
			{
				uint32_t pictureNumber = 0;
				FCdebugString_P(PSTR(" = Load/Goto picture number : "));

				for (byteCounter = 0; byteCounter < 8; byteCounter++) {
					if (scsiFcodeBuffer[byteCounter] == 0x0D) break;
					if ( (char)scsiFcodeBuffer[byteCounter] >= '0' && (char)scsiFcodeBuffer[byteCounter] <= '9') {
						pictureNumber = (uint32_t) ((uint32_t)(pictureNumber * 10) + ((uint32_t)(scsiFcodeBuffer[byteCounter] - '0')));
					}
				}
				if (byteCounter == 0 || byteCounter >= 8) break;

				scsiFcodeBuffer[byteCounter] = 0;
				FCdebugStringInt32_P(PSTR(""), pictureNumber, false);
				FCdebugString_P(PSTR(" op: "));
				FCdebugString_P(PSTR((char *)&scsiFcodeBuffer[byteCounter-1]));

				switch(scsiFcodeBuffer[byteCounter-1]) {
					case 'I':
					FCdebugString_P(PSTR(" = Load Picture Info register\r\n"));
					videoplayer_goto(pictureNumber, 'I');
					scsiFcodeBufferRX[0] = 'A';
					scsiFcodeBufferRX[1] = '3'; // when passed
					break;

					case 'S':
					FCdebugString_P(PSTR(" = Stop Register\r\n"));
					videoplayer_goto(pictureNumber, 'S');
					/* A2 is owed when the picture is REACHED, not now. */
					fcode_ack_when(FC_ACK_STOP, 'A', '2');
					break;

					case 'R':
					FCdebugString_P(PSTR(" = Still picture\r\n"));
					videoplayer_goto(pictureNumber, 'R');
					scsiFcodeBufferRX[0] = 'A';
					scsiFcodeBufferRX[1] = '0';

					break;

					case 'N':
					FCdebugString_P(PSTR(" = Goto Picture and play normally\r\n"));
					videoplayer_goto(pictureNumber, 'N');
					scsiFcodeBufferRX[0] = 'A';
					scsiFcodeBufferRX[1] = '1'; // when complete

					break;

					case 'Q':
					FCdebugString_P(PSTR(" = Goto Picture and play in previous mode\r\n"));
					videoplayer_goto(pictureNumber, 'Q');
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
				fc_rc_computer = false;
				break;

				case '1':
				FCdebugString_P(PSTR(" = Remote control routed to computer\r\n"));
				fc_rc_computer = true;
				break;

				default:
				FCdebugString_P(PSTR(" = Remote control routed (Invalid parameter)\r\n"));
				break;
			}

			break;

			case 0x49: // I // Domesday sends this
			switch(scsiFcodeBuffer[1]) {
				case '0':
				/* No reply: the command set lists none for I0/I1. */
				FCdebugString_P(PSTR(" = Local front panel buttons disabled\r\n"));
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
				/* No reply: the command set lists none for J0/J1. */
				FCdebugString_P(PSTR(" = Remote control disabled for player control\r\n"));
				fc_remote_on = false;
				break;

				case '1':
				FCdebugString_P(PSTR(" = Remote control enabled for player control\r\n"));
				fc_remote_on = true;
				break;

				default:
				FCdebugString_P(PSTR(" = Remote control for player control (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x4C: // L // VFS sends this
			FCdebugString_P(PSTR(" = Still forward\r\n"));
			videoplayer_step(1);
			break;

			case 0x4D: // M // VFS sends this
			FCdebugString_P(PSTR(" = Still reverse\r\n"));
			videoplayer_step(-1);
			break;

			case 0x4E: // N // VFS sends this
			FCdebugString_P(PSTR(" = Play forward\r\n"));
			videoplayer_play_fwd();
			break;

			case 0x4F: // O // VFS sends this
			FCdebugString_P(PSTR(" = Play reverse\r\n"));
			videoplayer_play_rev();
			break;

			case 0x51: // Q // VFS sends this
			FCdebugString_P(PSTR(" = Goto chapter and halt/play\r\n"));
			break;

			case 0x52: // R // VFS sends this
			FCdebugString_P(PSTR(" = Slow/Fast read\r\n"));
			break;

			case 0x53: // S // VFS sends this
			FCdebugString_P(PSTR(" = Set fast/slow speed value\r\n"));
			{
				/* SxxxF = fast register (2..40, xxx/2 x normal),
				   SxxxS = slow register (2..250, 2/xxx x normal).
				   Anything else after the digits is not this command -
				   a malformed value must not reprogram a register. */
				uint32_t v = 0;
				for (byteCounter = 1; byteCounter < 5; byteCounter++) {
					char c = (char)scsiFcodeBuffer[byteCounter];
					if (c < '0' || c > '9') break;
					v = v * 10u + (uint32_t)(c - '0');
				}
				uint8_t term = scsiFcodeBuffer[byteCounter];
				if (v && (term == 'F' || term == 'S'))
					videoplayer_speed(v, term == 'F');
				else
					FCdebugString_P(PSTR(" (Invalid parameter)\r\n"));
			}
			break;

			case 0x54: // T
			FCdebugString_P(PSTR(" = Goto/Load time code register\r\n"));
			break;

			case 0x55: // U // VFS sends this
			FCdebugString_P(PSTR(" = Slow motion forward\r\n"));
			videoplayer_slow_fwd();
			break;

			case 0x56: // V, VP // VFS sends this
			switch(scsiFcodeBuffer[1]) {
				case 'P':
				VPmode = scsiFcodeBuffer[2];
				switch(scsiFcodeBuffer[2]) {
					/* Layer visibility goes through screen_plane_gate(), the
					   mixer-level hide: the framebuffer, mouseredirect and the
					   player keep OWNING their planes via screen_plane_enable
					   (a MODE change or a pointer move re-asserts "wanted"),
					   but a gated layer stays hidden regardless - on the real
					   AIV the pointer is drawn INTO the computer RGB, so it is
					   gated and mixed exactly like the screen, while keeping
					   its per-pixel key so it never gains an opaque surround. */
					case '1':
					screen_dim_strips(false);
					FCdebugString_P(PSTR(" = Video overlay mode 1 (LaserVision video only)\r\n"));
					screen_set_highlight(false);
					/* No video open (data-only side): keep the plane off so the
					   screen is black, not a stale buffer. The player's first
					   real frame enables it. */
					screen_plane_enable(0, videoplayer_active());
					screen_plane_gate(0, false);
					screen_plane_gate(1, true);
					screen_plane_gate(2, true);
					screen_plane_alpha(1, 0xFF);
					break;

					case '2':
					screen_dim_strips(false);
					FCdebugString_P(PSTR(" = Video overlay mode 2 (External (computer) RGB only)\r\n"));
					screen_set_highlight(false);
					screen_plane_gate(0, true);
					screen_plane_gate(1, false);
					screen_plane_gate(2, false);
					screen_plane_enable(1, true);
					/* the pointer stays KEYED here: it composites onto the
					   screen, and an opaque surround would paint a box */
					screen_plane_treatment(1, 3, 0xFF);
					screen_plane_treatment(2, 6, 0xFF);
					break;

					case '3':
					screen_dim_strips(false);
					FCdebugString_P(PSTR(" = Video overlay mode 3 (Hard-keyed)\r\n"));
					screen_set_highlight(false);
					screen_plane_enable(0, videoplayer_active());
					screen_plane_gate(0, false);
					screen_plane_gate(1, false);
					screen_plane_gate(2, false);
					screen_plane_enable(1, true);
					screen_plane_treatment(1, 2, 0xFF);
					screen_plane_treatment(2, 6, 0xFF);
					break;

					case '4':
					screen_dim_strips(false);
					/* *VOTRANSPARENT: the computer's BLACK is fully
					   transparent - it must not dim the video at all - and
					   every other colour is see-through, mixed over it.
					   So: keyed palette (black = alpha 0) + fixed alpha,
					   which is the HVS fixed-nonzero mode - the fixed alpha
					   applies only where the palette alpha is non-zero, so
					   black stays clear and graphics mix at alpha/255.
					   NOT the all-opaque bank: that mixes black too and
					   dims the picture inside the computer's rectangle,
					   which is visible as a box against the surround.
					   The pointer mixes at the same level and is keyed for
					   the same reason (its surround is an overlay artifact,
					   not screen content - an opaque mix draws a grey box). */
					FCdebugString_P(PSTR(" = Video overlay mode 4 (Transparent - both mixed)\r\n"));
					screen_set_highlight(false);
					/* The mix is in the palette (premultiplied), not in the
					   plane's fixed-alpha stage, so the scaler interpolates
					   correct data and glyph edges get no dark outline. The
					   pointer shares the bank for the same reason. */
					screen_plane_enable(0, videoplayer_active());
					screen_plane_gate(0, false);
					screen_plane_gate(1, false);
					screen_plane_gate(2, false);
					screen_plane_enable(1, true);
					/* alpha comes from the palette, so the plane runs at 0xFF */
					screen_plane_treatment(1, 5, 0xFF);
					screen_plane_treatment(2, 5, 0xFF);
					break;

					case '5':
					/* *VOHIGHLIGHT (AIV User Guide p.33): dim the player's
					   picture except where the computer's image is non-black.
					   Keyed palette inverted (black = half-opaque, colours =
					   transparent), per-pixel alpha - the graphic is a stencil
					   that spotlights the video, not a layer drawn over it.
					   The pointer sits on the same inverted bank, so its glyph
					   is a brightup window too (its black surround adds a
					   small extra dim patch - accepted artifact). */
					FCdebugString_P(PSTR(" = Video overlay mode 5 (Highlight - LaserVision enhanced by computer)\r\n"));
					screen_set_highlight(true);
					/* dim the band outside the computer's raster too - out
					   there the computer signal is blanking, i.e. black */
					screen_dim_strips(true);
					screen_plane_enable(0, videoplayer_active());
					screen_plane_gate(0, false);
					screen_plane_gate(1, false);
					screen_plane_gate(2, false);
					screen_plane_enable(1, true);
					screen_plane_treatment(1, 2, 0xFF);
					screen_plane_treatment(2, 6, 0xFF);
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
				videoplayer_slow_rev();
				break;

				default:
				FCdebugString_P(PSTR(" = Slow motion reverse (Invalid parameter)\r\n"));
				break;
			}
			break;

			case 0x57: // W // VFS sends this
			FCdebugString_P(PSTR(" = Fast forward\r\n"));
			videoplayer_fast_fwd();
			break;

			case 0x58: // X
			FCdebugString_P(PSTR(" = Clear\r\n"));
			videoplayer_clear();
			break;

			case 0x5A: // Z // VFS sends this
			FCdebugString_P(PSTR(" = Fast reverse\r\n"));
			videoplayer_fast_rev();
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
				fc_teletext_on = false;
				break;

				case '1':
				FCdebugString_P(PSTR(" = Teletext from disc on\r\n"));
				fc_teletext_on = true;
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

	// Send the F-Code to the serial UART - upstream's AVR behaviour, compiled
	// out with a named gate rather than deleted so future diffs stay clean.
#define BEEBSCSI_FCODE_TO_UART 0
#if BEEBSCSI_FCODE_TO_UART
	printf("<FCODE>");
	for (byteCounter = 0; byteCounter < fcodeLength; byteCounter++)
		printf("%c", scsiFcodeBuffer[byteCounter]);
	printf("</FCODE>\r\n");
#endif

	fcode_hist_record();   /* tx and rx are both final here */
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