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
ROMs, and **45-119** belong to the IP/network, WiFi, security and FujiNet
services (120-255 are unallocated). The allocation map is
`src/services.h`.

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

Mode bit **0x80 (keep)** asks the Pi to remember the file for this
slot. A BBC reset still closes it - so the web server, WebDAV and USB
may change it again - but the next fread, fwrite or fsize on the slot
opens it again by name, in the same mode (read only if it has been
made read-only since), without the client noticing. If the file has
gone, or the storage has changed (the card or the USB drive), those
commands fail (FR_NO_FILE or FR_NO_PATH). A rename through command 12
moves the kept name with it. A Pi restart, an eject and commands 14/15
forget kept files and slot directories, and change command 20's boot
id.

Once a slot has its own directory (command 22), the name is relative to
it; otherwise to the shared current directory (command 11). An 8.3
name that does not exist as given is looked up as one of the made-up
short names of command 21, so a name a client read from a directory
block always opens.

**3 - fclose**

    +0        3

Also forgets a kept file.

**4 - fread** (with implicit lseek)

    +0        4
    +1..3     length to read; on completion, the actual bytes read
              (command register = 20 if the read was short)
    +4..7     destination address in buffer
    +8..11    file offset to read from

A read that runs past the end of the file fills the rest of the
requested length with zeros (and returns 20), so a cut-down disc image
reads as a full one.

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

With a slot directory (command 22), both names are in it, and the old
name may be a made-up short name.

**13 - fgetfree**

    +0        13
    +8..11    (returned) free space in bytes / 256

**14 - fmount** / **15 - funmount**

    +0        14 or 15

(Intended to support swapping SD cards while running.)

**16 - funlink** (delete a file or directory)

    +0        16
    +1...     name (zero terminated)

**17 - readdir-ex** (directory entry as a fixed record)

    +0        17
    +4..7     destination offset in the buffer (top byte must be 0)

Like readdir, but writes a fixed **128-byte record** at the given buffer
offset - laid out for a 6502 client (power-of-two size, and a
ready-to-print display line so the Beeb never formats sizes itself):

    rec+0       attribute byte (AM_DIR = &10 etc.)
    rec+1..3    reserved (0)
    rec+4..7    file size, little-endian (0 for a directory)
    rec+8..46   display line: exactly 38 printable characters (name,
                directories marked with a trailing '/', then a
                right-aligned size or "<DIR>"), NUL terminated.
                Control characters are replaced with '?' so the text is
                safe to feed straight to OSWRCH.
    rec+48..127 raw name for fopen/fchdir, NUL terminated (truncated
                at 79 characters)

Returns 20 when there are no more entries. This is what the SD card
explorer (helper 17) uses.

**18 - getcwd**

    +0        18
    +4..7     destination offset in the buffer (top byte must be 0)

Writes the current directory as a NUL-terminated string (up to 128
bytes, control characters replaced with '?') at the given offset.

**19 - set attributes**

    +0        19
    +1        the attribute bits to change (AM_RDO 0x01, AM_HID 0x02,
              AM_SYS 0x04, AM_ARC 0x20; others are ignored)
    +2        their new values
    +3...     name (zero terminated), as for fopen

MMFS2's `*DACCESS` uses this: a locked disc image is a read-only file.
Ignored (and reported as success) under `Beeb_write_protect`.

**20 - SD card type**

    +0        20
    +1..2     (returned) this boot's id, never 0
    Base+4    (returned) 0 or 1 depending on card type

The id stays the same over a BBC reset and changes when the Pi restarts
(or forgets the slots' state: an eject, commands 14/15),
so a client that keeps its own state over a BBC reset can tell when the
Pi has forgotten its side of it (MMFS2 empties its drives). Firmware
before the id leaves +1..2 as the client wrote them: write zeros first,
and zeros back mean an older Pi.

**21 - directory block**

    +0        21
    +4..7     destination offset in the buffer (512 bytes)
    +8..11    block number (0 = the first)

The slot's directory (command 22; else the shared current directory)
as a FAT directory would hold it, 16 entries of 32 bytes per block, so
a client written for raw FAT directory sectors can read it unchanged.
A subdirectory starts with "." and ".."; names starting with "." are
left out; the entries end with a zero byte. Each entry is:

    +0..10    8.3 name: the volume's short name, else the long name
              itself when it is a valid 8.3 name, else a made-up one
              (first 4 characters, "~" and 3 hex digits) - exFAT has no
              short names
    +11       attributes
    +14..19,
    +22..25   times and dates (all the modification time)
    +20..21,
    +26..27   the entry's id, where FAT keeps the first cluster: a
              20-bit hash of its path, never 0, except ".." of a
              first-level directory, which is 0 (FAT's root)
    +28..31   size

**22 - slot directory**

    +0        22
    +1...     directory name, relative to the slot's directory
              ("" or "/" = the root; ".." goes up)

Gives the slot (the low nibble of the command-block page) a current
directory of its own, which commands 2, 12, 19 and 21 then use, so
several clients - MMFS2's two drives, the SD explorer - no longer share
one. The first use starts from the root. A BBC reset keeps it; a Pi
restart, an eject and commands 14/15 forget it.

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

They cover the help screen, the HDMI screen redirector, loading the
shipped ROMs into sideways RAM, the SD card explorer and the serial
redirector/modem. See
[Loading ROMs and helpers](user/helpers-and-roms.md) for the function
list and usage.

## Special SD-card files: `kernel.now` and `reboot.now`

Two magic filenames trigger firmware actions rather than being stored:

- **`kernel.now`** - a firmware image sent under this name over
  [USB/MTP](user/usb-file-access.md), a WebDAV `PUT /kernel.now`, or the
  web interface's upload form is loaded into RAM and chain-booted
  **immediately and transiently** (an open video player is shut down
  first). The SD card's `kernel.img` is not changed, so a power cycle or
  reboot reverts to it. This is for safely test-driving a new build before
  committing it; to make a build permanent, overwrite `kernel.img`. A
  build that changes the VideoCore (VPU) code needs a cold boot instead.
- **`reboot.now`** - copying this file over USB/MTP reboots the Pi
  (reverting to the SD card's `kernel.img`). The copy is accepted and
  nothing is saved; the Pi then closes its files and restarts, so the
  computer may report an error as the device goes away - that is
  expected. It is MTP-only, so it does nothing while the USB port is a
  host (the default); over WiFi use the web interface's `/reboot` page.

Avoid naming any of your own files `kernel.now` or `reboot.now`.
