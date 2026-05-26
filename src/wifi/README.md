# WiFi Subsystem

This directory is the in-tree home for a future bare-metal WiFi and HTTP service.

## Hardware Scope

- Raspberry Pi Zero: no onboard WiFi. A bare-metal WiFi stack on a plain Zero needs an external USB or SDIO WiFi device.
- Raspberry Pi Zero W / WH: onboard CYW43438/BCM43430 WiFi is present and is the practical target for this code.

## Current Hardware Constraint

Circle's working Pi Zero W model uses the BCM2835 Arasan EMMC/SDIO controller for onboard WLAN and routes that controller to GPIO34-GPIO39, while SD-card access moves to a separate SDHOST path. This Pi1MHz tree now mirrors that split in [src/rpi/sdcard.c](../rpi/sdcard.c): normal storage traffic uses a polled SDHOST path on GPIO48-GPIO53, while the WiFi SDIO backend uses raw Arasan commands on GPIO34-GPIO39. The remaining blockers are now above the controller split: robust CYW43438 bring-up, firmware loading, and network-stack integration.

## Why This Is Split Into Layers

The WiFi work is substantially larger than a simple HTTP server. The stack needs these pieces before a browser can talk to the board:

1. SDIO transport for the CYW43438/BCM43430.
2. Firmware and NVRAM loading for the WiFi chip.
3. Control/data path to the FullMAC firmware.
4. TCP/IP integration, likely via lwIP.
5. HTTP request parsing and page generation.

## lwIP Location

The project now owns lwIP directly under `src/wifi/lwip` instead of building against TinyUSB's former nested `lib/lwip` checkout.

- The top-level build links against `src/wifi/lwip`.
- TinyUSB's nested lwIP submodule is no longer required for this repo's firmware build.
- The current pinned lwIP revision is `STABLE-2_2_1_RELEASE`.

This subtree currently provides the project-native parts that can be built now without pretending the radio bring-up is solved:

- `wifi.c/.h`: `cmdline.txt` parsing via `get_cmdline_prop()`, subsystem state, and staged bring-up hooks.
- `wifi.c/.h`: parsed `wifi_network_config_t` output for direct consumption by a future lwIP `netif` setup path.
- `sdio.c/.h`: SDIO CMD52/CMD53 argument builders plus probe/orchestration logic.
- `sdio_host.c/.h`: WiFi-local host backend seam. It now drives the raw Arasan WLAN path directly while keeping the SDIO protocol layer isolated from board-specific register handling.
- `webserver.c/.h`: HTML response generation for status, files, and framebuffer pages using the existing filesystem and framebuffer modules.

## Cmdline Parameters

The loader accepts either the new names or the legacy names from the original request:

- `wifi_enable=1`
- `wifi_ssid=YourSSID` or `SSID=YourSSID`
- `wifi_password=YourPassword` or `SSIDpassword=YourPassword`
- `wifi_hostname=pi1mhz`
- `wifi_ip=dhcp` for DHCP, or `wifi_ip=192.168.1.40` for a static address
- `wifi_netmask=255.255.255.0` for static addressing
- `wifi_gateway=192.168.1.1` for static addressing
- `wifi_dns=192.168.1.1` for static addressing
- `wifi_http_port=80`
- `wifi_sdio_probe=1` to run a diagnostic CMD5 SDIO OCR probe during boot
- `wifi_sdio_tx_probe=1` to opt into one minimal function-2 CMD53 control-frame write during the SDIO probe
- `wifi_sdio_rx_sweep_limit=4` to control how many function-2 frames the bounded receive sweep will decode after a transmit probe
- `wifi_sdio_tx_probe_command=version`, `magic`, `up`, `infra`, `auth`, `ssid`, `wpa_auth`, `wsec`, `pmk`, or `join` to choose which gated ioctl the SDIO transmit probe sends

Example:

```
wifi_enable=1 wifi_ssid=PiNet wifi_password=secret123 wifi_ip=192.168.1.40 wifi_netmask=255.255.255.0 wifi_gateway=192.168.1.1 wifi_dns=192.168.1.1 wifi_http_port=80
```

If `wifi_ip` is omitted or set to `dhcp`, the future network stack should use DHCP.

If `wifi_sdio_probe=1` is set, the probe now reaches the raw Arasan WLAN backend and can attempt CMD5/CMD52/CMD53 traffic. That still does not mean WiFi association works yet: the remaining work is the CYW43438 bring-up sequence, firmware/NVRAM loading, and the higher-level control/data path above the transport layer.

## Web Pages

The current webserver layer builds these pages once a TCP listener is wired in:

- `/` main menu
- `/status` build, board, and WiFi state
- `/files` root directory listing and planned upload/download actions
- `/framebuffer` current mode information and planned PNG export path

## Not Implemented Yet

- CYW43438 SDIO driver bring-up sequence on top of the raw CMD52/CMD53 transport wrappers
- Firmware loading
- WPA/WPA2 join logic
- lwIP network interface
- Multipart upload handling
- Framebuffer to PNG encoding
- FujiNet protocol bridge

## FujiNet Direction

The likely layering for FujiNet is:

1. Get stable WiFi association and TCP/IP.
2. Land the HTTP management interface for diagnostics and file movement.
3. Add a FujiNet-facing transport shim that bridges BBC-side requests onto the network stack.

That keeps FujiNet work separate from the management web UI and avoids coupling protocol work to the first WiFi bring-up.