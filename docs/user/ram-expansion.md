# RAM Expansion (JIM)

Pi1MHz gives the Beeb an enormous RAM expansion through the JIM page
(`&FD00-&FDFF`), in two flavours at once. How much you get depends on
the Pi: the Pi allocates all the memory it does not need itself, in
16-megabyte units - up to around 480MB on a Pi Zero and around 992MB on
a Pi 3B+.

This is the same paged-RAM scheme other Beeb expansions use, so
software written for those can work with Pi1MHz.

## Byte mode (16MB, Sprow-compatible)

Compatible with the scheme used by Sprow's RAM disc
(<http://www.sprow.co.uk/bbc/ramdisc.htm>):

- Write the address, one byte at a time, to `&FC00` (low), `&FC01`
  (middle), `&FC02` (high).
- Read or write the data byte at `&FC03`.

Byte mode reaches the first 16MB of the RAM (the same 16MB as page
mode's lowest pages).

## Page mode (the whole lot)

- Write a page number to `&FCFF` (low), `&FCFE` (middle), `&FCFD`
  (high). Each page is 256 bytes.
- That page of expansion RAM then appears in JIM, at `&FD00-&FDFF`,
  for ordinary reads and writes.

From BASIC, for example:

```
?&FCFD=0 : ?&FCFE=0 : ?&FCFF=0
PRINT $&FD00
```

## What's in it at power-on

- The first page contains a short greeting (hence `PRINT $&FD00`
  showing a message). It is ordinary RAM and can be overwritten freely.
- If a file called `JIM_Init.bin` exists in the SD card root, its
  contents are loaded into the expansion RAM from the start, replacing
  the greeting. This lets large programs or data sets be pre-loaded
  onto the card and be instantly available to the Beeb.

## Sharing with other features

Other Pi1MHz features use parts of this same RAM space through the JIM
window - the Music 5000 wave RAM, the SD/FAT transfer buffer, and the
Music 5000 WAV recorder all live in it. For straightforward RAM-disc
style use of byte mode and page mode you do not need to care; just be
aware the space is shared if you use those features at the same time as
filling hundreds of megabytes.
