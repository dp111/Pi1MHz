# Teletext

Pi1MHz emulates the Acorn Teletext Adapter - the box that let a Beeb
receive and display broadcast teletext pages (CEEFAX, ORACLE) and
download telesoftware. Since there are no teletext broadcasts any
more, the page stream comes over [WiFi](wifi.md) from an internet
teletext server instead.

The emulated adapter appears at the real adapter's addresses
(`&FC10-&FC13`), so period teletext software sees what it expects.

## What you need

- [WiFi](wifi.md) working.
- The address of a teletext ("t42" format) stream server. These are
  run by teletext enthusiasts - the same servers used with BeebEm's
  teletext support work with Pi1MHz. The vhs-teletext community is a
  good place to find current ones.
- Teletext software on the Beeb - see the next section.

## The ATS ROM

Pi1MHz ships the BBC's own Advanced Teletext System ROM ((C) BBC
1988), the period software for the Acorn Teletext Adapter, as
`/Pi1MHz/ATS.rom` on the SD card. Load it into sideways RAM with
helper 7 (see [Loading ROMs](helpers-and-roms.md)):

```
X%=7 : CALL &FC88
```

or `*FX147,136,7` then `*GO FD00` (`*GOIO FD00` with a Tube second
processor), followed by **CTRL-BREAK** so the OS notices the ROM.

It provides the full ATS command set: `*TELETEXT` starts the page
viewer (channel, magazine and page selection from the keyboard), and
commands such as `*PAGE`, `*MAGAZINE`, `*DATE` and `*TELESOFT` /
`*TRANSFER` fetch pages and download telesoftware from the stream.
Because the emulated adapter behaves like the real hardware, other
period teletext software works too - ATS is simply the one included.

## Configuration

Up to four channels (the adapter's channel selector positions,
think "CEEFAX 1-4") can each be given a server in
`/Pi1MHz/Pi1MHz.cfg`:

```
teletext_server1=203.0.113.5:19761
teletext_server2=203.0.113.9
```

The `:port` part is optional; 19761 is the usual teletext-server
port. Use the server's IP address (not a name).

## Notes

- Pi1MHz connects to the configured servers over TCP and keeps a few
  fields buffered, reconnecting automatically if the link drops.
- The emulation reproduces the adapter's 50Hz field timing and
  interrupts, so page capture behaves like the real thing - including
  needing a moment to catch the page you asked for as it "broadcasts"
  past.
