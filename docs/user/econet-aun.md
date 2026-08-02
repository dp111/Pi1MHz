# Econet over WiFi (AUN)

Pi1MHz can stand in for an Econet interface, carrying Econet traffic
over your home network instead of the old five-pin DIN cabling. It
uses AUN (Acorn Universal Networking), the standard scheme for Econet
over IP - the same one spoken by PiEconetBridge, BeebEm, RISC OS and
various modern fileserver projects.

In practical terms: your Beeb can log into an Econet fileserver
(`*I AM ...`, `*CAT`, `LOAD`/`SAVE`) where the "network" is your WiFi
and the fileserver is, say, a Raspberry Pi elsewhere in the house.

## What you need

- [WiFi](wifi.md) working on the Pi1MHz.
- A filing-system ROM on the Beeb. Pi1MHz ships one and can load it
  into sideways RAM (see [Loading ROMs](helpers-and-roms.md)):

  ```
  X%=8 : CALL &FC88      (BBC B version)
  X%=9 : CALL &FC88      (Master 128 version)
  ```

  then CTRL-BREAK.

  A caution: the BBC B version is the one that gets regular testing;
  the Master 128 version has had much less validation and should be
  considered experimental.

### The filing-system ROMs

Both ROMs are ports of Acorn's own Econet network filing system, with
the Econet hardware layer replaced by Pi1MHz AUN commands - so to the
machine (and to fileserver software) they behave like the genuine
article:

| File on the SD card | Machine | Based on | Announces itself as |
|---|---|---|---|
| `/Pi1MHz/AUNFSbeeb.rom` | BBC B / B+ | Acorn NFS 3.65 (the 8K NFS) | `AUN 3.65` |
| `/Pi1MHz/AUNFSM128.rom` | Master 128 | Acorn ANFS 4.26 | `AUNFS 4.26` |

### Installing the ROM with the helper

The machine needs writable sideways RAM (a Master has it built in; a
BBC B needs a sideways RAM board or fitted RAM). From BASIC:

```
X%=8 : CALL &FC88      loads AUNFSbeeb.rom  (BBC B)
X%=9 : CALL &FC88      loads AUNFSM128.rom  (Master 128)
```

or, equivalently, from the command line:

```
*FX147,136,8
*GO FD00
```

(`*GOIO FD00` on a Master, and `9` in place of `8` for the Master
ROM.) The loader scans the sideways slots from 15 downwards and loads
into the first empty writable one; `No SWR` means it found no free
sideways RAM, `No ROM` means the ROM file is missing from `/Pi1MHz` on
the Pi's SD card.

Then press **CTRL-BREAK** so the OS notices the new ROM. `*HELP`
should now list it (`AUN 3.65` or `AUNFS 4.26`).

### Using it

The standard Econet commands work as they always did: `*NET` to select
the filing system, `*I AM <user>` to log on, `*CAT`, `LOAD`/`SAVE`,
`*LOGOFF` and friends. Because these are real Acorn filing systems
underneath, software that talks to NFS or ANFS - boot options,
fileserver utilities, network games - sees what it expects.

The ROMs can also be installed as physical EPROMs or via any other
sideways ROM mechanism - the images are ordinary 16K ROM files and
nothing about them requires the helper loader.

- Something to talk to: an AUN fileserver or bridge on your network.

## Configuration

In `/Pi1MHz/Pi1MHz.cfg`:

```
aun_station=0.42
aun_map=1.254=192.168.1.10
```

### Your station number

`aun_station` sets this machine's Econet address as `net.station`
(or just a station number, with net 0). Every machine on the network
needs a different one. Two shortcuts:

- `aun_station=ip` - use the last part of the Pi's IP address as the
  station number (e.g. IP 192.168.1.42 → station 42). Handy with fixed
  addresses.
- `aun_station=ip.ip` - take the net number from the third part of the
  IP address too.

If nothing is set, the default is station 32 on net 0.

### Finding other stations

AUN carries Econet packets in UDP, normally on port 32768. Pi1MHz
needs to know which IP address each Econet station lives at:

- `aun_map=` lists them explicitly:
  `aun_map=1.254=192.168.1.10,1.200=192.168.1.11:32769`
  (that is: station 1.254 is at 192.168.1.10, and 1.200 is at
  192.168.1.11 using the non-standard port 32769).
- `aun_learn=2` says "any machine that sends to me is on net 2 -
  work out its station number from its IP address". Good for casual
  setups where everything is on one net.

`aun_port=` changes the UDP port Pi1MHz itself listens on (default
32768).

## Checking it works

- On the Beeb, the filing system ROM's own commands report the station
  and status.
- From a browser, `http://pi1mhz.local/aun` shows the engine's state:
  your station number, the address map, and counters of packets sent
  and received - the quickest way to see whether traffic is flowing.

## Tips

- If the fileserver end is a **PiEconetBridge**, configure the bridge's
  entry for the Pi1MHz station with the AUTOACK flag; without it,
  transmissions from the Beeb time out.
- Give the Pi a [fixed IP address](wifi.md#fixed-address), and fixed
  addresses to the other AUN machines, so the `aun_map` entries stay
  valid.
- `aun_machine=` (eight hex digits) sets the machine-type answer given
  to "machine peek" queries, should a bridge or monitor care.

## Limits

- This is Econet over IP, not real Econet: it cannot talk to a machine
  that only has classic Econet hardware unless a bridge (such as
  PiEconetBridge) joins the two worlds.
- WiFi latency is not clock-driven Econet latency; interactive
  performance depends on your network quality.
