# SD Card Explorer

Helper 17 is a full-screen file browser for the Pi's SD card that runs
entirely on the Beeb - no ROM, no sideways RAM, and PAGE is untouched.
The 6502 code streams from the Pi through the paged JIM window 256
bytes at a time, so it works the moment the machine has booted, on any
filing system.

Use it to copy files **from the SD card onto the current filing system**
(DFS discs, ADFS, MMFS images - whatever is active) and **from the
current filing system onto the SD card**.

## Starting it

From BASIC:

```
X%=17 : CALL &FC88
```

or from the command line:

```
*FX147,136,17
*GO FD00
```

The screen switches to MODE 7 and shows the SD card's current
directory: directories in yellow (with a trailing `/`), files in white
with their size, and the selection in green.

## Keys

| Key | Action |
|---|---|
| Up / Down | Move the selection |
| RETURN | Open the selected directory, or copy the selected file to the current filing system |
| G | Same as RETURN on a file |
| Left arrow (or U) | Up to the parent directory |
| P | Put: copy a file from the current filing system onto the SD card (prompts for the name) |
| ESC | Quit |

## Copying a file from the SD card (get)

Select a file and press RETURN. You are asked `Copy as:` - press
RETURN to keep the SD name, or type a name that suits the filing
system (DFS wants at most 7 characters, e.g. `E.ELITE`). The file is
then streamed onto the current filing system.

## Copying a file to the SD card (put)

Press P and type the name of a file on the current filing system. It
is copied into the SD directory you are looking at, under the same
name.

## Notes and limits

- A filing system error (bad name, disc full, file locked...) is
  caught and shown on the status line; press a key to carry on
  browsing.
- Directories are listed in SD card order, up to 250 entries.
- Files larger than 16 MB cannot be transferred.
- The explorer uses the RS423/cassette buffers (&0900-&0AFF) and zero
  page &70-&86 as workspace, and leaves the machine in MODE 7.
- Load/exec addresses are not preserved; set them afterwards if the
  file needs them (or use `*OPT`-style defaults).
- Transfers use FAT service file slot &FD and directory slot &FC, so
  they coexist with MMFS2's own slots.
