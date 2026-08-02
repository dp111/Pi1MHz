# Configuration Reference

Almost everything is configured in one plain-text file on the SD card:

```
/Pi1MHz/Pi1MHz.cfg
```

It is read once when the Pi starts up, so after changing it either
power-cycle the machine or reboot the Pi from the web interface.

Two settings live elsewhere, in `cmdline.txt` in the SD card root,
because they are needed before the SD card's filing system is fully up -
see [cmdline.txt settings](#cmdlinetxt-settings) at the end.

## File format

- One setting per line. The key must start at the very beginning of the
  line (a line starting with a space or tab is ignored).
- `key=value` or `key value` both work. The value is the rest of the
  line and may contain spaces.
- `#` starts a comment, either a whole line or after a value.
- Keys are not case-sensitive.
- A key on its own (no value) counts as "set" - some debug switches
  work this way.

Example:

```
# Join my network and give the Pi a fixed name
wifi_ssid=HomeNet
wifi_password=correct horse battery staple
wifi_hostname=beeb

SCSIJUKE=0          # start with the /BeebSCSI0 disc set
watchdog=10         # auto-reboot the Pi if it ever locks up
```

The shipped `Pi1MHz.cfg` contains a commented-out example of most keys.

---

## Enabling, disabling and moving emulators

Each emulated device can be disabled, and those that occupy addresses
in FRED (the `&FCxx` page) can be moved. The key is the emulator's name
plus `_addr`:

- `<name>_addr=0xNN` - move the device to base address `&FCNN`
  (this also switches on a device that defaults to off, like BeebSID)
- `<name>_addr=-1` - disable the device completely

| Key | Default base | Device |
|---|---|---|
| `Helpers_addr` | `0x88` | Helper functions at `&FC88` |
| `Rampage_addr` | `0xFD` | Paged JIM RAM registers at `&FCFD-&FCFF` |
| `Rambyte_addr` | `0x00` | Byte-access RAM registers at `&FC00-&FC03` |
| `Harddisc_addr` | `0x40` | SCSI hard disc at `&FC40-&FC43` |
| `M5000_addr` | (none) | Music 5000/3000 (uses JIM paging, no FRED base) |
| `BeebSID_addr` | `0x20`, **off by default** | SID chip at `&FC20` - set an address to enable |
| `Services_addr` | `0xA6` | The services port at `&FCA6-&FCAA`: SD card / FAT access plus the Econet AUN commands |
| `Videoplayer_addr` | (none) | Video background plane |
| `Framebuffer_addr` | `0xA0` | HDMI framebuffer / VDU port at `&FCA0` |
| `Mouseredirect_addr` | `0xAC` | Mouse pointer registers at `&FCAC-&FCAF` |
| `usb_addr` | (none) | USB MTP file access |
| `wifi_addr` | (none) | WiFi stack |
| `aun_addr` | (none) | Econet-over-WiFi engine |
| `Teletext_addr` | `0x10` | Acorn Teletext Adapter at `&FC10-&FC13` |
| `Watchdog_addr` | (none) | Watchdog (use the `watchdog` key below instead) |

Example - you have real hardware at `&FC40` and want the Pi1MHz hard
disc out of the way:

```
Harddisc_addr=0x50
```

or off entirely:

```
Harddisc_addr=-1
```

---

## Core settings

| Key | Default | Meaning |
|---|---|---|
| `Pi1MHznOE` | `1` | Set `0` if your interface board has no external output-enable (nOE) pin on its data bus buffer. `1` (the default) drives the nOE pin, which also lets Pi1MHz share the 1MHz bus with other devices. Which one you need depends on the board - if the shipped default works, leave it alone. |
| `watchdog` | off | A number of seconds (1-15). If set, the Pi's hardware watchdog reboots it automatically should the firmware ever lock up. `0` or absent = off. `watchdog=10` is a sensible value if you want it. |
| `BeebAudio_Off` | off | `1` mutes the emulated audio path into the BBC's internal speaker. For the Music 5000 on a Pi 3B+ this also enables proper stereo on the Pi's headphone jack. Applies to whichever audio emulator is running (Music 5000 or BeebSID). |

## Hard disc settings

See [Hard discs](hard-discs.md) for what these mean in practice.

| Key | Default | Meaning |
|---|---|---|
| `SCSIJUKE` | `0` | Which `/BeebSCSIn` directory (disc set) to use at power-on. |
| `VFSJUKE` | `0` | Which `/BeebVFSn` directory to use for VFS volumes at power-on. |
| `SCSIID` | `0` | SCSI ID the emulation answers to. `0` (default) answers every ID. Only relevant if you run more than one SCSI adapter on a Master. |

## Sound settings

See [Sound](sound.md).

| Key | Default | Meaning |
|---|---|---|
| `M5000_Gain` | `3` | Music 5000 output gain. Add 1000 to the value to also switch off automatic scaling (which normally reduces gain if the output clips) - e.g. `M5000_Gain=1016` means gain 16, no auto-scaling. |
| `BeebSID_addr` | off | Set `0x20` to enable the SID chip at `&FC20`. Enabling BeebSID disables the Music 5000 - they share the audio output. |

## WiFi settings

See [WiFi setup](wifi.md). WiFi only starts at all when `wifi_ssid` is
set.

| Key | Default | Meaning |
|---|---|---|
| `wifi_ssid` | (none) | Name of the network to join. **Required** for any WiFi function. (`SSID` is accepted as an older alias.) |
| `wifi_password` | (none) | WPA2 passphrase, 8-63 characters. (Alias: `SSIDpassword`.) |
| `wifi_hostname` | `Pi1MHz` | Network name. Lets you browse to `http://Pi1MHz/` (Windows) or `http://Pi1MHz.local/` (Mac/Linux/iOS). |
| `wifi_country` | `GB` | Two-letter ISO country code for radio regulations, e.g. `US`, `DE`. Set it to where the machine actually is. |
| `wifi_ip` | `dhcp` | `dhcp` to get an address from your router (the default), or a literal address such as `192.168.1.40` for a fixed one. |
| `static_ip` | (none) | Alternative way to give a fixed address: set this and leave `wifi_ip` unset. |
| `wifi_netmask` | `255.255.255.0` | Only used with a fixed address. |
| `wifi_gateway` | (none) | Only used with a fixed address. Needed for anything beyond your own network. |
| `wifi_dns` | (none) | Only used with a fixed address. |
| `wifi_http_port` | `80` | TCP port for the built-in web server. |
| `wifi_debug` | off | `1` prints verbose WiFi logging on the serial port. Only does anything on a debug build of the firmware - release builds print nothing. |

There are also several `wifi_sdio_*` keys and `wifi_emulator`. These
are hardware-diagnostic switches for developers; leave them unset.

## Web interface / WebDAV settings

See [The web interface](web-interface.md).

| Key | Default | Meaning |
|---|---|---|
| `webdav_user` | (none) | Username for the web interface and WebDAV. Password protection is only enforced when **both** user and password are set; otherwise everything is open to your network. |
| `webdav_password` | (none) | The matching password. |
| `webdav_realm` | `Pi1MHz` | The name shown in the login prompt. Rarely needs changing. |
| `webdav_utc_offset_minutes` | `0` | Your timezone as minutes east of UTC (e.g. `60` for BST, `-300` for US Eastern). Used so file dates shown over WebDAV come out in local time. |

## Econet (AUN) settings

See [Econet over WiFi](econet-aun.md).

| Key | Default | Meaning |
|---|---|---|
| `aun_station` | `0.32` | This machine's Econet station number, as `net.station` (e.g. `1.42`) or just `42`. The special value `ip` takes the station number from the last part of the Pi's IP address; `ip.ip` takes the network number from the third part too. |
| `aun_port` | `32768` | UDP port to listen on. 32768 is the AUN standard. |
| `aun_map` | (none) | Where to find other stations, as a comma-separated list of `net.stn=ip` or `net.stn=ip:port` entries. Example: `aun_map=1.254=192.168.1.10,1.200=192.168.1.11:32769` |
| `aun_learn` | (none) | A network number (e.g. `2`). Stations on that net are learned automatically from incoming traffic instead of needing `aun_map` entries. |
| `aun_machine` | (none) | Eight hex digits sent as the reply to a "machine peek". Leave unset unless a bridge needs a specific machine type. |
| `aun_debug` | off | `1` logs Econet events on the serial port (debug builds only). |

## Teletext settings

See [Teletext](teletext.md).

| Key | Default | Meaning |
|---|---|---|
| `teletext_server1` | (none) | TCP source for channel 1, as `ip.address:port`. The port is optional and defaults to 19761. |
| `teletext_server2` | (none) | Channel 2 source. |
| `teletext_server3` | (none) | Channel 3 source. |
| `teletext_server4` | (none) | Channel 4 source. |
| `teletext_debug` | off | Log teletext events (debug builds only). |

---

## cmdline.txt settings

`cmdline.txt` (a single-line text file in the SD card root; create it
if it does not exist) is read before the SD card's filing system comes
up, which is why these two cannot live in `Pi1MHz.cfg`:

| Key | Default | Meaning |
|---|---|---|
| `disk_led_gpio=NN` | off | Flash an LED on Pi GPIO pin NN during disc activity. Traditionally written with a Pi-model prefix, e.g. `bcm2708.disk_led_gpio=16` (Pi Zero) or `bcm2709.disk_led_gpio=16` (Pi 3) - both prefixed and bare forms are recognised. |
| `baud_rate=NNNN` | `115200` | Speed of the serial debug port. Only relevant if you attach a serial lead. |

Everything on the `cmdline.txt` line goes in one line, separated by
spaces.

## config.txt

`config.txt` in the SD root is the standard Raspberry Pi firmware
configuration and is already set up correctly. The only line you might
ever change is near the top: un-commenting `kernel=debug/kernel.img`
(or `kernel=debug/kernel7.img` in the `[pi3]` section) boots the debug
build of Pi1MHz, which prints diagnostic messages on the serial port.
See [Troubleshooting](troubleshooting.md).
