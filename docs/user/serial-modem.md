# The serial redirector and the WiFi modem

Pi1MHz can take over the Beeb's **RS423 serial port** and put a
**Hayes-style modem** on the end of it that "dials" over WiFi. Comms
software that talks AT commands to a modem can then reach telnet BBSes
and other TCP services on your network or the internet - no serial lead,
no real modem.

## Turning it on

Run **helper 19**:

```
X%=19 : CALL &FC88
```

(or `*FX147,136,19` then `*GO FD00`). It prints ` Serial: Pi`. From then
on, everything the Beeb sends to RS423 goes to the Pi, and what the Pi
sends back arrives as RS423 input.

- **BREAK undoes it**, as with the screen redirector: run the helper
  again after every BREAK.
- To hand the port back without a BREAK, select page 19 and enter it at
  `&FD03`: `*FX147,136,19` then `*GO FD03`. It prints ` Serial: 6850`.
- While it is on, the **real serial port is silent** - a printer or
  serial lead on RS423 sees nothing until you unhook it.

The modem needs WiFi set up and `net_enable=1` in `/Pi1MHz/Pi1MHz.cfg`
(see [WiFi setup](wifi.md)). Without it, dialling answers `NO DIALTONE`.

## Which software works

The redirector stands in for the serial chip one level up, at the MOS's
RS423 buffers, so anything that uses the operating system's serial
routines works unchanged: `*FX2` / `*FX3` stream selection, OSBYTE 138
to send, OSBYTE 145 / `ADVAL(-2)` to receive, `OSRDCH` with RS423 input
selected.

Software that programs the 6850 chip at `&FE08` directly does **not**
see the redirector - it talks to the real, now silent, chip.

The ESCAPE key's code arriving from the line is passed through as data
(as a modem link needs), not turned into an ESCAPE condition.

Bytes reach the Beeb at up to about 6.7 KB a second.

## A minimal terminal

If you have no comms program, this is enough to talk to the modem:

```
10 REM Minimal terminal for the Pi1MHz modem
15 *FX3,0
20 REPEAT
30   K%=INKEY(0)
40   IF K%>=0 THEN A%=138:X%=2:Y%=K%:CALL &FFF4
50   A%=145:X%=1:R%=USR&FFF4
60   IF (R% AND &1000000)=0 THEN VDU (R% AND &FF0000) DIV &10000
70 UNTIL FALSE
```

Line 15 sends printing to the screen only: if the screen were also
copied to RS423 (`*FX3,1`), everything the modem echoed would be sent
straight back to it, round and round. Line 40 puts each key into the
RS423 output buffer (OSBYTE 138), line
50 takes one byte from the input buffer (OSBYTE 145; carry set means
it was empty), and line 60 prints it. Run helper 19 first, then `RUN`,
and type `AT` and RETURN: the modem answers `OK`. ESCAPE stops it.

## Dialling

```
ATDT bbs.example.com:6502     a host name or IP address, and a port
ATDT bbs.example.com          port 23 (telnet) when none is given
ATDT192168001005              12 digits: 192.168.1.5, port 23 - for
                              diallers that only accept digits
ATDT1                         phonebook entry 1 (one or two digits)
```

`ATD`, `ATDT` and `ATDP` are the same. The modem answers `CONNECT` and
goes online, or `NO ANSWER`, `BUSY`, `NO CARRIER` or `NO DIALTONE`.
When the other end hangs up, the modem says `NO CARRIER` and is back in
command mode.

**The phonebook** lives in `Pi1MHz.cfg`:

```
modem_phone_1=bbs.example.com:6502
modem_phone_2=192.168.1.20:23
```

**Telnet servers:** many BBSes speak the telnet protocol. `ATNET1`
before dialling handles its option negotiation, so it does not show up
as junk on the screen; `ATNET0` (the default) is a plain TCP
connection.

## While connected

Everything you type goes down the line. To get back to the modem
without hanging up, wait a second, type `+++`, and wait another second:
the modem answers `OK`. Then:

- `ATH` hangs up;
- `ATO` goes back online to the same call.

A BBC reset also hangs up.

## Commands

| Command | What it does |
|---|---|
| `AT` | Answers `OK` |
| `A/` | Repeats the last command line |
| `ATD...` | Dials - see above |
| `ATH` | Hangs up |
| `ATO` | Back online after `+++` |
| `ATZ`, `AT&F` | Hang up and restore the defaults |
| `ATE0` / `ATE1` | Command echo off / on (on by default) |
| `ATV0` / `ATV1` | Numeric / word results (words by default) |
| `ATQ0` / `ATQ1` | Results shown / suppressed |
| `ATI` | Identifies the modem |
| `ATNET0` / `ATNET1` | Plain TCP / telnet |
| `ATSn=v`, `ATSn?` | Set / show S-register *n* |
| `AT&C`, `AT&D`, `AT&K`, `AT&W`, `ATX`, `ATM`, `ATL` | Accepted and ignored, so comms software's setup strings work |

Several commands can share a line: `ATE0V1`.

Useful S-registers: **S2** the escape character (`+`; a value over 127
turns the escape off), **S7** how many seconds to wait for a connection
(50), **S12** the escape guard time in fiftieths of a second (50 = one
second).

## Things to know

- **Calls only go out.** The modem does not answer incoming calls
  (`RING` / `ATA`).
- The Pi's web status page has a **Modem** row: the modem's state and
  the last network error, useful when a dial fails.
- The redirector's stub sits at `&FCCC`-`&FCF8`. `Serial_addr=-1` in
  `Pi1MHz.cfg` switches it off, or moves it with another address - see
  [Configuration](configuration.md).
