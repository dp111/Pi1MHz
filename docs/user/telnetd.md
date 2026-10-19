# The telnet daemon: the Beeb's keyboard and screen over the network

Pi1MHz can let you `telnet` to the Pi and sit at the Beeb's keyboard and
screen. What you type arrives as if it had been typed on the Beeb, and what
the Beeb prints comes back to your terminal. Anything that can run telnet -
PuTTY, a Linux or macOS terminal, Windows' telnet client, a script - can
drive the Beeb from another room, and a program can drive it too.

It is written to work on a BBC B, B+ and Master 128, with the real keyboard still
working beside it. It has been run on a Master 128 and on a BBC Model B (MOS 1.20,
the auto-start ROM in a flash ROM bank); it has not been run on a B+.

## Turning it on

1. In `/Pi1MHz/Pi1MHz.cfg` set the port to listen on and reboot the Pi:

   ```
   telnetd_port=23
   ```

   WiFi has to be set up (see [WiFi setup](wifi.md)). Without the key, or with
   `0`, the daemon is off. `net_enable` is not needed.

2. On the Beeb, fit the **auto-start ROM** (below) and press BREAK - or run
   **helper 20** by hand:

   ```
   X%=20:CALL &FC88
   ```

   (or `*FX147,136,20` then `*GO FD00`). It prints ` Telnet: Pi`. This hooks
   the Beeb's keyboard and RS423 output to the Pi (it uses the same stub as
   the [serial redirector](serial-modem.md), helper 19) and sets `*FX3,1`, so
   what the Beeb prints is copied to the Pi.

3. From another machine:

   ```
   telnet 192.168.1.20
   ```

   using the Pi's address. You get a banner and, if the Beeb is hooked,
   `[BBC attached - press RETURN]`. Press RETURN and the Beeb answers with its
   prompt.

To hand things back without a BREAK, enter the page at its second entry:
`*FX147,136,20` then `*GO FD03`. It prints ` Telnet: off` and puts `*FX3` back.

**BREAK undoes it**, as with the other redirectors: run helper 20 again after
every BREAK - or load the auto-start ROM below once and never type it again. A
telnet session that is open when the Beeb is reset stays connected and says
`[BBC not attached: ...]` until the hooks are back.

### Drawing what is already on the screen

The Pi only knows what has been printed since it hooked in, so a terminal that connects after
the Pi was restarted, or after a reset the Pi did not see, would otherwise miss text that is
already on the Beeb's screen. With the auto-start ROM fitted the Pi can simply ask for it, and
nothing shows on the Beeb:

- The ROM takes the Beeb's buffer-remove vector (REMV). Whatever is waiting for a key calls it
  over and over, outside any interrupt, so that is a safe place to run code.
- The Pi puts one byte, `&FF`, in the keyboard buffer. REMV sees it come out and keeps it from
  the program; the ROM reads every character cell of the screen (OSBYTE 135) and sends the ones
  that are not blank to the Pi as `VDU 31,x,y,char` with the screen drivers off, then puts the
  cursor back. The Pi clears the terminal and draws that.
- It is done whenever a terminal connects, and on F11 or Ctrl-L. It takes a second or two
  (about a millisecond per cell); characters only, not colours.
- When a terminal connects and the Pi's own copy of the screen (below) has something on it, the
  terminal is drawn from that copy first, in its colours, and the read only checks it. If every
  cell agrees, nothing more happens. If not - a program changed the screen where the Pi could not
  see, by printing with the RS423 copy off or writing to screen memory - the copy takes the
  Beeb's characters (cells that agree keep their colours) and the terminal is cleared and drawn
  again.
- It runs on the host machine, so it works with a 6502 second processor on, and on a BBC B
  (MOS 1.2) and a Master 128 (MOS 3.20), which is how it was tested.
- If the Beeb is busy in a program that is not reading the keyboard, the request waits in the
  buffer until the program does. Without the ROM the Pi draws what it saw printed instead.

### Surviving BREAK: the auto-start ROM

`telnetd.rom` is a 16 KB sideways ROM, under a kilobyte of code, that hooks the
daemon in on every reset. The image is `firmware/Pi1MHz/telnetd.rom`. Burn it into
an EPROM or a flash ROM bank, or put it on the SD card (`/Pi1MHz/telnetd.rom`) and
load it into sideways RAM the way you load any ROM image, then press CTRL-BREAK. It
stays there across BREAK, and from then on every BREAK prints `TelnetD` and the
session is back.

- **It needs nothing from the SD card.** The ROM finds the Pi's serial stub at its
  usual place (`&FCCC`) and hooks the Beeb's keyboard and RS423 output into it
  itself, exactly as helper 20 does, so it works without the helper pages
  (`6502code.bin`) and without paging anything into JIM.
- **It does nothing unless telnetd is on.** The Pi tells the ROM whether
  `telnetd_port` is set; without it (or with no Pi1MHz, or with `Serial_addr=`
  moving the stub) the ROM touches nothing: no vectors, no `*FX3`, no `*FX156`. With
  a moved stub, helper 20 still hooks the Beeb in by hand (`X%=20:CALL &FC88`).
- **It uses no Beeb memory.** The three MOS routines it goes on to and the SHIFT
  request for paged mode are kept in 8 bytes the Pi provides in its own page,
  `&FCB7-&FCBE` (just below the serial stub), so nothing in the Beeb's RAM or the
  MOS's tables that belongs to anything else is used.
- **`*HELP` shows which image is fitted**, as `TelnetD 0.2 b15` (the build number
  goes up with every image).
- **It claims nothing.** Every service call is passed on with A, X and Y as they
  came, so it sits happily beside filing systems and other ROMs in any bank.
- **It puts the hooks back if something else takes them.** If the keyboard insert
  or interrupt vector is reset behind the Pi's back (a second processor starting
  up, a Master's MOS) while the Pi still has the session, the next time anything
  reads the keyboard the ROM hooks them in again. Unhooking on purpose (helper 20's
  second entry) is left alone.
- **It presses SHIFT for paged mode** (build 10). A telnet client cannot send SHIFT on
  its own. SHIFT is a key the MOS reads from the keyboard itself, not from the buffer,
  so for Page Down the Pi sets a flag in the ROM's workspace in its own page (build 15;
  before that, Beeb memory by an interrupt "trip"), never a byte in the keyboard buffer: nothing typed
  ahead, not even a COPY pressed while the page waits, can hold it up. The ROM claims
  the keyboard vector (KEYV, only while it is the MOS's own) and, when the MOS asks
  whether SHIFT is pressed (paged mode's wait at the end of a page), answers yes once
  if the flag is set, and clears it.
- **It tells the Pi every key a program takes** (build 13). From that the Pi knows how
  many keys are waiting in the Beeb's buffer and when the Beeb last took one; the Status
  page's Telnetd line shows `keys waiting N, last taken S s ago`, and adds
  `BBC NOT TAKING KEYS` when keys have waited 5 seconds with none taken (a hung program,
  or one that does not read the keyboard). And when a program takes a key and draws no
  character, as WordWise-Plus's editor does (it writes the screen memory itself), the Pi
  reads the screen quietly and draws only what changed, so what you type shows without F11.
- **Characters above 127 can be typed and pasted** (build 13). The MOS's own key read
  turns a byte &80-&FF in the keyboard buffer into a function key or a cursor key, so the
  Pi sends such a character behind a marker byte, and the ROM (on the read-character
  vector, RDCHV) hands the program the byte as it is. UTF-8 from the terminal is decoded
  (e-acute arrives as 233); in MODE 7 the terminal's teletext glyphs map back to their codes.
- **MODE 7 control codes stored below 32** (WordWise-Plus writes its colours as &01-&07,
  which the teletext chip treats as &81-&87) are sent in a screen read as &81-&87
  (build 14), so F11 shows them in the right place.

**The ROM does not survive switching the Beeb off.** Ordinary sideways RAM is
cleared when the power goes, so after a power-on the ROM is simply not there, and
the daemon is not started until something loads it again (see "Starting at
power-on" below). What the ROM gives you is the daemon back after every BREAK and
CTRL-BREAK for as long as the power stays on. If your sideways RAM is
battery-backed, or the ROM is burnt into an EPROM, it is there at power-on too.

**In an EPROM or flash bank it is there at power-on.** The image,
`firmware/Pi1MHz/telnetd.rom`, is exactly 16 KiB, so a 27C128 or compatible holds it,
and it fits a ROM socket. On the Model B bench it lives in a flash ROM bank, and three
power cycles in a row each hooked the Pi in with nothing typed.
A ROM that is present at power-on answers the Beeb's first reset call, before the
Beeb tries its boot disc, and because it waits for the Pi (above) it holds the
whole boot until the Pi is up, which should also give the Pi's emulated hard disc
time to be ready before ADFS looks for it. **That last effect is expected, not yet
measured:** without the ROM, a power-on boot from a Pi-served disc was seen to fail
about as often as it worked (below). Read the chip back after programming; the
image's MD5 (build 15) is `7E4C87DF4A6CAB43497A6D37B61AF4C3`.

**On a Master 128 also `*INSERT` the bank** (`*ROMS` shows which one `TelnetD`
went into): the Master only starts ROMs marked as inserted, and a ROM loaded after
the last reset is listed as `unplugged` until it is. The mark is kept in the
Master's battery-backed memory. If your sideways RAM is battery-backed, the ROM
stays in it, and then loading it once is all it takes for the telnet daemon to
start by itself at every power-on and BREAK.

**At power-on the ROM waits for the Pi.** The Pi is normally powered from the
Beeb, so when you switch the Beeb on the Pi is still booting (about two and a
half seconds) while the Beeb checks its ROMs. After a power-on the ROM therefore
waits, up to about 3.6 seconds, for the Pi to answer before it starts the daemon;
after a BREAK it does not wait. If no Pi ever answers it gives up and the Beeb
boots on; on a machine without the Pi1MHz (the screen blank, the cursor top left
for those seconds) press any key to boot at once. The Pi counts as there only when
its whole `STX &FC88` window reads back, so an empty bus is never taken for one. Tried on a
Master 128 with the ROM in an EPROM and no Pi1MHz fitted (build 12): it boots on. Power-on is the one place the Beeb pauses for the Pi, so the first boot
of the day takes a little longer. It needs sideways RAM (a Model B needs a
sideways RAM board; a Master 128 has it), is the same
image on a B, B+ and Master 128, and does nothing if no Pi1MHz answers. To
remove it, switch the machine off or `*UNPLUG` its slot. Load it only once per
power-on: a second copy in another bank answers the same reset call.

## What your keys do

| You press | The Beeb gets |
|---|---|
| ordinary keys | the same characters |
| RETURN | RETURN |
| Backspace, Delete | DELETE |
| arrow keys | the cursor keys (the MOS's copy-cursor editing works) |
| End | COPY (copies the character under the copy cursor) |
| F1 to F9, F10 | f1 to f9, f0 |
| F12 | BREAK: a soft reset, the same jump as `CALL !-4`, so it works whatever is running. The terminal is cleared and the cursor put where the Beeb reports it; the session comes back when something hooks the daemon in again (HELPERS or the auto-start ROM). Boot banners printed before the hook are not shown. |
| Page Down | SHIFT, pressed once: in paged mode (`VDU 14`, CTRL-N) the Beeb stops at the end of each page until SHIFT is pressed, and Page Down lets it go on a page. Needs the auto-start ROM, build 10 or later. |
| F11 or Ctrl-L | Draw the screen again: the Pi asks the auto-start ROM for what is on the Beeb's screen (see above). Nothing shows on the Beeb. |
| the pound sign | the Beeb's pound (character 96) |
| ESC | the character 27 (see below) |
| Ctrl-letters | the control code, `Ctrl-A` = 1 and so on |

A lone ESC is held for a tenth of a second to see whether it is the start of
an arrow or function key.

## What you see

The Beeb's output is its VDU stream, and the daemon turns it into ANSI for
your terminal: `CLS` and `MODE` clear the screen, `TAB(x,y)` (VDU 31)
positions the cursor, `COLOUR` sets the text and background colours, and DELETE
rubs out a character. The parameter bytes of the VDU codes a terminal cannot
show (palette, VDU 23) are swallowed, so they do not print as rubbish. In the
modes other than 7 `COLOUR` stays set from line to line, as on the Beeb. The
colour numbers are the mode's own: in MODE 1 and 5
`COLOUR 1`, `2` and `3` are red, yellow and white, in the two-colour modes `1` is
white, and `VDU 19` changes them, as it does on the Beeb. Text already printed
keeps the colour it was drawn in, which a terminal cannot change afterwards.
Output the operating system prints carries no colour of its own, it just
continues in the colour the Beeb has set, so a terminal that connects is told
that colour before anything else. `VDU 23,1,0` and `VDU 23,1,1` hide and show
the terminal's cursor as they do the Beeb's. A pound sign comes out as UTF-8.

What a program writes straight to screen memory does not appear, since the Pi
only sees what goes through the MOS. The window of the terminal should be at
least as large as the Beeb's screen for the positioning to look right.

### MODE 7: teletext as the chip draws it

In MODE 7 each row of the screen is drawn the way the Beeb's teletext chip (the
SAA5050) draws it, from the left: a row starts white on black, and a control code
changes the cells after it (or, for some, from its own cell). So you see:

- **the colours**: alphanumeric and graphics colours, new background and black
  background, flash (as the terminal's blink) and conceal;
- **the graphics**: each mosaic character as its block of six cells, using
  Unicode's "sextant" characters, contiguous and separated alike, with hold
  graphics;
- **the chip's own characters**: where the BBC has `£ ← ½ → ↑ # ― ¼ ‖ ¾ ÷` and a
  solid block instead of some ASCII ones, you see those;
- **double height**: a row with a double height code is drawn as a double height
  line (the terminal's DECDHL), the first of a pair as the top half and the next as
  the bottom, as the Beeb pairs them. On a double height line the terminal also
  doubles the width, so that row is twice as wide as the others.

A control code typed or printed in front of text that is already there changes how
that text looks, just as on the Beeb. `COLOUR` does nothing in MODE 7, as on the
Beeb. The sextants need a font with Unicode's "Symbols for Legacy Computing"
(Cascadia Code, Windows Terminal's font, has them); a terminal that is not using UTF-8
(`telnetd_utf8=0`) gets the Latin-1 or nearest ASCII character instead, and `#`
for a mosaic.

### Graphics: xterm's Tektronix window

MOVE, DRAW and PLOT (`VDU 25`) cannot be shown as text, but xterm still has the
Tektronix 4014 graphics terminal it grew out of. When a session starts the daemon
asks the client for its terminal type (telnet TTYPE), and only if that says xterm
asks the terminal itself what it is (`CSI > q`, which a terminal that does not know
it may print, as Windows telnet would); if it answers that it is xterm, the
Beeb's graphics are drawn in xterm's Tek window: lines, points, triangles and the
Master's rectangles (outlined), dotted lines (PLOT 16-31) in the tube's dotted style,
with `VDU 29` moving the origin and `CLG` and `MODE` clearing the page. The Beeb's 1280 x 1024 is mapped onto the Tek's
1024 x 780. A storage tube cannot rub out, so drawing in the background colour
or inverting only moves the graphics cursor. Text goes back to xterm's text
window, and xterm then brings its text window to the front: the Tek window keeps
its picture behind it (place the two side by side with
`xterm -xrm 'XTerm*tekGeometry: +620+0'`). Tested in a real xterm (2026-10-18).
Nothing changes for any other terminal, and `telnetd_tek=0` turns it off.

### The daemon keeps a picture of the Beeb's text screen

The Beeb's screen is not the terminal's. Its size depends on the mode (40, 80 or
20 columns, 25 or 32 rows), it wraps the cursor the moment a line is full where
a terminal waits for the next character, and the MOS scrolls inside a text window
without telling anyone. So the daemon follows every VDU code and keeps its own
copy of the screen, and sends an exact cursor position or a scroll wherever the
terminal would otherwise drift. It does this all the time, whether or not anyone
is connected, so **when you connect, your terminal is drawn from it**: you see
what the Beeb's screen holds, in its colours, and the cursor where the Beeb's is.
With the auto-start ROM the copy is then checked against the Beeb's real screen and
drawn again if they differ (see "Drawing what is already on the screen").

It can only follow what it sees. Helper 20 asks the MOS for the screen mode
(OSBYTE 135) and the text cursor (OSBYTE 134) every time it hooks in, and tells
the Pi, so after a CTRL-BREAK the daemon knows the mode and where the cursor is;
what is on the screen it learns as it is printed. If the helper is an older one
that does not report, the daemon assumes `telnetd_mode` (default 7) and does not
trust its cursor position until the Beeb sends a `CLS`, `MODE`, `TAB`, `HOME` or
a text window; type `CLS` once.

What the Beeb prints in the first moments after the helper hooks in is kept (the
first six lines, up to 400 bytes, until the language's prompt), and only then: what
a program prints later, even hidden (VDU 21), is never taken for a banner. With
the auto-start ROM that is the second processor's, filing systems' and BASIC
banners: the ROM hooks in at the first service call, before they print. A
connecting terminal is greeted with them, then a blank line and

    Pi1MHz telnet - connecting . . .

with a dot every 0.3 seconds, for at least a second and until the client has said
its window size; the Beeb's output waits meanwhile. Then the terminal is cleared
and the Beeb's screen drawn in place, row by row, so the terminal's rows are the
Beeb's from the start: what the Beeb prints next lands where it should, and
nothing jumps when it scrolls or clears. A window smaller than the Beeb's mode is
told so under the dots for three seconds first.

A session whose rows are not the Beeb's (a client that does not understand the
screen codes, or a terminal resized smaller) is drawn again in place by F11, by a
new window size from the client, or by the first copy cursor key.

Text windows narrower than the screen scroll correctly on the Beeb, but the
terminal is not scrolled to match.

### The copy cursor

BASIC's cursor editing has two cursors: the one where typing goes, and the copy
cursor that the arrow keys move over the screen and End (COPY) copies from. The
MOS moves the second one itself and never sends it down the serial line, so a
terminal would show only the first. The daemon follows the arrow keys, End and
RETURN, shows the copy cursor as a reversed character, and moves it as COPY
takes characters, as the MOS does, wrapping as the MOS's does: inside the text
window, off the top to the bottom row and back, off either side to the other side of
the row above or below. It moves only when the Beeb takes the key: with ROM build 11
the ROM tells the Pi each time a program takes a cursor key, COPY or RETURN out of
the keyboard buffer, as the MOS's line input does, so a key that only waits there - a
listing in paged mode waiting for SHIFT - moves nothing, whatever the program's
prompt looks like. (With an older ROM the daemon guesses from the screen instead: the
copy cursor starts only after something on the cursor's row, and in paged mode only
on a row that begins with `>` or `?`.) COPY with no copy cursor does nothing, as on
the Beeb. The mark
appears once the Beeb has been quiet
for a few hundredths of a second, so that it follows whatever has just been
typed. RETURN or ESC removes it. It is never put on a cell the daemon has not
seen drawn, so after connecting, `CLS` and `LIST` again if you want to edit a
line that was on the screen before the daemon saw it. `telnetd_copycursor=0`
turns it off (a game that reads the arrow keys would otherwise move a mark
about).

## Things to know

- **Make the terminal at least as big as the Beeb's screen**: 80 columns by 32
  rows covers every mode (MODE 0, 1, 2, 4 and 5 have 32 rows, 3, 6 and 7 have 25).
  The daemon puts each row at the Beeb's row number, so in a smaller window the
  bottom rows land on the wrong lines (and the copy cursor's mark with them).
  Windows Terminal opens at 30 rows; start it bigger, for example
  `wt --size 100,40 telnet 192.168.1.20`. The daemon asks the client for its window
  size (telnet NAWS) and, if the window is smaller than the Beeb's mode, says so in
  one line before the screen is drawn. A client that reports a resize (Debian's
  telnet does) has its screen drawn again by the daemon, at once: no key is needed.
  Windows telnet reports its size only when it connects, so after resizing it press
  F11 (the message says so only to Windows telnet).

- **ESC and Ctrl-C raise the Beeb's ESCAPE**, so they stop a running BASIC
  program (error 17, "Escape") as the real key does, even when the Beeb is busy
  and its keyboard buffer is full of type-ahead, and they discard what was typed
  ahead. A lone ESC is told from the start of an arrow or function key by waiting
  a tenth of a second. The catch: a program that has set `*FX229,1` so that ESC
  is an ordinary key (a few games) never gets the character 27 from telnet, only
  the ESCAPE condition. The daemon writes the MOS's escape flag directly (bit 7 of
  &FF), which is safe from an interrupt, rather than calling the MOS.
- **One session at a time.** A second connection is told the BBC is in use
  and closed. The exception is a session that has died: if the client's machine
  crashed or the client was killed, the old session may still be holding the
  slot. When the same address connects again and the old session has made no
  progress for 10 seconds while something was waiting on it (output not
  acknowledged, or typing it could not pass on), the old session is dropped and the
  new one takes over. A session that is only quiet is never replaced, nor is one
  from another address. After 30 seconds with no progress a session is dropped
  anyway. The Status page's Telnetd line counts the `replaced` and `dropped`.
- **While helper 20 is in, the modem is off.** The redirected RS423 stream is
  the daemon's, not the [WiFi modem](serial-modem.md)'s; run helper 19 to get
  the modem back.
- **Nobody connected:** the Beeb's output is read and thrown away, so the
  Beeb never stalls waiting for it. Keys you type at the Beeb are unaffected.
- **Printing is a little slower while helper 20 is in.** Everything printed is
  also sent to the Pi: a loop of 1,500 `PRINT`s took 17.8 s against 15.1 s with
  it off (about 18%, measured in the first sessions). It does not depend on whether a
  client is connected. `*FX3,0` turns the copy off and `*FX3,1` back on.
- **Function keys follow your `*KEY` definitions**, as the real keys do, and
  the cursor keys edit with the copy cursor (cursor up, then COPY, which is End
  on a PC keyboard). `*FX4,1` and `*FX225,128` make them return their codes
  instead, for a program that reads them.
- **A vanished client is dropped.** An idle session is sent a telnet NOP every
  30 seconds, which every client ignores; one whose machine has gone away does
  not answer, and the session closes by itself within a few minutes, freeing
  the daemon for the next one.
- **Keys wait for the Beeb.** The Beeb's keyboard buffer holds 32 characters. A
  paste is fed in as the Beeb takes it, so a long paste while a program is
  not reading the keyboard simply waits.
- **How fast it goes.** The Beeb prints into the Pi at about 1,900 characters a
  second when it does nothing else (a teletext animation that prints 284
  characters a frame ran at about 6.7 frames a second), and the Pi passes that
  on to the client in full; output reaches the client in batches of up to a
  kilobyte or eight thousandths of a second, too short to notice when typing.
- **The ROM filing system and the cassette.** While `*ROM` or `*TAPE` reads,
  the MOS sends nothing to RS423, so what `*CAT` and `*.` print is on the
  Beeb's screen only. With the auto-start ROM fitted the Pi is told, and draws
  the screen again (as F11 does) once the operation is over. MOS 1.20 also never
  gives RS423 back after a ROM filing system operation; the ROM does, the next
  time something reads the keyboard.

## With a second processor

A 6502 (or other) second processor runs the program, and the BBC itself - the
"host" - runs the MOS, the screen and the keyboard. Everything the daemon uses is on
the host, so it works with a second processor on:

- **Use the auto-start ROM.** Sideways ROMs always run on the host, so the ROM
  hooks the daemon in at every BREAK whatever processor is running the language.
  `X%=20:CALL &FC88` typed into BASIC on a second processor runs on the second
  processor, which cannot see the 1MHz bus: it does nothing. By hand, use
  `*FX147,136,20` then `*GOIO FD00` (the OSBYTE crosses to the host, and `*GOIO`
  runs the helper there).
- **Output and keys go through the host's MOS** as they always do with a second
  processor: what the program prints reaches the host's OSWRCH and is copied to
  the Pi, and what you type goes into the host's keyboard buffer.
- **The screen read (F11) runs on the host**, inside the host's keyboard read, so
  it works whichever processor runs the program.
- **If the second processor's start-up puts the host's vectors back** after the
  ROM has hooked them, the ROM hooks them in again the next time the keyboard is
  read (see the auto-start ROM above).
- **ESC from the terminal stops a program on the second processor.** The escape
  flag lives in both machines; the Pi raises it the way the MOS does, which also
  tells the second processor.
- **F12 (BREAK) resets the second processor too**, as the BREAK key does (ROM build
  6 and later). Without that the second processor's start-up waited for ever.
- **Not covered:** a program on the second processor that draws by writing to
  the host's screen memory across the Tube, as a host program writing to the
  screen directly, is not seen.

**Tested on a real BBC Model B with a 6502 second processor** (ROM build 7): the
screen, boot, editing and demo test suites (see the developer notes), F12 twice,
ESC stopping a running program, and flashing the ROM itself. One fault is still
open: earlier on the day of the test, with BASIC on the second processor in MODE
5, the Beeb now and then stopped taking keys from the terminal just after
printing its prompt (7 times in about 100 runs of a scrolling loop). The screen
was intact, but the Beeb had to be switched off and on. It has not happened since,
in 240 more runs with the same firmware, so the cause is not known. If it happens
to you, please note the screen MODE and what was running.

## RS423

The daemon works on the MOS's RS423 *buffers* (what `*FX3,1` copies the screen
into, and what the Pi's redirector empties), not on the serial socket. So:

- **While helper 20 is in, a lead on the Beeb's real RS423 port sees nothing.** The
  6850 chip is silenced (its interrupts off, its baud rate and format left as they
  were, so handing the port back works). Baud-rate settings (`*FX7`, `*FX8`,
  `*CONFIGURE BAUD`) make no difference to the telnet link.
- A program that drives the 6850 directly, not through the MOS, does not see the
  redirect either.
- **A plain serial console on the real port, with no Pi, is a separate thing the MOS
  already does:** `*FX2,1` takes input from RS423 and `*FX3,7` sends the screen to
  it (the Master Reference Manual and the second-serial-port manual describe the
  pair). It cannot run together with the telnet daemon, and the daemon does not
  provide it. Running the two "either one at will" has been designed, not built
  (see the design record).

## Starting at power-on

Something the Beeb reads at power-on has to run helper 20: the Pi cannot start
anything on the Beeb by itself, because it is only a device on the 1MHz bus. The
way that has been tried and works, on a Master 128, is a boot file on the disc the
Master boots from (ADFS, drive 0, boot option 3 = Exec).

**The two files** (on the Master, `*BUILD` makes a text file; type its lines, then
press ESCAPE):

`!BOOT`

```
CHAIN"HELPERS"
```

`HELPERS`, a BASIC program (type it and `SAVE "HELPERS"`):

```
10REM Start the Pi1MHz telnet daemon (helper 20) once the Pi is up
20T%=TIME
30REPEAT UNTIL ?&FC88=&8E OR TIME-T%>1000
40IF ?&FC88=&8E THEN X%=20:CALL &FC88
```

then `*CONFIGURE BOOT`, so that power-on and CTRL-BREAK boot the disc (`*STATUS`
shows `Boot`). If the disc already has a `!BOOT`, keep its lines after the `CHAIN`
line: when `HELPERS` ends, BASIC reads the rest of the exec file as typed input.

- **Use `CHAIN`, not `*RUN`.** A saved BASIC program's execution address is in
  the BASIC ROM, so `*RUN HELPERS` jumps into whichever ROM is paged in at the
  time. On the Master it left the MOS in a state where `*INFO`, `*DUMP` and `*TYPE`
  printed nothing but blank lines until a CTRL-BREAK.
- **The wait matters.** The Pi is powered from the Beeb, so for its first two or
  three seconds `&FC88` does not read `&8E`; line 30 waits up to ten seconds for it,
  and if the Pi never answers the boot goes on without the daemon.
- **The ROM is not part of this.** With `*CONFIGURE BOOT`, every power-on and
  CTRL-BREAK runs `!BOOT` and so re-hooks the daemon. Loading the ROM from `!BOOT`
  as well would add another copy to a free sideways RAM bank every time. The ROM
  still covers a plain BREAK, which does not boot the disc.
- **A boot in progress can be stopped with ESC** from a telnet session once the
  daemon is hooked, as a stuck command can with the keyboard.

Measured on a Master 128: after a CTRL-BREAK with this boot file and
`*CONFIGURE BOOT`, the Pi saw its first helper selection 245 ms after the Beeb's
reset, and its Serial row read `hooked (telnet)` with nothing typed. **Not yet
measured:** the same after a power-off, where the Pi has only just started.

The same two commands work from any other boot file that is `*EXEC`ed (a DFS disc
with `*OPT 4,3`, MMFS) if you start the daemon by `*FX147,136,20` and `*GO FD00`,
but wait for the Pi first; a floppy is slow enough that the wait is rarely needed.

## Security

Telnet is not encrypted. The daemon has its own login, `telnetd_user` and
`telnetd_password` in `Pi1MHz.cfg`, and asks for them first (`login:` and
`Password:`). It never uses the web interface's WebDAV user name and password:
**the telnet password crosses your network in the clear**, so give it one you use
nowhere else, and keep telnet to a network you trust.

- `telnetd_password` may be empty (`telnetd_password=` on its own line): the password
  is then just RETURN.
- Without `telnetd_user` and a `telnetd_password` line every connection is refused
  with a message saying so. `telnetd_login=0` opens the daemon to everyone on the network
  instead, with no login.
- Each wrong password holds that session for 2 seconds. Every third wrong password,
  on whichever connections they came (they are forgotten after 10 minutes without
  one), closes the session and refuses new ones for a minute.
- A connection still at the login prompt is closed after 30 seconds, and a new
  connection takes its place at once, so nobody can keep you out by connecting and
  waiting.
- The Beeb's boot banners and screen are sent only after a login.

## Settings

| Key | Default | Meaning |
|---|---|---|
| `telnetd_port` | (off) | TCP port to listen on. `23` is the telnet port. Setting it turns TelnetD on; absent or `0` turns it off. |
| `Telnetd_addr` | (none) | `-1` turns TelnetD off whatever else is set, as `<name>_addr=-1` does for the other devices (Teletext, Harddisc ...). |
| `telnetd_user` | (none) | The login's user name (up to 64 characters). |
| `telnetd_password` | (none) | The login's password (up to 64 characters). It may be empty (`telnetd_password=` on its own): the password is then just RETURN. Without a user name and this line, connections are refused unless `telnetd_login=0`. |
| `telnetd_login` | `1` | `0`: no login at all, open to the network. |
| `telnetd_ansi` | `1` | `0` sends the Beeb's output exactly as it is, with no ANSI. For a program that does its own VDU handling. |
| `telnetd_utf8` | `1` | The pound sign as UTF-8; `0` sends it as Latin-1. |
| `telnetd_mode` | `7` | The Beeb's screen mode (0-7) until it sends a `MODE`. Its size cannot be read, and the daemon needs it to follow the text. |
| `telnetd_copycursor` | `1` | `0` does not mark the copy cursor in the terminal. |
| `telnetd_tek` | `1` | Draw the Beeb's graphics in xterm's Tektronix window when the terminal says it is xterm; `0`: never. |
| `telnetd_debug` | `0` | `1` shows TelnetD's diagnostics on the status page (keys waiting, the MODE it follows and why it last changed, the TCP state). Without it the status shows the port, the sessions and, only when it happens, BBC NOT TAKING KEYS. |

`/status` on the web interface has a **Telnetd** row (the port, whether a
session is live, who from) and the **Serial** row says `hooked (telnet)`
while helper 20 is in.
