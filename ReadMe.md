# Raspberry Pi to Beeb 1MHz interface

[![Codacy Badge](https://api.codacy.com/project/badge/Grade/ebe2e1bd0b1c42719c0a7ea5bec9bed2)](https://app.codacy.com/app/dominic.plunkett/Pi1MHz?utm_source=github.com&utm_medium=referral&utm_content=dp111/Pi1MHz&utm_campaign=Badge_Grade_Settings)

This project enables a Raspberry Pi to emulate a number of existing Beeb peripherals plus some new ones:

* ADFS Harddisc
* Music 5000 / 3000
* Expansion RAM (480Mbytes for a PiZero)

A simple Level shifter is required to connect the 1MHz port to the Beeb. A PiZero and level shifter can sit under a Beeb or Master.

PiZero and PiZeroW are treated as the same. Pi3B+ is also supported via a cable as it won't fit under the computer. Pi3A+ should also work but hasn't been tested.

## Setting up

You will need a PiZero or Pi3B+ with a cable, SD-CARD and level shifter and some means of powering the Pi ( a bit of wire from the Beeb)

Copy the contents of the firmware directory to the root of your SDCARD. If you want a prepared ADFS Harddisc you can copy <https://www.domesday86.com/wp-content/uploads/2019/03/BeebSCSI_Quickstart_LUN_2_5.zip> to the root of the SD-CARD otherwise you can find out more details on creating an ADFS LUN at : <https://www.domesday86.com/?page_id=400>

Insert the SD-CARD into the Pi. Attach the level shifter to the Pi and insert into the 1MHz bus socket of the Beeb. Take extra care to ensure that it is connected correctly. You will also need to take +5v from somewhere to power the Pi, this can be the user port or Tube for example.

To check the build information type P.$&FD00

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

Page mode uses the registers &FCFD &FCFE FCFF to select the page for FD00--FDFF

The first page of JIM ram is preloaded with build information. This can be accessed by doing PRINT $&FD00. This is RAM so can easily be over written.

If a file called "JIM_Init.bin" exists it will be loaded starting at the beginning of JIM on wards ( NB over writes build info). This enables future very large programs which, with clever programming could all run in JIM RAM.

## Internal status and control

&FCCA selects the command/status address
&FCCB is the return status / command write.

Addresses currently defined

* &00 : Read only : JIM RAM size in 16Mbyte steps

## cmdline.txt options

* LED override : depending on the pi use either bcm2708.disk_led_gpio=xx or bcm2709.disk_led_gpio=xx where xx is the pi GPIO number
* M5000_BeebAudio_Off=1 to turn off Audio out of the Beeb and enable stereo on the headphone jack of Pi3B+
* M5000_Gain=xxxx : Over rides default gain of 16. Add 1000 to disable auto scaling as well. Auto scaling reduces the gain if the signal clips
* Ram_addr=0xYY : set the base address of the byte write ram registers , -1 to disable
* Harddisc_addr=0xYY : set the base address of the harddisc registers , -1 to disable
* M5000_addr=-1 : disables the M5000 emulator
* Framebuffer_addr=0xYY : set the base address of the frame buffer registers , -1 to disable

## Making the code

You will need a Linux command prompt. Under windows10 I use  windows bash shell. You will also need the arm dev tools : <https://developer.arm.com/-/media/Files/downloads/gnu-a/9.2-2019.12/binrel/gcc-arm-9.2-2019.12-x86_64-arm-none-eabi.tar.xz> . in the src/scripts directory you can select the platform you'd like to build for by executing the configure_rpi.sh ( for PiZero) and configure_rpi3.sh ( for rpi3B+). Then just use make -j4. Copy the firmware directory to the root of your SD-CARD. Serial debug can be enabled using the configure scripts. Or the complete system can be built in one go (PiZero, RPI3 both normal and debug) by using the release.sh script.

The current PCB is too small to have a serial debug connector fitted. I fitted a 3 pin sil header to the underside of my PiZero ( 0v TX TX)

## Donations Welcome

Donations to development of Projects are welcome, especially if you are make a profit from using the project e.g. selling boards or kits.

## License

    Pi1MHz is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Pi1MHz is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Pi1MHz.  If not, see <http://www.gnu.org/licenses/>.
