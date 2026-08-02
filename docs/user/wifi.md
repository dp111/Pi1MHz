# WiFi Setup

On a WiFi-equipped Pi (Zero W, Zero 2 W, 3B+), Pi1MHz can join your
home network. That gives you:

- the [web interface](web-interface.md) - manage the SD card from a
  browser, view the screen, reboot the Pi
- WebDAV - mount the SD card as a network drive on a PC or Mac
- [Econet over WiFi](econet-aun.md) for the Beeb
- [Internet teletext](teletext.md)

WiFi is entirely optional. Without it, everything else still works.

## What the WiFi supports

- 2.4GHz networks with **WPA2 personal** (ordinary home password
  security). WPA3-only and enterprise/login-page networks are not
  supported, and there is no HTTPS - treat it as a device for your own
  trusted network.

## Turning it on

Edit `/Pi1MHz/Pi1MHz.cfg` on the SD card and set at least:

```
wifi_ssid=YourNetworkName
wifi_password=YourNetworkPassword
```

WiFi does not start at all until `wifi_ssid` is set.

If you are outside the UK, also set your country code (it affects
which radio channels may be used):

```
wifi_country=US
```

Then power-cycle. Allow up to about 30 seconds after power-on for the
Pi to join the network and get an address.

## Finding the Pi on your network

By default the Pi asks your router for an address (DHCP) and announces
itself under the name `Pi1MHz`. Try, from another computer on the same
network:

- `http://pi1mhz.local/` (Mac, Linux, iOS, recent Windows)
- `http://Pi1MHz/` (Windows)
- or look in your router's device list for `Pi1MHz` and use its IP
  address directly.

Change the name with `wifi_hostname=` if you have more than one, or if
the name lookup is unreliable on your network.

### Fixed address

If you prefer a fixed address (recommended if you use Econet or
teletext, so nothing moves):

```
wifi_ip=192.168.1.40
wifi_netmask=255.255.255.0
wifi_gateway=192.168.1.1
wifi_dns=192.168.1.1
```

Pick an address outside your router's DHCP range. `wifi_gateway` and
`wifi_dns` are only needed if the Pi must reach beyond your own network
(for example internet teletext servers).

## The WiFi chip firmware files

The WiFi chip needs its manufacturer's firmware files, which Pi1MHz
loads from `/Pi1MHz/wifi/` on the SD card. **The standard firmware set
already includes them** for the Pi Zero W (43430), Pi Zero 2 W (43436)
and Pi 3B+ (43455), so normally there is nothing to do - this section
matters only if you built the card by hand.

The right set is picked automatically for the board you boot on:

| Board | Files needed under `/Pi1MHz/wifi/` |
|---|---|
| Pi Zero W | `brcmfmac43430-sdio.bin`, `.txt`, `.clm_blob` |
| Pi Zero 2 W | `brcmfmac43436-sdio.bin`, `.txt`, `.clm_blob` |
| Pi 3B+ | `brcmfmac43455-sdio.bin`, `.txt`, `.clm_blob` |

If they are missing for your board, WiFi reports an error and stays
off; everything else keeps working. The files come from the Raspberry
Pi firmware-nonfree repository (or from `/lib/firmware/brcm/` on any
Raspberry Pi OS installation).

## Password protection

Out of the box the web interface and WebDAV are open to anyone on your
network. To require a login:

```
webdav_user=me
webdav_password=something-secret
```

Both must be set; the login then applies to every web page and WebDAV
operation.

## If it doesn't connect

See [Troubleshooting](troubleshooting.md#wifi). The short version:
double-check the SSID and password, make sure the network is 2.4GHz
WPA2, set `wifi_country`, and give it 30 seconds after power-on.
