# Advanced: Programming Pi1MHz

This is the **programmer's reference** - the memory-mapped registers and
command protocols Pi1MHz exposes on the BBC's 1MHz bus, for writing 6502
software (or a filing system such as MMFS) that talks to it directly.

If you just want to *use* Pi1MHz, you want the
[user guide](user/README.md) instead; this page is not needed for normal
operation.

All base addresses below are the defaults and can be relocated in
`Pi1MHz.cfg` (see the [configuration reference](user/configuration.md)) -
**except** the framebuffer (`&FCA0`) and the services port (`&FCA6`),
which are hard-wired into the built-in 6502 helper code and must stay put.

## JIM expansion RAM

Two views of the same large expansion RAM (up to ~480 MB on a Pi Zero,
~992 MB on a Pi 3B+):

**Byte mode** (16 MB window, Sprow-compatible -
<http://www.sprow.co.uk/bbc/ramdisc.htm>):

    &FC00  address, low byte
    &FC01  address, middle byte
    &FC02  address, high byte
    &FC03  data (read/write, address auto-increments)

Byte mode addresses the first 16 MB of the RAM.

**Page mode** (the whole RAM, 256 bytes at a time):

    &FCFD  page number, high byte
    &FCFE  page number, middle byte
    &FCFF  page number, low byte
    &FD00-&FDFF  the selected 256-byte page appears in JIM

The first page holds a short greeting at power-on (`PRINT $&FD00`); it is
ordinary RAM and may be overwritten. If a file `JIM_Init.bin` exists in
the SD card root it is loaded into JIM from the start (overwriting the
greeting), so large programs or data can be pre-staged on the card.

## Services port (`&FCA6`): SD/FAT and AUN command interface

A general command mailbox. The first byte of a command block selects a
service by number: commands **0-29** are the SD/FAT service documented
here, commands **30-44** are the Econet AUN service used by the AUNFS
ROMs. The allocation map is `src/services.h`.

    &FCA6  address pointer, low 8 bits  (24-bit address into JIM)
    &FCA7  address pointer, middle 8 bits
    &FCA8  address pointer, top 8 bits
    &FCA9  data register (read/write, address auto-increments)
    &FCAA  command register (&F0-&FF)
    &FCAB  IRQ status (used by services that raise nIRQ, e.g. AUN)

A 16 MB buffer in JIM RAM holds both the data being transferred and the
command blocks. Writing `&Fn` to the command register (`&FCAA`) executes
the command block located at `0xFFn000` in the buffer - so up to 16
command blocks (`&F0`-`&FF`) can be in flight, one per open file or
directory. When a command completes, the command register reads back a
result code (0 = success, or an error; bit 7 set means "still busy").

Suggested polling loop:

    LDA #command
    STA &FCAA
    .wait
    LDA &FCAA
    BMI wait            \ bit 7 set = still executing
    BNE command_error   \ non-zero = FatFs error code

Multiple files may be open at once, but each must be a unique file. A
filing system such as MMFS can therefore cache an entire drive in the
buffer and never need to raise PAGE. Suggested buffer layout: first 4 MB
for the active filing system, 8-14 MB for the active program.

### Result codes (FatFs `FRESULT`)

    0  FR_OK                 succeeded
    1  FR_DISK_ERR           low-level disk I/O error
    2  FR_INT_ERR            assertion failed
    3  FR_NOT_READY          physical drive not ready
    4  FR_NO_FILE            file not found
    5  FR_NO_PATH            path not found
    6  FR_INVALID_NAME       invalid path name
    7  FR_DENIED             access denied / directory full
    8  FR_EXIST              file exists
    9  FR_INVALID_OBJECT     invalid file/directory object
    10 FR_WRITE_PROTECTED    write protected
    11 FR_INVALID_DRIVE      invalid logical drive
    12 FR_NOT_ENABLED        volume has no work area
    13 FR_NO_FILESYSTEM      no valid FAT volume
    14 FR_MKFS_ABORTED       f_mkfs() aborted
    15 FR_TIMEOUT            timed out
    16 FR_LOCKED             rejected by file-sharing policy
    17 FR_NOT_ENOUGH_CORE    LFN working buffer alloc failed
    18 FR_TOO_MANY_OPEN_FILES  too many open files
    19 FR_INVALID_PARAMETER  invalid parameter
    20                       short fread/fwrite

### Commands (first byte of the command block)

All multi-byte fields are little-endian. Buffer addresses are offsets
into the JIM buffer; the top byte must be zero.

**0 - Read sector**

    +0        0
    +4..7     destination address in buffer
    +8..11    start sector (LBA)
    +12..15   number of sectors to read

**1 - Write sector**

    +0        1
    +4..7     source address in buffer
    +8..11    start sector (LBA)
    +12..15   number of sectors to write

**2 - fopen**

    +0        2
    +2        mode: FA_READ 0x01, FA_WRITE 0x02, FA_OPEN_EXISTING 0x00,
              FA_CREATE_NEW 0x04, FA_CREATE_ALWAYS 0x08,
              FA_OPEN_ALWAYS 0x10, FA_OPEN_APPEND 0x30
    +3...     filename (zero terminated)

**3 - fclose**

    +0        3

**4 - fread** (with implicit lseek)

    +0        4
    +1..3     length to read; on completion, the actual bytes read
              (command register = 20 if the read was short)
    +4..7     destination address in buffer
    +8..11    file offset to read from

**5 - fwrite** (with implicit lseek + fsync)

    +0        5
    +1..3     length to write; on completion, the actual bytes written
              (command register = 20 if the write was short)
    +4..7     source address in buffer
    +8..11    file offset to write from

**6 - fsize**

    +0        6
    +8..11    (returned) file size

**7 - fopendir**

    +0        7
    +1...     directory name (zero terminated)

**8 - fclosedir**

    +0        8

**9 - readdir**

    +0        9
    +4...     (returned) next entry's filename, zero terminated

Each call returns the next directory entry's name; returns 20 when there
are no more entries. Only the name is returned (not size/dates/attributes).

**10 - fmkdir**

    +0        10
    +1...     directory name (zero terminated)

**11 - fchdir**

    +0        11
    +1...     directory name (zero terminated), e.g. "/dir1"

**12 - frename**

    +0        12
    +1...     old name (zero terminated), then new name (zero terminated)

**13 - fgetfree**

    +0        13
    +8..11    (returned) free space in bytes / 256

**14 - fmount** / **15 - funmount**

    +0        14 or 15

(Intended to support swapping SD cards while running.)

**16 - funlink** (delete a file or directory)

    +0        16
    +1...     name (zero terminated)

**20 - SD card type**

    +0        20
    Base+4    (returned) 0 or 1 depending on card type

## Internal status and control (`&FCCA`)

    &FCCA  select the status/command address
    &FCCB  read status / write command

Addresses currently defined:

    &00  read only: JIM RAM size in 16 MB steps

## Helper functions

The built-in helpers are invoked by writing a function number and calling
the helper entry point:

    ?&FC88 = function : CALL &FD00
    or  X% = function : CALL &FC88

They cover the help screen, the HDMI screen redirector, and loading the
shipped ROMs into sideways RAM. See
[Loading ROMs and helpers](user/helpers-and-roms.md) for the function
list and usage.

## Special SD-card files: `kernel.now` and `reboot.now`

Two magic filenames, sent to the Pi over [USB/MTP](user/usb-file-access.md),
trigger firmware actions rather than being stored (this is an MTP-write
feature; copying a file of the same name over WebDAV just stores it):

- **`kernel.now`** - a firmware image copied under this name is loaded
  into RAM and chain-booted **immediately and transiently**. The SD
  card's `kernel.img` is not changed, so a power cycle or `reboot.now`
  reverts to it. This is for safely test-driving a new build before
  committing it; to make a build permanent, overwrite `kernel.img`.
- **`reboot.now`** - writing this file reboots the Pi (reverting to the
  SD card's `kernel.img`).

Avoid naming any of your own files `kernel.now` or `reboot.now`.
