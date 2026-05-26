/*
 Emulates Harddisc using beebSCSI
 This file combines the verilog file the hostadaptor.c and the interface to Pi1MHz
 NB the inversions have been optimized out

*/
#define HD_ADDR 0x40

#include <string.h>
#include "Pi1MHz.h"

#define FILE __FILE

#include <stdbool.h>
#include <stdio.h>

// includes from the beebSCSI directory
#include "BeebSCSI/debug.h"
#include "BeebSCSI/filesystem.h"
#include "BeebSCSI/hostadapter.h"
#include "BeebSCSI/scsi.h"

volatile bool HD_ACK;
volatile uint8_t HD_DATA;

volatile bool HD_SEL;
bool HD_IRQ_ENABLE;

#define ACK_CLEAR 0
#define ACK_SET   1

#define CLEAR     0
#define ACTIVE    1

#define HD_STATUS (Pi1MHz_Memory[HD_ADDR+1])

#define STATUS_CND (1<<7)
#define STATUS_INO (1<<6)
#define STATUS_REQ (1<<5)
#define STATUS_IRQ (1<<4)
#define STATUS_3   (1<<3)
#define STATUS_2   (1<<2)
#define STATUS_BSY (1<<1)
#define STATUS_MSG (1<<0)

void hd_emulator_write_data(unsigned int gpio)
{
   HD_DATA = GET_DATA(gpio);
   Pi1MHz_Memory[HD_ADDR] = GET_DATA(gpio);
   HD_ACK = ACTIVE;
}

void hd_emulator_read_data(unsigned int gpio __attribute__((unused)))
{
   HD_ACK = ACTIVE;
}

void hd_emulator_nSEL(unsigned int gpio)
{
   HD_DATA = GET_DATA(gpio);
   HD_SEL = ACTIVE;
}

static void hd_emulator_status(unsigned int bit, bool state)
{
   if (state == CLEAR)
      HD_STATUS &= ~bit;
   else
      HD_STATUS |= bit;
}

static void hd_emulator_IRQ(unsigned int gpio)
{
   unsigned int data = GET_DATA(gpio);
   // D0 used to enable / disable
   HD_IRQ_ENABLE = ( data & 1 );

   if (!HD_IRQ_ENABLE)
   {
      Pi1MHz_SetnIRQ(CLEAR_IRQ);
      hd_emulator_status(STATUS_IRQ, CLEAR);
   } else
   {
      if (HD_STATUS & STATUS_REQ)
      {
         Pi1MHz_SetnIRQ(ASSERT_IRQ);
         hd_emulator_status(STATUS_IRQ, ACTIVE);
      }
   }

}
#ifdef DEBUG
static void hd_emulator_conf(unsigned int gpio)
{
   unsigned int databusValue = GET_DATA(gpio);

   // Read the databus value (containing the configuration command)
   // Don't invert the databus if we are connected to the internal bus
   //if (hostadapterConnectedToExternalBus()) databusValue = ~databusValue;

   // All debug off (Command 0)
   if (databusValue == 0)
   {
      debugFlag_filesystem = false;
      debugFlag_scsiCommands = false;
      debugFlag_scsiBlocks = false;
      debugFlag_scsiFcodes = false;
      debugFlag_scsiState = false;
      debugFlag_fatfs = false;
   }

   // All debug on (Command 1)
   if (databusValue == 1)
   {
      debugFlag_filesystem = true;
      debugFlag_scsiCommands = true;
      debugFlag_scsiBlocks = false;
      debugFlag_scsiFcodes = true;
      debugFlag_scsiState = true;
      debugFlag_fatfs = true;
   }

   // File system debug on/off (Command 10/11)
   if (databusValue == 10) debugFlag_filesystem = true;
   if (databusValue == 11) debugFlag_filesystem = false;

   // SCSI commands debug on/off (Command 12/13)
   if (databusValue == 12) debugFlag_scsiCommands = true;
   if (databusValue == 13) debugFlag_scsiCommands = false;

   // SCSI blocks debug on/off (Command 14/15)
   if (databusValue == 14) debugFlag_scsiBlocks = true;
   if (databusValue == 15) debugFlag_scsiBlocks = false;

   // SCSI F-codes debug on/off (Command 16/17)
   if (databusValue == 16) debugFlag_scsiFcodes = true;
   if (databusValue == 17) debugFlag_scsiFcodes = false;

   // SCSI state debug on/off (Command 18/19)
   if (databusValue == 18) debugFlag_scsiState = true;
   if (databusValue == 19) debugFlag_scsiState = false;

   // FAT FS debug on/off (Command 20/21)
   if (databusValue == 20) debugFlag_fatfs = true;
   if (databusValue == 21) debugFlag_fatfs = false;
}
#endif

void harddisc_emulator_init( uint8_t instance )
{
   static int PowerOn = 0 ;
   // Turn off all host adapter signals
   hd_emulator_status(STATUS_MSG | STATUS_BSY | STATUS_REQ | STATUS_INO | STATUS_3 | STATUS_2 | STATUS_CND | STATUS_IRQ, CLEAR);

   HD_ACK = CLEAR;
   HD_SEL = CLEAR;
   HD_IRQ_ENABLE = CLEAR;

   // register call backs
   // address FC40 read  = Read SCSI databus command
   Pi1MHz_Register_Memory(READ_FRED,  HD_ADDR, hd_emulator_read_data );
   // status doesn't need a call back
   // address FC41 read  = Read SCSI status byte command
   // address FC40 write = Write SCSI databus command
   Pi1MHz_Register_Memory(WRITE_FRED, HD_ADDR, hd_emulator_write_data );
   // address FC42 write = Assert SCSI nSEL command
   Pi1MHz_Register_Memory(WRITE_FRED, HD_ADDR+2, hd_emulator_nSEL     );
   // address FC43 write = Enable/Disable BBC nIRQ command
   Pi1MHz_Register_Memory(WRITE_FRED, HD_ADDR+3, hd_emulator_IRQ );
   // address FC44 write = Write BeebSCSI configuration byte
#ifdef DEBUG
   Pi1MHz_Register_Memory(WRITE_FRED, HD_ADDR+4, hd_emulator_conf );
#endif
   // Initialise but only at power on
   // Fixes *SCSIJUKE surving over shift break.
   if (!PowerOn)
   {
      // Initialise the SD Card and FAT file system functions
       filesystemInitialise();
       // Initialise the SCSI emulation
      scsiInitialise();

      PowerOn = 1;
   }

   // register polling function
   Pi1MHz_Register_Poll(scsiProcessEmulation);
}


/************************************************************************
   hostadapter.c

   BeebSCSI Acorn host adapter functions
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

   This code has been changed to work with Pi1MHz

************************************************************************/

// Local includes
//#include "uart.h"
//#include "debug.h"
//#include "hostadapter.h"

// Timeout counter (used when interrupts are not available to ensure
// DMA read and writes do not hang the AVR waiting for host response
// Note: This is an unsigned 32 bit integer and should therefore be
// smaller than 4,294,967,295
#define TOC_MAX 1000000000

// Databus manipulation functions -------------------------------------------------------

// Read a byte from the databus (directly)
uint8_t hostadapterReadDatabus(void)
{
   return HD_DATA;
}

// SCSI Bus action functions ------------------------------------------------------------

// Function to read a byte from the host (using REQ/ACK)
uint8_t hostadapterReadByte(void)
{
   // Set the REQuest signal
   hostadapterWriteRequestFlag(ACTIVE);

   // Wait for ACKnowledge
   while ((HD_ACK == CLEAR) && !Pi1MHz_is_rst_active());

   hostadapterWriteRequestFlag(CLEAR);

   return HD_DATA;
}

// Function to write a byte to the host (using REQ/ACK)
void hostadapterWriteByte(uint8_t databusValue)
{
   // Write the byte of data to the databus
   Pi1MHz_Memory[HD_ADDR] = databusValue;

   // Set the REQuest signal
   hostadapterWriteRequestFlag(ACTIVE);

   // Wait for ACKnowledge
   while ((HD_ACK == CLEAR) && !Pi1MHz_is_rst_active());

   // Clear the REQuest signal
   hostadapterWriteRequestFlag(CLEAR);
}

// Host DMA transfer functions ----------------------------------------------------------

// Host reads data from SCSI device using DMA transfer (reads a 256 byte block)
// Returns number of bytes transferred (for debug in case of DMA failure)
uint16_t hostadapterPerformReadDMA(const uint8_t *dataBuffer)
{
   uint32_t currentByte = 0;

   // Loop to write bytes (unless a reset condition is detected)

   do {
      // Write the current byte to the databus and point to the next byte
      Pi1MHz_Memory[HD_ADDR] = dataBuffer[currentByte++];

      // Set the REQuest signal
      hostadapterWriteRequestFlag(ACTIVE);

      // Wait for ACKnowledge
      uint32_t timeoutCounter = 0; // Reset timeout counter

      while ((HD_ACK == CLEAR) && !Pi1MHz_is_rst_active())
      {
         if (++timeoutCounter == TOC_MAX)
         {
            return currentByte - 1;
         }
      }

      // Clear the REQuest signal
      hostadapterWriteRequestFlag(CLEAR);
   } while (currentByte < 256 );

   return currentByte - 1;
}

// Host writes data to SCSI device using DMA transfer (writes a 256 byte block)
// Returns number of bytes transferred (for debug in case of DMA failure)
uint16_t hostadapterPerformWriteDMA(uint8_t *dataBuffer)
{
   uint32_t currentByte = 0;

   // Loop to read bytes (unless a reset condition is detected)

   do {
      // Set the REQuest signal
      hostadapterWriteRequestFlag(ACTIVE);

      // Wait for ACKnowledge
      uint32_t timeoutCounter = 0; // Reset timeout counter

      while ((HD_ACK == CLEAR) && !Pi1MHz_is_rst_active())
      {
         if (++timeoutCounter == TOC_MAX)
         {
            return currentByte;
         }
      }

      // Read the current byte from the databus and point to the next byte
      dataBuffer[currentByte++] = HD_DATA;

      // Clear the REQuest signal
      hostadapterWriteRequestFlag(CLEAR);
   } while (currentByte < 256 );

   return currentByte - 1;
}

// Host adapter signal control and detection functions ------------------------------------

// Function to determine if the host adapter is connected to the external or internal
// host bus
bool hostadapterConnectedToExternalBus(void)
{
   //if ((INTNEXT_PIN & INTNEXT) != 0) return false; // Internal bus
   return true; // External bus
}

// Function to write the host reset flag
void hostadapterWriteResetFlag(bool flagState __attribute__((unused)))
{

}

// Function to return the state of the host reset flag
bool hostadapterReadResetFlag(void)
{
   return Pi1MHz_is_rst_active();
}

// Function to write the data phase flags and control databus direction
// Note: all SCSI signals are inverted logic
void hostadapterWriteDataPhaseFlags(bool message, bool commandNotData, bool inputNotOutput)
{
   hd_emulator_status(STATUS_MSG, message);
   hd_emulator_status(STATUS_CND, commandNotData);
   hd_emulator_status(STATUS_INO, inputNotOutput);
}

// Function to write the host busy flag
// Note: all SCSI signals are inverted logic
void hostadapterWriteBusyFlag(bool flagState)
{
   hd_emulator_status(STATUS_BSY, flagState); // BSY = active

   if (flagState) HD_SEL = CLEAR;
}

// Function to write the host request flag
// Note: all SCSI signals are inverted logic
void hostadapterWriteRequestFlag(bool flagState)
{
   if (flagState==CLEAR)
   {
      // Clear the REQuest signal
      hd_emulator_status(STATUS_REQ, CLEAR); // REQ = 1 (inactive)
   }
   else
   {
      HD_ACK = CLEAR;
      hd_emulator_status(STATUS_REQ, ACTIVE); // REQ = 0 (active)
      if (HD_IRQ_ENABLE)
      {
         Pi1MHz_SetnIRQ(ASSERT_IRQ);
         hd_emulator_status(STATUS_IRQ, ACTIVE);
      }
   }
}

// Function to read the state of the host select flag
// Note: all SCSI signals are inverted logic
bool hostadapterReadSelectFlag(void)
{
   return HD_SEL;
}
