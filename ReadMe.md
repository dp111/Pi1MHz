# Pi1MHz

[![Codacy Badge](https://api.codacy.com/project/badge/Grade/ebe2e1bd0b1c42719c0a7ea5bec9bed2)](https://app.codacy.com/app/dominic.plunkett/Pi1MHz?utm_source=github.com&utm_medium=referral&utm_content=dp111/Pi1MHz&utm_campaign=Badge_Grade_Settings)

Pi1MHz is a Raspberry Pi bare-metal interface for the BBC Micro/Master 1MHz bus.

It emulates:

- ADFS hard disk (based on BeebSCSI)
- Hybrid Music 5000/3000 functionality
- Large JIM RAM expansion
- FAT/SD transfer services for host software

Supported hardware (project-tested):

- Raspberry Pi Zero / Zero W
- Raspberry Pi Zero2W / 3B+

Pi 3A+ is expected to work but is not formally tested.

## Hardware Requirements

- BBC Micro or BBC Master with 1MHz bus access
- Raspberry Pi Zero/Zero W or Pi 3B+
- SD card
- 1MHz bus level shifter
- 5V power feed for the Pi (for example from User Port or Tube)

## Quick Start

1. Copy the contents of firmware/ to the root of the SD card.
2. Optionally add a prepared BeebSCSI image set:
   - https://www.domesday86.com/wp-content/uploads/2019/03/BeebSCSI_Quickstart_LUN_2_5.zip
3. Insert SD card into the Pi.
4. Connect the level shifter and 1MHz bus interface carefully.
5. Power the Pi and boot the BBC.

On fast beeb boot plus slow SD cards, an extra CTRL-BREAK may be required.

To check the build information and get help type one of the following:

X%=0 :CALL &FC88
*fx147,136,0 : *GO &FD00
*fx147,136,0 : *GOIO &FD00

## ADFS Harddisc Emulation

ADFS Harddisc emulation is based on BeebSCSI. For more information goto : <https://www.domesday86.com/?page_id=400> . If you have a Master then you will have ADFS already in ROM. If you have a Beeb you will need ADFS. It is possible if the computer boots very fast and the SD-CARD is slow that the computer boots faster than the Pi in this case an extra CTRL-BREAK will be required.

Read speed appears to be about 100K/sec.

## Music 5000 / 3000 Emulation

The emulation can play sounds through the computers internal speaker. If you are using the pi3B+ you can use the headphone jack. Currently if an ADFS access occurs while playing music the music will be interrupted briefly.

See the cmdline.txt section for various configuration options

## Expansion Ram Emulation

Two types of JIM expansion ram are supported:

* Byte mode : 16Mbytes <http://www.sprow.co.uk/bbc/ramdisc.htm>
* Page mode : 480Mbytes for PiZero, 992Mbytes Pi3B+

Byte mode uses the registers &FC02, &FC01, &FC00 to select the byte and FC03 to read and write the memory.

Page mode uses the registers &FCFD &FCFE FCFF to select the page for &FD00--&FDFF.

The first page of JIM ram is preloaded with build information. This can be accessed by doing PRINT $&FD00. This is RAM so can easily be over written.

If a file called "JIM_Init.bin" exists it will be loaded starting at the beginning of JIM on wards ( NB over writes build info). This enables future very large programs which, with clever programming could all run in JIM RAM.

## SDCARD / Fat Access

A simplified access to the Pi's SDCARD is provided. This can be used to access local files and could for instance by used by mmfs2. A 16Mbyte buffer is provided that can be split up into various different ways. Multiple files maybe open at the same time , but they must be unique files. A 24 bit pointer is provided with autoincrement. Using this address space a file system e.g. MMFS may "cache" the entire drive and not need to raise PAGE. The buffer also hold the FAT command that is going to be executed.
It is suggested the first 4Mbytes be reserved for the currently active Filesystem. 8Mbyte to 14 Mbytes be reserved for the currently active program.

    Base address = &FCD6

    Base + 0 = lower 8bits of the 24bit address pointer
    Base + 1 = middle 8bits of the 24bit address pointer
    Base + 2 = top 8bits of the 24bits address pointer
    Base + 3 = data register ( with auto address increment)
    Base + 4 = command pointer (&F0-FF).
            command = &F0 points to a command at 0xFFF000 in the buffer
            command = &F1 points to a command at 0xFFF100 in the buffer
            ...
            command = &FF points to a command at 0xFFFF00 in the buffer

            Each command pointer can only be used for one FAT file/ directory
            When the command is complete the the command pointer returns success or and error code

            Suggested code
                    LDA # command
                    STA &FCDA
            .complete_check_loop
                    LDA &FCDA
                    BMI complete_check_loop
                    BNE command_error



 	FR_OK = 0,				/* (0) Succeeded */
	FR_DISK_ERR,			/* (1) A hard error occurred in the low level disk I/O layer */
	FR_INT_ERR,				/* (2) Assertion failed */
	FR_NOT_READY,			/* (3) The physical drive cannot work */
	FR_NO_FILE,				/* (4) Could not find the file */
	FR_NO_PATH,				/* (5) Could not find the path */
	FR_INVALID_NAME,		/* (6) The path name format is invalid */
	FR_DENIED,				/* (7) Access denied due to prohibited access or directory full */
	FR_EXIST,				/* (8) Access denied due to prohibited access */
	FR_INVALID_OBJECT,		/* (9) The file/directory object is invalid */
	FR_WRITE_PROTECTED,		/* (10) The physical drive is write protected */
	FR_INVALID_DRIVE,		/* (11) The logical drive number is invalid */
	FR_NOT_ENABLED,			/* (12) The volume has no work area */
	FR_NO_FILESYSTEM,		/* (13) There is no valid FAT volume */
	FR_MKFS_ABORTED,		/* (14) The f_mkfs() aborted due to any problem */
	FR_TIMEOUT,				/* (15) Could not get a grant to access the volume within defined period */
	FR_LOCKED,				/* (16) The operation is rejected according to the file sharing policy */
	FR_NOT_ENOUGH_CORE,		/* (17) LFN working buffer could not be allocated */
	FR_TOO_MANY_OPEN_FILES,	/* (18) Number of open files > FF_FS_LOCK */
	FR_INVALID_PARAMETER	/* (19) Given parameter is invalid */
                                (20) Short fread/fwrite


SDCARD / FAT commands are the first byte of the command buffer

0 = Read sector

    command pointer + 0 = 0
    command pointer + 1 = 0
    command pointer + 2 = 0
    command pointer + 3 = 0
    command pointer + 4,5,6,7 4 bytes of destination address in buffer NB top byte must be zero.
    command pointer + 8,9,10,11 4 bytes , start sector in LBA
    command pointer + 12,13,14,15 4 bytes , number of sectors to read

1 = Write sector

    command pointer + 0 = 1
    command pointer + 1 = 0
    command pointer + 2 = 0
    command pointer + 3 = 0
    command pointer + 4,5,6,7 4 bytes of source address in buffer NB top byte must be zero.
    command pointer + 8,9,10,11 4 bytes , start sector in LBA
    command pointer + 12,13,14,15 4 bytes , number of sectors to write

2 = fopen

    command pointer + 0 = 2
    command pointer + 1 = 0
    command pointer + 2 =
        #define	FA_READ				0x01
        #define	FA_WRITE			0x02
        #define	FA_OPEN_EXISTING	0x00
        #define	FA_CREATE_NEW		0x04
        #define	FA_CREATE_ALWAYS	0x08
        #define	FA_OPEN_ALWAYS		0x10
        #define	FA_OPEN_APPEND		0x30
    command pointer + 3 = filename zero terminated

3 = fclose

    command pointer + 0 = 3

4 = fread ( with implicit lseek)

    command pointer + 0 = 4
    command pointer + 1,2,3 3 bytes of length to read. Once complete this returns the actually number of byte read
                The command pointer register = 20 if the read was short
    command pointer + 4,5,6,7 4 bytes of destination address in buffer NB top byte must be zero.
    command pointer + 8,9,10,11 4 byte pointer within file to start the read from

5 = fwrite ( with implicit lseek , fsync)

    command pointer + 0 = 5
    command pointer + 1,2,3 3 bytes of length to write. Once complete this returns the actually number of byte read
                 The command pointer register = 20 if the write was short
    command pointer + 4,5,6,7 4 bytes of source address in buffer NB top byte must be zero.
    command pointer + 8,9,10,11 4 byte pointer within file to start the write from

6 = fsize

    command pointer + 0 = 6
    returns :
    command pointer + 8,9,10,11 4 bytes size of file

7 = fopendir

    command pointer + 0 = 7
    command pointer + 1 ...  = directory name zero terminated

8 = fclosedir

    command pointer + 0 = 8

9 = readdir

    each read returns the next entry in the directory until there are no more entries at which point it returns 20. If the entry is a file the size is returned, if it's a directory the size is invalid and should be ignored. The modification and creation dates and times are also returned as FAT format date and time which can be converted to a more standard format if needed. The attribute byte can be used to determine if the entry is a file or directory or other attributes. The long and short names are also returned. The long name should be used in preference to the short name if it exists.

    command pointer + 0 = 9
    command pointer + 4,5,6,7 4 bytes of destination address in buffer NB top byte must be zero.

    {return structure for each entry
    4 byte files size ( invalid for directory)
    2 byte modification date
    2 byte modification time
    2 byte Creation date
    2 byte Creation time
    1 byte attribute
    13 bytes alternate name ( zero terminated)
    256 bytes for long primary name ( zero terminated)
    }
    /* File attribute bits for directory entry (FILINFO.fattrib) */
    #define	AM_RDO	0x01	/* Read only */
    #define	AM_HID	0x02	/* Hidden */
    #define	AM_SYS	0x04	/* System */
    #define AM_DIR	0x10	/* Directory */
    #define AM_ARC	0x20	/* Archive */

    return type is FR_OK if read ok, 20 if no more files

10 = fmkdir

    command pointer + 0 = 10
    command pointer + 1 ... = directory name ( zero terminated)

11 = fchdir change directory

    command pointer + 0 = 11
    command pointer + 1 ... = directory name ( zero terminated)

    /* Change current directory of the current drive ("dir1" under root directory) */
    f_chdir("/dir1");

    /* Change current directory of the drive "flash" and set it as current drive (at Unix style volume ID) */
    f_chdir("/flash/dir1");

12 = frename

    command pointer + 0 = 12
    command pointer + 1 ... =  old name ( zero terminated)
    command pointer + ..newname ( zero terminated)

13 = fgetfree

    command pointer + 0 = 13

    return
    command pointer + 8,9,10,11 4 bytes freespace in bytes / 256

14 = fmount ( not sure how this works yet ) ( this could support swapping SDCARDs while running)

    command pointer + 0 = 14

15 = funmount ( not sure how this works yet )

    command pointer + 0 = 15

16 = funlnk ( delete a file or directory)

    command pointer + 0 = 16
    command pointer + 1 = filename zero terminated

20 = SDCARD type
    command pointer + 0 = 20
    return Base + 4 = 0 or 1 depending on SDCARD type

## Frame buffer

This is taken from PiTubeDirect but cutdown to support just the beeb fonts. writes to &FCA0 are directed to HDMI port. OSWRCH redirection can be enabled by calling a helper function.

## Helper function

There are a number helper functions built in. These are accessed by :

  ?&FC88 = function number : CALL &FD00
  or X% = function number : CALL &FC88

Helper functions include

* 0 help screen
* 1 Status ( not implemented yet )
* 2 Screen redirector to HDMI port
* 3 SRLOAD ADFS rom
* 4 SRLOAD MMFS rom
* 5 SRLOAD MMFS2 rom


## Internal status and control

&FCCA selects the command/status address
&FCCB is the return status / command write.

Addresses currently defined

* &00 : Read only : JIM RAM size in 16Mbyte steps

## cmdline.txt options

* LED override : depending on the pi use either bcm2708.disk_led_gpio=xx or bcm2709.disk_led_gpio=xx where xx is the pi GPIO number
* M5000_BeebAudio_Off=1 to turn off Audio out of the Beeb and enable stereo on the headphone jack of Pi3B+
* M5000_Gain=xxxx : Over rides default gain of 16. Add 1000 to disable auto scaling as well. Auto scaling reduces the gain if the signal clips
* Rampage_addr=0xYY : set the base address of the page write ram registers default &FD, -1 to disable
* Rambyte_addr=0xYY : set the base address of the byte write ram registers default &00, -1 to disable
* Harddisc_addr=0xYY : set the base address of the harddisc registers default &40, -1 to disable
* M5000_addr=-1 : disables the M5000 emulator
* Framebuffer_addr=0xYY : set the base address of the frame buffer registers default &A0, -1 to disable
* Discaccess_addr=0xYY : set the base address of the discaccess registers default &A6, -1 to disable
* Helpers_addr=0xYY : set the base address of the helpers registers default &88, -1 to disable
* Pi1MHZnOE=0 : Disables external nOE pin on the buffers,  =1 supports multiple devices on the 1MHz bus
* SCSIID=xx : Set the SCSI ID of the ADFS/VFS emulation. 0 is default to listens to every id
* SCSIJUKE=xx : sets the default SCSI jukebox. 0 is default.
* VFSJUKE=xx : sets the default VFS jukebox. 0 is default.

## Notes

- PCB space is limited for a dedicated serial debug connector on some builds.
- A custom 3-pin header (0V, TX, RX) can be fitted under a Pi Zero.

## Donations

Donations are welcome, especially from commercial kit/board sellers using this project.

## SAST Tools

- [PVS-Studio](https://pvs-studio.com/en/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source): static analyzer for C, C++, C#, and Java.

## License

Pi1MHz is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Pi1MHz is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with Pi1MHz. If not, see http://www.gnu.org/licenses/.
