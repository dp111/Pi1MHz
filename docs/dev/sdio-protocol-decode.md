# Decoding the SDIO bus with a Siglent SDS2000X Plus

The WiFi bring-up in `src/wifi/` drives the CYW43 over SDIO (Arasan EMMC,
GPIO34-39), and the SD card side (`src/rpi/sdcard.c`) drives SDHOST on
GPIO48-53.  When either goes wrong at the electrical or protocol level, a
logic-level trace of CMD/DAT is the only thing that settles the argument.

## What the scope can and cannot do

The SDS2000X Plus has a **fixed** decoder set in firmware - I2C, SPI,
UART/RS232, CAN, LIN, and the licensed CAN FD / FlexRay / I2S / 1553B
bundle.  There is no plugin interface, no on-instrument scripting and no
way to add SD/SDIO to the Decode menu.  The scope is used here purely as
a capture instrument; the decode happens on a PC with
`tools/sdio_decode.py`.

An on-scope approximation for quick bench checks is described at the end.

## Wiring

| Signal | Pi Zero W WiFi (CYW43) | Pi SD card |
|--------|------------------------|------------|
| CLK    | GPIO34                 | GPIO48     |
| CMD    | GPIO35                 | GPIO49     |
| DAT0   | GPIO36                 | GPIO50     |
| DAT1   | GPIO37                 | GPIO51     |
| DAT2   | GPIO38                 | GPIO52     |
| DAT3   | GPIO39                 | GPIO53     |

On a Pi Zero W the CYW43 lines are not brought out to a header - they are
only on the module pads, so this needs a scope-probe-friendly break-out or
fine-pitch probing.  CLK and CMD alone are enough for command-level
debugging (everything `sdio.c` does with CMD52, plus the CMD53 headers);
the DAT lines are only needed to see payloads.

- **2 analog channels**: probe CLK and CMD.  That covers every command,
  every response and their timing.
- **MSO logic probe fitted**: probe all six lines and get the data phases
  decoded as well.

Keep the ground lead short.  At 25-50 MHz a long ground spring turns the
clock into something the digitiser will double-trigger on; if that
happens, `--min-width` and `--hysteresis` clean it up after the fact, but
better probing is the real fix.

## Capture settings

- Sample rate: at least 5x the bus clock.  The bus runs 25 MHz during
  bring-up and 50 MHz once high speed is enabled (`bus_high_speed` in
  `sdio_runtime_status_t`), so 250 MSa/s is the floor and 500 MSa/s or
  more is comfortable.
- Memory depth: as deep as the model allows.  Bring-up from `WL_REG_ON`
  to `STAGE_JOIN` is tens of milliseconds; at 500 MSa/s that is a lot of
  memory, so either accept a window or trigger on the interesting part.
- Threshold: the bus is 3V3 CMOS (1V8 on some paths - check before
  assuming).  The decoder defaults to the midpoint of each channel's
  range, which is right for a clean capture; override with
  `--threshold`.
- Trigger: falling edge on CMD catches the first start bit of a token.
  For a specific transaction, trigger on CMD and use the scope's holdoff
  or the Zone trigger to pick the burst you want.

## Exporting

`Save` -> `Save Type: CSV` -> pick the channels -> `Save` to a USB stick,
or pull the same file from the scope's web interface.  For MSO captures
export the digital group the same way.

The exporter's exact header has changed across firmware revisions, so the
tool sniffs the file rather than assuming a layout: the first all-numeric
line starts the data, and the non-empty line above it names the columns.
If the export has no time column, pass `--sample-rate`.

`--list-channels` prints what the tool found, which is the quickest way
to work out what to pass to `--clk` / `--cmd` / `--dat`.

## Decoding

```sh
# CLK + CMD on two analog channels
tools/sdio_decode.py capture.csv --clk C1 --cmd C2

# full 4-bit bus from an MSO capture, with payload hexdumps
tools/sdio_decode.py capture.csv --clk D0 --cmd D1 --dat D2,D3,D4,D5 --dump 64

# what channels does this file actually have?
tools/sdio_decode.py capture.csv --list-channels

# no scope needed - synthesise a capture and decode it
tools/sdio_decode.py --emit-test-csv demo.csv
tools/sdio_decode.py demo.csv --clk CLK --cmd CMD --dat DAT0,DAT1,DAT2,DAT3
```

Output is one line per bus event, with the time and the gap since the
previous event:

```
      time(us)  delta(us)  dir   packet  name                 detail
         0.340      0.000  ->   CMD52   IO_RW_DIRECT         WR F0 [0x00007] BUS_INTERFACE_CONTROL <= 0x02
         2.340      2.000  <-   R5                           flags 0x10 state=CMD data 0x02   (+2.000 us)
        12.340      2.000  ->   CMD52   IO_RW_DIRECT         WR F1 [0x1000E] SDIO_CHIP_CLOCK_CSR <= 0x08 {ALP_AVAIL_REQ}
        28.340      2.000  ->   CMD53   IO_RW_EXTENDED       RD F2 [0x08000] 1 blk x 64 B block, fixed
        32.340      2.000  <--  DAT     F2                   64 B, 4-bit, block 0, CRC16 ok
      SDPCM len 44 seq 5 chan CONTROL doff 12 flow 0x00 credit 4
      CDC cmd 262 len 12 id 1 GET status 0 iovar 'cur_etheraddr'
```

What it gives you that a generic SPI decode cannot:

- CRC7 on every command and response, CRC16 per DAT line on every block,
  and the write CRC status token plus its busy time.
- CMD52/CMD53 argument breakdown with the **register names from
  `src/wifi/sdio.c`**, so a decode line greps straight back into the
  driver.
- Bus width and per-function block size learned from the CCCR/FBR writes
  earlier in the same capture, so CMD53 block transfers get the right
  length without being told.
- Backplane window tracking: the three CMD52 writes to
  `SDIO_BACKPLANE_ADDRESS_LOW/MID/HIGH` are folded into an absolute
  backplane address on the following F1 CMD53, which is what makes a
  "wrong window" bug visible (see the comment on
  `sdio_backplane_set_window_timeout`).
- SDPCM/BDC/CDC dissection of the function 2 payloads, including the
  iovar name in a control frame.

`--json` writes the same events machine-readably.  `--vcd` writes the
digitised lines for PulseView/GTKWave, where sigrok's own `sdcard_sd`
decoder can be run over them as a cross-check.

`--selftest` synthesises a bus, pushes it through the whole pipeline
(digitise, sample, decode, CSV round trip) and checks the result - run it
after touching the tool.

## On-scope approximation with the SPI decoder

For a quick "is the host talking at all?" check without exporting
anything, the SPI decoder gets you part of the way:

- `CLK` -> SPI Clock, rising edge, `CMD` -> MISO (or MOSI, both are
  shown), MSB first, 8-bit words.
- CS: set to `Clock Timeout` mode rather than a chip select line.

The caveat is alignment: SDIO has no framing the SPI decoder knows about,
so the 48-bit token does not land on the decoder's byte boundaries and
you have to shift the bytes by hand.  A host command starts with `01`
followed by the 6-bit index, so a CMD52 (`0b110100`) begins with the bit
pattern `01110100` if you happen to be aligned - i.e. look for `0x74`,
and for CMD53 (`0b110101`) look for `0x75`.  It is enough to confirm
activity and roughly which command, not to trust a payload.
