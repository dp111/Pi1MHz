STATUS (2026-11-16): stages 1-5 BUILT for the Pi 3B+ (LAN7515) on branch
ethernet-3bplus, NOT hardware-tested (no bench that week).  Compiles
warning-free in rpi, rpi2, rpi3 (release + debug).  Owner chose the 3B+ first.
Defaults taken from this plan: `network=auto`, one link at a time, no
`wifi_lwip_*` rename.

As built, differing from the design below:
- No `net_link_t` ops table.  `wifi.c`'s boot state machine got a
  `CHOOSE_LINK` stage (it owns the choice); `wifi_lwip.c` picks the wired
  `linkoutput` once in `netif_init` and dispatches the service pass to
  `wifi_lwip_wired_service` or the extracted `wifi_lwip_radio_service`.
- The chip is set up by a table of register operations run as non-blocking
  control transfers from the poll loop (`src/usb_ether.c`).
- `usb_host_step()` (usb.c): the wired drain and the link choice step the USB
  host themselves, because SMB storage waits for the network inside a SCSI
  command where the poll loop's USB task does not run.
- Empty bulk IN answered by a zero-length packet (USB_CFG0.BIR), re-asked
  1 ms later: no NAK-polling of the endpoint.
- Register map written from memory (the datasheet would not download) and
  checked against Circle's lib/usb/lan7800.cpp: every address and bit
  matches.  Circle clears USB_CFG0.BIR (NAK) and runs one frame per transfer
  (no HW_CFG.MEF) where this sets both: if receive misbehaves, try Circle's.
- Review fixes (2026-11-17): a control transfer refused because the shared
  pipe is busy is retried, never taken as done; the set-up has a 5 s limit;
  the chip is set up and polled only while it may be the link
  (`usb_ether_use`, from wifi.c), and receive is asked only with a link up;
  a stalled receive endpoint is asked once a second and shown on /status
  (TinyUSB's host API has no clear-halt); `network=ethernet` with no chip
  after 8 s fails the boot (the ERROR retry tries again).
- Behaviour change for the owner: a 3B+ with no wifi_ssid now starts the
  network under `auto` (DHCP on the cable), where before it had none.

Hardware test list (each with a control): Zero 2 W / no-chip board boots and
joins WiFi exactly as before (control for every shared-code change); 3B+ with
`network=wifi` unchanged; 3B+ `auto` + cable -> Ethernet, DHCP, web UI, SMB,
VFS video; `auto` without cable + SSID -> WiFi after ~8 s; cable pulled and
replaced -> link down/up on /status and DHCP recovers; idle poll-loop pass time
with the chip set up vs `network=wifi` on the same board.

Original plan follows.

# Wired Ethernet on the Pi 3 Model B (and the other Model Bs)

## What the hardware is

The Model B boards have no Ethernet MAC in the SoC.  The RJ45 socket sits
behind a USB chip that is also the board's USB hub:

| Board | Chip | USB ID | Linux driver (reference only) |
|---|---|---|---|
| Pi 1 B+ / Pi 2 B / Pi 3 B | SMSC LAN9514 | 0424:EC00 | smsc95xx |
| Pi 1 B (rev 2) | SMSC LAN9512 | 0424:EC00 | smsc95xx |
| Pi 3 B+ | Microchip LAN7515 | 0424:7800 | lan78xx |

(Chip facts INFERRED from the boards' published schematics and my knowledge,
not measured here.)  The chip's hub part already enumerates today: `tusb_config.h:173`
counts "a Pi 1/2/3 Model B's own (with its Ethernet chip on it too)" hub
(MEASURED: comment).  The Ethernet function behind it is simply never claimed.

So "Ethernet on a 3B" is two USB vendor-class drivers (one per chip family)
plus a way for lwIP to use a wired link instead of the WiFi one.  One rpi3
kernel serves 3B, 3B+, 3A+ and Zero 2 W; the rpi and rpi2 kernels would pick
up LAN9512/9514 support on Pi 1 B/B+ and Pi 2 B for free.

## What exists to build on (MEASURED unless marked)

- **USB host:** TinyUSB, DWC2 in slave (PIO) mode, **polled** from the main
  loop (`PI1MHZ_TUH_POLLED`, `tusb_config.h`; `src/usb.c` `usb_host_task`).
  Hub, HID (mouse) and MSC (flash drive) classes are in use.
- **No host network class in TinyUSB:** `class/net` is device-side only
  (ncm_device.c, ecm_rndis_device.c).  TinyUSB does let an application add
  its own host class driver: `usbh_app_driver_get_cb()` (`usbh.c:81`).  That is
  the hook - no TinyUSB patch needed.
- **lwIP:** one netif, owned by `wifi/wifi_lwip.c`, fed by SDIO
  (`sdio_runtime_send_ethernet_frame(s)` / `sdio_runtime_poll_ethernet_frame`).
  The netif is already an Ethernet netif (`NETIF_FLAG_ETHERNET`, `ethernet_input`).
- **Consumers** reach the network through `wifi_lwip_get_context()`
  (address_ready, ipaddr, netif...): net_service, wifi_service, webserver,
  netname, AUN, smb_client, teletext, secure_service_wolfssh (9 files).
- **MAC address:** the firmware's board MAC is already read
  (`TAG_GET_BOARD_MAC_ADDRESS`, `rpi/info.c:265`); `info.h:43` notes it is the
  wired MAC on a Model B.  The LAN chips have no EEPROM on a Pi, so the driver
  must program it.

## Design

### 1. A link layer under wifi_lwip.c (refactor, no behaviour change)

Keep **one netif and one context**, so the nine consumers and DHCP/DNS/NTP
code do not change.  Split what is WiFi-specific out of the netif's two ends:

```c
typedef struct {
   const char *name;                              /* "WiFi", "Ethernet" */
   bool  (*link_up)(void);
   bool  (*send)(const uint8_t *frame, size_t len);   /* netif linkoutput */
   bool  (*poll)(uint8_t *buf, size_t max, size_t *len); /* one RX frame */
   void  (*mac)(uint8_t out[6]);
} net_link_t;
```

`wifi_lwip.c` gets an `active_link` pointer; WiFi's existing SDIO calls become
the WiFi `net_link_t`.  Renaming `wifi_lwip_*` to `net_*` is cosmetic and
should wait (it touches every consumer) - owner's call.

Control for this step: a WiFi-only build before and after, same rig, same
transfer (webserver PUT/GET and a ping series); numbers must match within
the per-boot RSSI spread already recorded (3x - compare like with like).

### 2. One USB Ethernet driver, two chip back-ends

`src/usb/usb_ether.c` registers an app class driver claiming 0424:EC00 and
0424:7800 (vendor class, bulk IN + bulk OUT + interrupt IN).

- **Control:** chip registers are read/written with vendor control requests
  (smsc95xx: 0xA1 read / 0xA0 write; lan78xx: same idea, different register
  map).  PHY registers go through the chip's MII access registers.
- **Init:** soft reset, program MAC from the mailbox, set up bulk-in
  burst/aggregation, PHY reset + autonegotiate, enable RX/TX.
- **TX:** each frame gets the chip's 8-byte command header (TX_CMD_A/B on
  both families, different bit layout) and goes out on bulk OUT.
- **RX:** keep one bulk IN transfer queued (several KB); one completion can hold
  several frames, each with a status word (smsc: 4-byte, lan78xx: 10-byte +
  alignment).  Split, copy to pbufs, hand to lwIP.
- **Link:** poll the PHY status (BMSR) about once a second from the main
  loop, or use the interrupt endpoint; report up/down to the link layer.

All of it runs in the main loop (TinyUSB callbacks from `tuh_task`), so lwIP
stays single-threaded.  Nothing goes near FIQ.

Reference implementations: **Circle** (rsta2/circle, GPLv3: `lib/usb/smsc951x.cpp`,
`lib/usb/lan7800.cpp`) is licence-compatible with Pi1MHz (GPLv3).  Linux's
drivers are GPLv2-only and must not be copied (memory: no Linux kernel code);
read the datasheets instead.

### 3. Choosing the link

Config key, e.g. `network=auto|wifi|ethernet` (default `auto`):
- `auto`: if a USB Ethernet chip enumerates and gets link within N seconds of
  boot, use it, and don't start WiFi; otherwise WiFi as today.
- Only one link at a time.  Two netifs (both up, routing, two IPs, two
  DHCP leases, which IP the Beeb and the web UI show) is a much bigger change
  for little gain.
- WiFi on a 3B is SDIO and independent of USB, so `wifi` keeps working exactly
  as now even with a cable plugged in.

`/status` rows: "Network: Ethernet, 100 Mb/s full, link up" (and the WiFi rows
only when WiFi is the link).  User doc: docs/user (where `network=` lives).

### 4. Release discipline

- Zero cost when no chip enumerates: the driver is only entered from TinyUSB
  callbacks for its own device.
- No new release logging; DEBUG-only enumeration/link prints.

## Stages (each its own commit, each hardware-tested with a control)

1. **Link layer refactor.**  Control: WiFi before/after, as above.
2. **Driver skeleton.**  Claims the device, prints (DEBUG) the IDs and endpoints.
   Proof: the print fires on the bench board.  Control: a board without the
   chip (Zero 2 W) shows nothing and boots as before.
3. **Chip init + link.**  Proof: `/status` shows link up/down as the cable is
   plugged/pulled, and the switch's port LED/speed agrees.
4. **Data path.**  DHCP lease, ping, webserver, then SMB storage and VFS video
   over the cable.
5. **Selection, /status, docs.**
6. **Measurements:** throughput (webserver GET/PUT, SMB) vs WiFi on the same board;
   main-loop idle-pass time with the link idle (the poll loop must not get
   slower when no traffic flows); SCSI/VFS behaviour under network load.

Do the chip on the bench board first and the other one second.

## Risks

- **Polled, PIO-only DWC2** (INFERRED): an always-queued bulk IN on a high-speed
  device makes the host NAK-poll continuously.  In slave mode with the USB IRQ
  masked that costs main-loop time per pass even with no traffic.  It may need
  a rate limit on RX re-arming, or the interrupt endpoint to say "data waiting".
  Measure in stage 2/3 before building the data path on top.
- **TinyUSB 168e76742 is NOT hardware-tested** (memory: tinyusb-upstream-bump
  2026-11-10).  Test the plain mouse/flash cases on that pin first, so a USB
  regression isn't blamed on the new driver.
- **Throughput ceiling** (GUESSED): the LAN9514 shares one USB 2.0 link with the
  other ports; with PIO and polling, expect well under its ~90 Mb/s.  Still
  likely better and steadier than the WiFi figures in
  docs/dev/wifi-throughput-2026-09.md - measure, don't promise.
- **Power-on:** USB power-on is already posted last (memory: improvements
  2026-11-11).  With `auto`, waiting for the Ethernet link must not hold up
  boot; time-box it and fall back to WiFi.

## Owner decisions

1. Which board is on the bench: **3B (LAN9514)** or **3B+ (LAN7515)**?  That
   chip goes first.
2. `network=` key name and default (`auto`?).
3. Single active link (proposed) vs both at once.
4. Rename `wifi_lwip_*` now or later.
