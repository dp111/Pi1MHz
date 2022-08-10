# Changes from BeebSCSI

These files have beeen taken from the BeebSCSI project. Fatfs has been updated to 0.14b.
host_interface.c has been significantly changed as it contains the equalivant of the CPLD as well, moved up a level and renamed to harddisdc_emaultor.c

Filesystem.c is largely unchanged except from remove AVR specific code.

The rest of this file is from BeebSCSI

## Synopsis

BeebSCSI_AVR is the AT90USB1287 firmware source code for the BeebSCSI project.

## Motivation

Originally Acorn provided a SCSI solution based on 3 individual parts: The Acorn SCSI host adapter, the Adaptec ACB-4000 SCSI adapter and a physical MFM hard disc (a ‘Winchester drive’). Later, as part of the Domesday project (in 1986), this was extended to include the AIV SCSI Host Adapter (designed to be connected internally to a BBC Master Turbo) and the Philips VP415 LaserVision laser disc player with SCSI-1 support.

BeebSCSI 7 is a credit-card sized board that provides a single-chip implementation of the host adapter board (both original and AIV) using a modern CPLD (Complex Programmable Logic Device). In addition, an AVR Microcontroller provides a complete SCSI-1 emulation including the vendor specific video control commands of the VP415.

Rather than using a physical hard drive, BeebSCSI uses a single Micro SD card to provide up to >64Gbytes of storage with support for either 4 (ADFS) or 8 (VFS) virtual hard drives (or ‘LUNs’) per card. In addition, in the BBC Master, two BeebSCSI devices can be attached, one internal and one external, providing 12 SCSI LUNs (hard drive images) simultaneously.

## Installation

Please see <http://www.domesday86.com> for detailed documentation on BeebSCSI

## Author

BeebSCSI is written and maintained by Simon Inns.

## License

    BeebSCSI is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    BeebSCSI is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with BeebSCSI.  If not, see <http://www.gnu.org/licenses/>.
