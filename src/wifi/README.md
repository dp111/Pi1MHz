# WiFi Subsystem

Working bare-metal WiFi + HTTP service for the Raspberry Pi Zero W / Zero 2 W.
The board associates with WPA2 networks on its own, runs lwIP for TCP/UDP, and
exposes a small HTTP file browser, mDNS/NetBIOS name responders and a status
page on the same network.

## Hardware Support

- **Pi Zero W / WH** (CYW43438, BCM43430 firmware) — the primary target.
- **Pi Zero 2 W** (CYW43436, same SDIO-side bring-up) — works with the same
  firmware path.
- **Pi Zero (no W)** — no onboard radio; this subsystem stays disabled.

The WiFi SDIO controller uses the bcm2835 Arasan EMMC block on GPIO34–GPIO39;
SD-card storage moves to a polled SDHOST path on GPIO48–GPIO53. The two are
fully independent (see `src/rpi/sdcard.c` for the storage side).

## Layout

```
wifi/
  wifi.{c,h}          state machine, boot stages, single-slot poll dispatcher
  wifi_lwip.{c,h}     lwIP netif glue, TX/RX path, DHCP / static config
  webserver.{c,h}     HTTP/1.1 file browser, /status, /reboot, /framebuffer
  netname.{c,h}       NetBIOS (UDP 137) and mDNS (UDP 5353) responders
  framebuffer_export.{c,h}   BMP encoder for /framebuffer.bmp
  cyw43.{c,h}         CYW43 firmware/NVRAM/CLM loaders + NVRAM MAC patcher
  sdio.{c,h}          SDIO transport + SDPCM/CDC + BCM43430 join state machine
  sdio_host.{c,h}     bcm2835 Arasan EMMC backend (PIO; no SDMA)
  lwipopts.h          per-project lwIP config (MEM 32K, TCP_MSS 1460, ...)
  arch/cc.h           lwIP arch port (LWIP_RAND from system timer,
                      LWIP_PLATFORM_ASSERT logs + reboots)
  lwip/               vendored lwIP submodule, pinned to STABLE-2_2_1_RELEASE
```

## Boot Flow

`wifi_init` is called once at startup from `Pi1MHz.c` (the `wifi` entry of
the emulator table) and is idempotent across BBC resets. After
`wifi_validate_config()` accepts the cmdline a single dispatcher,
`wifi_dispatch_poll()`, is registered with the main poll table. Every
WiFi sub-system (`wifi_boot`, `wifi_lwip_poll`, `webserver_poll`,
`netname_poll`) is called from that one dispatcher; each early-returns
until its sub-system is initialised, so the whole stack costs one slot
in the main poll table.

`wifi_boot()` runs a small state machine across ticks so no single
callback stalls the 1 MHz bus loop:

```
IDLE → START_SDIO → WAIT_SDIO → OPTIONAL_PROBE → INIT_LWIP → INIT_WEBSERVER → COMPLETE
                          │
                          └─ runs sdio_runtime_tick() internally:
                             OPEN_HOST → IDENTIFY_CARD → READ_CCCR → ENABLE_FUNCTIONS
                             → REQUEST_ALP → READ_POWER → WAKE_KSO → BOOT_FIRMWARE
                             → READ_MAILBOX → ACK_INTERRUPTS → WRITE_INTR_MASK
                             → PREPARE_JOIN → CLM_DOWNLOAD → JOIN → SWEEP_RX → DONE
```

### NVRAM MAC patch

Before the CYW43 firmware is downloaded, `wifi_preload_images()` calls
`rpi_get_board_mac()` (VC4 mailbox tag `0x10003`) to fetch the
Foundation-style MAC the VC4 bootcode derived from the SoC's board-serial
OTP — the same `B8:27:EB:xx:xx:xx` / `DC:A6:32:xx:xx:xx` Pi-OS would use
on this board. `cyw43_patch_nvram_macaddr()` appends a `macaddr=` line to
the in-memory brcmfmac NVRAM; the chip then transmits with that address
and lwIP's netif advertises the same one. If the mailbox query or the
patch fails the chip falls back to its factory Cypress-OUI OTP MAC; WiFi
still works, the MAC just looks like a Cypress address rather than a
Foundation one.

### Join sequence

A faithful port of PicoWi's `join_start` + `join_restart` for WPA2-PSK,
with a few Pi1MHz-specific additions (MPC_OFF, POWERSAVE_OFF, ROAM_OFF,
the three-form event-mask setup, and read-back diagnostics for
`country` / `radio_disable` / `bssid` / `ssid` / `wsec` / `wpa_auth` /
`auth` / `infra` / `sup_wpa`). The sequence is built once in
`sdio_tx_probe_join_commands()` and then advanced one ioctl per tick
(`STAGE_JOIN`) with the per-command settle delays PicoWi uses
(150 ms after the AMPDU block, 100 ms after each `WLC_UP`, 50 ms after
events_enable and mcast_list, 10 ms inter-ioctl elsewhere).

## Cmdline Parameters

All entries are optional except `wifi_ssid` (without an SSID the whole
stack stays down). Legacy names from the original feature request are
accepted alongside the `wifi_*` form.

| Key | Default | Notes |
|---|---|---|
| `wifi_ssid=` / `SSID=`                | (none)            | Required; without it the WiFi stack does not start. |
| `wifi_password=` / `SSIDpassword=`   | (none)            | WPA2-PSK passphrase, 8–63 ASCII characters. |
| `wifi_hostname=`                     | `Pi1MHz`          | NetBIOS / mDNS name; embedded `.` is mapped to `-`. |
| `wifi_country=`                      | `GB`              | ISO 3166 country code for the brcmfmac regulatory iovar (e.g. `US`, `DE`). |
| `wifi_ip=`                           | `dhcp`            | `dhcp` or a literal IPv4 address (e.g. `192.168.1.40`). |
| `wifi_netmask=`                      | `255.255.255.0`   | Only used with static `wifi_ip=`. |
| `wifi_gateway=`                      | (none)            | Only used with static `wifi_ip=`. |
| `wifi_dns=`                          | (none)            | Only used with static `wifi_ip=`. |
| `wifi_http_port=`                    | `80`              | TCP port the webserver listens on. |
| `wifi_debug=`                        | (off)             | `1` (or any non-zero) enables verbose `WIFI:` / `WIFI-LWIP:` / `WIFI-SDIO:` debug to the aux UART. |
| `wifi_emulator=`                     | (off)             | `1` keeps the lenient "proceed anyway" path when the CYW43 firmware never reports HT_AVAIL / fn2-ready. Useful only under an SDIO emulator with no real chip; leave off on real hardware. |
| `wifi_sdio_probe=`                   | (off)             | `1` runs a diagnostic CMD5/CCCR/function-enable probe in the boot path. |
| `wifi_sdio_tx_probe=`                | (off)             | `1` adds a single gated function-2 CMD53 control-frame write during the SDIO probe. |
| `wifi_sdio_tx_probe_command=`        | `version`         | Picks the diagnostic ioctl the TX probe sends: `version`, `magic`, `up`, `infra`, `auth`, `ssid`, `wpa_auth`, `wsec`, `pmk`, `join`. |
| `wifi_sdio_rx_sweep_limit=`          | `16`              | Maximum number of function-2 frames the bounded receive sweep will decode after the TX probe. |

Example (DHCP):

```
wifi_ssid=PiNet wifi_password=secret123 wifi_country=GB
```

Example (static):

```
wifi_ssid=PiNet wifi_password=secret123 wifi_ip=192.168.1.40 wifi_netmask=255.255.255.0 wifi_gateway=192.168.1.1 wifi_dns=192.168.1.1 wifi_country=GB
```

## Network Configuration

- **DHCP** is the default; the boot-time link timeout (30 s) catches a
  join that never associates, and a `dhcp_start` failure is reported via
  `wifi_set_error` rather than silently going nowhere.
- **Static** addressing accepts `wifi_ip`, `wifi_netmask`, optional
  `wifi_gateway`, optional `wifi_dns`.
- A NetBIOS name responder answers Windows broadcast queries on UDP 137,
  so `http://Pi1MHz/` works from Windows without `.local`.
- An mDNS responder multicasts an unsolicited `<hostname>.local`
  A-record every ~20 s. The lwIP build has no IGMP, so the Pi can't
  *receive* mDNS queries, but periodic unsolicited announcements are
  cached by macOS / iOS / Linux / Windows 10+ resolvers, so
  `http://Pi1MHz.local/` resolves.

## Web Pages

- `/`                — home page with links.
- `/status`          — WiFi / network state, MAC, IP, gateway, frame
                       counters, SD-card free space (cached on a 5 s TTL
                       so the TCP callback is not stalled by FatFs).
- `/files/...`       — browse the SD card, download files, upload files
                       (`multipart/form-data`, streamed straight to FatFs).
- `/framebuffer`     — preview the live BBC VDU output as an embedded BMP.
- `/framebuffer.bmp` — 24-bit BMP of the current framebuffer (rows
                       streamed on demand to keep TCP buffers bounded).
- `/reboot`          — confirm + POST → defer ~1.5 s → `reboot_now()`.

The HTTP layer is one request per connection (`Connection: close`),
built directly on the lwIP raw-TCP API. URL paths are percent-decoded
and `..` segments are rejected at the parser, so the file browser cannot
escape the SD root.

## Implementation Notes

- **Single-slot poll dispatch.** Every WiFi sub-system hangs off
  `wifi_dispatch_poll()`, so the WiFi stack consumes exactly one entry
  in `Pi1MHz_poll_table`. Sub-polls have early-exit guards
  (`stage == IDLE`, `timers_running`, `g_ws_reboot_pending`, `g_ready`)
  so calling them before their sub-system is initialised is safe.
- **No retained heap allocations on the data path.** The SDIO transient
  buffers (`tx_frame`, `frame_buffer`, `scratch`, the CLM staging
  buffer) are all `static _Alignas(4) uint8_t[SDIO_RUNTIME_MAX_FRAME_SIZE]`
  to keep the deep RX call chain off the bare-metal stack and to satisfy
  the 32-bit EMMC PIO loop's alignment requirement.
- **DHCP / TCP randomness.** `LWIP_RAND()` mixes the free-running 1 MHz
  system timer with the golden-ratio constant 2654435761, so every call
  returns a distinct value and per-boot starting points are
  unpredictable; the original constant `0x12345678` made DHCP XIDs and
  TCP ISNs identical across boots.
- **`tcp_abort` semantics.** `conn_close` returns `bool aborted`;
  `ws_sent`, `ws_poll` and `ws_recv` propagate that and return
  `ERR_ABRT` whenever the abort fallback fires, so lwIP never touches a
  freed pcb.
- **lwIP assert handler.** A failed `LWIP_PLATFORM_ASSERT` logs the
  expression + file + line via the aux UART and reboots, rather than
  spinning silently.

## Files Required on the SD Card

```
/Pi1MHz/wifi/
  brcmfmac43430-sdio.bin       firmware (Cypress / brcmfmac, ~419 KB)
  brcmfmac43430-sdio.txt       NVRAM      (the macaddr= line is added at runtime)
  brcmfmac43430-sdio.clm_blob  CLM regulatory database (optional but recommended)
```

If `.clm_blob` is missing the chip uses its built-in minimal regulatory
data; the WiFi stack still comes up but country-locked channels may be
unavailable. If the `.bin` or NVRAM is missing the WiFi stack reports an
error and stays down — everything else on the Pi1MHz keeps running.

## Not Implemented

- **TLS / HTTPS.** Webserver is plain HTTP only.
- **WPA3 / Enterprise / 802.1X.** WPA2-PSK only.
- **FujiNet protocol bridge.** Not started yet; intended to layer on top
  of the working TCP/IP stack as a separate sub-system rather than as
  part of the management UI.

## See also

