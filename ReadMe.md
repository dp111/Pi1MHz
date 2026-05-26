# Raspberry Pi to Beeb 1MHz interface

This project enables a Raspberry Pi to emulate a number of existing Beeb peripherals plus some new ones:

* ADFS Harddisc
* Music 5000 / 3000
* Expansion RAM (480Mbytes for a PiZero)

A simple Level shifter is required to connect the 1MHz port to the beeb. A Pizero and level shifter can sit under a Beeb or Master.

PiZero and PiZeroW are treated as the same. Pi3B+ is also supported via a cable as it won't fit under the computer. Pi3A+ should also work but hasn't been tested.

## setting up

You will need a PiZero or Pi3B+ with a cable, SDCARD and level shifter and some means of powering the Pi ( a bit of wire from the beeb)

Copy the contents of the firmware directory to the root of your SDCARD. If you want a prepared ADFS harddisc you can copy https://www.domesday86.com/wp-content/uploads/2019/03/BeebSCSI_Quickstart_LUN_2_5.zip to the root of the SDCARD otherwise you can find out more details on creating an ADFS LUN at : https://www.domesday86.com/?page_id=400

Insert the SDCARD into the pi. Attach the level shifter to the pi and insert into the 1MHz bus socket of the beeb. Take extra care to ensure that it is connected correctly. You will also need to take +5v from somewhere to power the pi, this can be the user port or Tube for example.

## ADFS Harddisc Emulation

ADFS harddisc emulation is based on BeebSCSI. For more information goto : https://www.domesday86.com/?page_id=400 .If you have a Master then you will have ADFS already in ROM. If you have a beeb you will need ADFS. It is possible if the computer boots very fast and the SDCARD is slow that the computer boots faster than the Pi in this case an extra CTRL-BREAK will be required.

Read speed appears to be about 100K/sec. 

## Music 5000 / 3000 Emulation

The emulation can play sounds through the computers internal speaker. If you are using the pi3B+ you can use the headphone jack. Currently if an ADFS access occurs while playing music the musics will be interrupted briefly. 

See the command.txt section for various configuration options 

## Expansion Ram Emulation

Two types of JIM expansion ram are supported:

* Byte mode : 16Mbytes http://www.sprow.co.uk/bbc/ramdisc.htm
* Page mode : 480Mbytes for Pizero, 992Mbytes RPI3B+

Byte mode uses the registers &FC02, &FC01, &FC00 to select the byte and FC03 to read and write the memory.

Page mode uses the registers &FCFD &FCFE FCFF to select the page for FD00--FDFF

The first page of JIM ram is preloaded with build information. This can be accessed by doing PRINT $FD00. This is RAM so can easily be over written.

If a file called "JIM_Init.bin" exists it will be loaded starting at the beginning of JIM onwards ( NB over writes build info). This enables future very large programs which, with clever programming could all run in JIM RAM.

## Internal status and control

&FCCA selects the command/status address
&FCCB is the return status / command write.

Addresses currently defined
* &00 : Readonly : JIM RAM size in 16Mbyte steps

## Command.txt options

*LED override : depending on the pi use either bcm2708.disk_led_gpio=xx or bcm2709.disk_led_gpio=xx where xx is the pi GPIO number
*M5000_BeebAudio_Off=1 to turn off Audio out of the Beeb and enable stereo on the headphone jack of Pi3B+
*M5000_Gain=xxxx : Over rides default gain of 16. Add 1000 to disable auto scaling as well. Auto scaling reduces the gain if the signal clips 
