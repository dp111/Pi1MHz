# 1MHz-WiFi host ROM

A 16 KiB sideways ROM that drives the Pi1MHz WiFi and net services from a BBC
Micro, B+, Master or Acorn Electron. It is the host half of `wifi_service` and
`net_service`: those provide the radio, the sockets and the URL fetch, and this
provides the star commands and the OSWORD `&65` entry that applications call.

Build it with beebasm:

```sh
./build.sh
```

That writes `src/1mhz-wifi.rom`. Put it in a sideways slot, or add it to
`firmware/Pi1MHz/` and reference it from `Pi1MHz.cfg` to have Pi1MHz serve it.
It is not wired into the CMake build and does not affect the firmware.

## Commands

| | |
| ------------------------------------- | -------------------------------- |
| `*JOIN <ssid> [password]`, `*LEAVE`    | associate and disassociate |
| `*LAP`, `*LAPOPT [n]`                  | list access points, set scan options |
| `*IFCFG`, `*ONLINE`, `*MODE`           | address configuration and readiness |
| `*WIFI ON\|OFF`                        | radio control |
| `*PING <host>`, `*NSLOOK <host>`       | reachability and name resolution |
| `*DATE`, `*TIME`                       | network time |
| `*WGET <url> [file]`                   | fetch, through the active filing system or a JIM window |
| `*VERSION`, `*PRD`                     | identity, paged RAM dump |
| `*RDINIT`, `*RDCAT`, `*RDSAVE`, `*RDLOAD`, `*RDRUN` | RAM disk |

`*HELP WIFI` lists them on the machine.

## Workspace

The ROM's scratch - the command heap, the parameter string and the driver's
32 bytes of timeouts and error block - lives inside the image, above the
code at `&BC00`. Pi1MHz loads this ROM into sideways RAM (helper 16), so the
image is writable and the workspace costs the host nothing.

It used to sit in host memory at `&0900`, `&0A00` and `&0D90`. Every one of
those belongs to the OS on the machines this ROM supports: `&0900` is the
RS423 output buffer, the speech buffer and ENVELOPEs 5-16; `&0A00` is the
CFS/RFS/RS423 input buffer; and `&0D90` runs into the VFS/AMX mouse
workspace at `&0D92` and then the **extended vector table at `&0D9F`**. On a
Master with ROMs using vectors the last one hung the machine on the first OS
call after any command, and the first stopped a `*FX3,1` serial redirect
dead.

`autorun` (service call 1) probes the image for writability and records the
answer in the image. If the ROM is burnt into a real EPROM there is nowhere
to put the workspace, so commands raise "1MHz-WiFi needs sideways RAM"
rather than corrupting host memory. A real-ROM build would need the
workspace claimed from the OS instead - service call `&02`, or `&24`/`&22`
on the Master, which allocates in hidden RAM and leaves PAGE alone - and
every workspace reference reached through a pointer rather than an absolute
address.

## The RAM disk

The RAM disk holds 65,024 bytes in up to 15 files in the low 64 KiB JIM
window, so a program can be fetched over the network and run without a filing
system fitted. Page 0 is left to the OSWORD `&65` service reply buffer, page 1
is the catalogue, and files follow from page 2 on page boundaries.

It stays in JIM bank 0. That is the only bank an unmodified Electron AP5
forwards, so a format using the upper banks at `&FCFD` and `&FCFE` would work
on the BBC family and silently not on the Electron. Files are page aligned, so
a short file still costs a whole page, and space is reclaimed only by
`*RDINIT`, which clears the catalogue.

## Machine differences

One image runs on all four machines. The host is identified at run time with
OSBYTE `&81`, and two differences follow from it. Sideways ROM selection is
`&FE05` with the Electron deselect cycle against `&FE30` on the BBC family.
The JIM selectors `&FCFD` and `&FCFE` exist on direct BBC-family systems and
must be reasserted in every masked transaction, while an unmodified Electron
AP5 does not forward them.

The startup glyph is the third. The BBC family boots into MODE 7, where codes
above 127 are teletext cells rather than soft characters, so the ROM reads the
mode with OSBYTE 135 and prints the banner without the glyph there. The
Electron has no teletext mode and is unaffected.

## Transport

The ROM writes a bounded command block to `&FCA6-&FCAA`, dispatches it, treats
every status with bit 7 set as busy, yields with OSBYTE `&13`, checks Escape
and applies a finite timeout. Escape sends command 90 so the Pi can discard a
late DNS, ICMP, NTP or scan callback. After a service completes it masks
interrupts, reselects the response cursor and copies the bounded reply before
restoring the previous interrupt state, so IRQ-side MMFS or ADFS activity
cannot move the cursor between the completion poll and the copy.

It never reaches for the `&FC30` cartridge UART the original ElkWiFi hardware
used.

## Provenance

Every file here was written for the 1MHz-WiFi project and carries no ElkWiFi
or UPCFS lineage. The project began as an adaptation of Roland Leurs' ElkWiFi
cartridge ROM, and the parts that still derive from it, together with Martin
Barr's UPCFS, are the cassette filing system, which is not in this ROM and is
not offered here. `*VERSION` credits ElkWiFi for the interface this ROM
implements.
