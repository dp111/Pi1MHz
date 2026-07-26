# Driving the target Pi1MHz from a Claude session

How to flash firmware to the Pi and read its debug output, end to end, with
no help from the user. Verified working 2026-07-25.

Three channels:

| Channel | Direction | Used for |
|---------|-----------|----------|
| MTP flash | host -> Pi | pushing a new kernel |
| UART / COM5 | Pi -> host | boot banner, debug printf, crash dumps |
| HTTP / WebDAV | both | status pages, SD-card files, reboot |

Everything below is already allow-listed, so none of it prompts.

---

## 0. The one-paragraph version

Build to `firmware/kernel.img`, start the serial capture **in the
background first**, flash, wait, then read the log. Confirm the new image
is really running by **behaviour** (a value your change alters, or the
`/status` frame counters resetting) — never by the Pi merely being
reachable.

---

## 1. Build

```bash
cd /mnt/c/Archlinux/Pi1MHz && ./build-gcc17.sh
```

Produces `firmware/kernel.img` (Pi 1 / Zero) and `firmware/kernel7.img`
(Pi 2/3). **The flasher always sends `kernel.img`** — build to that path or
you will flash a stale image.

Debug builds (`-DDEBUG=1`) are what emit serial output. **Release builds
print nothing**, so a silent capture from a release build is expected, not
a fault — smoke-test with a debug build of the same source.

---

## 2. Read the UART (Pi -> host)

COM5 @ 115200. WSL exposes `/dev/ttyS5` but does **not** bridge it to COM5 —
reading it yields nothing. Drive the port from the Windows side:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Archlinux\claude-tmp\serial-capture.ps1' -Seconds 40
```

Writes to `C:\Archlinux\claude-tmp\serial.log`
(= `/mnt/c/Archlinux/claude-tmp/serial.log` from WSL). Options:
`-Port`, `-Baud`, `-Seconds`, `-Out`.

**Only one reader may hold COM5.** A second capture fails with
`Access to the port 'COM5' is denied`, which looks exactly like a dead Pi.
Never overlap captures; wait for the previous one to exit (`Get-Process
powershell` to check for stragglers). If the user has their own terminal on
COM5, ask them to close it rather than killing processes you did not start.

---

## 3. Flash (host -> Pi)

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Archlinux\claude-tmp\flash-robust.ps1'
```

Copies `C:\Archlinux\Pi1MHz\firmware\kernel.img` to the "Pi1MHz MTP" device
as `kernel.now`, which makes the Pi restart into it. Prints
`Flashed - Pi rebooted after N.Ns`.

Use **`flash-robust.ps1`**, not `flash-fast.ps1`: the fast one grabs the MTP
storage folder immediately, but just after a reboot the device enumerates
before its storage child does, so it silently copies nothing and then
reports `Sent; Pi still present after ~10s (did it reboot?)` — that message
means *the copy never happened*, not a slow reboot.

### `kernel.now` is a TRANSIENT RAM boot

It chain-boots from RAM; **the SD card's `kernel.img` is not replaced**. A
power cycle or `POST /reboot` reverts to the SD kernel. To persist a kernel,
write it to the SD root as `kernel.img` over WebDAV/MTP.

A `kernel.now` push can also boot the *previous* pushed image from stale
RAM, and can Data-Abort the running kernel mid-transfer (the Pi then reboots
itself — just retry).

### If flashing stops working

Check the MTP storage-child count:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Archlinux\claude-tmp\mtp-health.ps1'
# -> STORAGE_CHILDREN=1  (1 = healthy, 0 = needs a power cycle)
```

**If it reports 0, stop flashing and ask for a physical power cycle** — the
Pi is wedged in a state where the MTP device still enumerates but exposes no
storage folder to copy into. Serial goes silent and WiFi drops too, and no
remote recovery works. `NO_DEVICE` just means it is not in MTP mode.

To confirm a file actually landed on the device, list a folder:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Archlinux\claude-tmp\mtp_enum.ps1' -Folder Transfer [-Match name.dat]
```

---

## 3a. Type on the Beeb (host -> Master 128, COM9)

COM9 is the Master 128's keyboard. It takes **plain text** — but one
character at a time, with a gap between them:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Archlinux\claude-tmp\com9-send.ps1' -Text '*CAT' [-CharDelayMs 200]
```

Two things that silently produce nothing, both of which look like a dead
channel rather than a mistake:

* **Send the characters paced, ~200 ms apart.** A back-to-back write is
  swallowed by the keyboard emulation — `*CAT` sent as one string arrives
  as `*aa`. `Write()` reports success either way.
* **Assert DTR and RTS.** The COM5 capture script deliberately leaves them
  low; copying that here means the host stack accepts the write and never
  puts the bytes on the wire. Baud is irrelevant — it is USB CDC.

There is no echo back unless the screen redirect is enabled, so **verify by
effect, not by reply**: `*CAT` should produce `READ6` + `Attempting to
Auto-Start LUN` in the COM5 trace (debug build), and `*BYE` should produce
`STARTSTOP command (0x1B)` + `LUN number 0 is stopped`. Those two commands
are the standard way to start and release a LUN for any test that needs the
disc held open or let go. A dismount does *not* reach the firmware.

---

## 4. HTTP / WebDAV (both directions)

Use the host-pinned wrapper, not bare curl (the allowlist grants the
wrapper, which can only ever reach the Pi):

```bash
/mnt/c/Archlinux/claude-tmp/pi-http.sh [curl-args...] <absolute-path>
```

```bash
pi-http.sh -s /status                       # WiFi, IP, frame counters, SD free
pi-http.sh -s /aun                          # AUN engine state + counters
pi-http.sh -s -X PROPFIND -H 'Depth: 1' /   # list the SD card root
pi-http.sh -s -T local.rom /some.rom        # upload a file to the SD card
pi-http.sh -s -X POST /reboot               # reboot (reverts to the SD kernel)
```

- The DAV namespace is mounted at `/`, **not** `/dav/`. `/files/` is the HTML
  browser, a different thing that 404s under PROPFIND.
- `OPTIONS` answers 200 on any path, so it does **not** prove a path exists.
- Digest auth, user and password both `Pi1MHz`, read automatically from
  `~/.pi1mhz-creds`. curl always shows a first 401 during the digest
  handshake — that is normal.
- `PI_IP` defaults to 192.168.0.42 but **DHCP moves it**. Re-read it from the
  boot line `WIFI-LWIP: address ready ip=` and pass `PI_IP=...` if it moved.

---

## 5. The ordering that works

```bash
# 1. start the capture in the BACKGROUND first, or you miss the boot banner
#    (run_in_background: true)
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Archlinux\claude-tmp\serial-capture.ps1' -Seconds 40

# 2. flash
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Archlinux\claude-tmp\flash-robust.ps1'

# 3. wait for the log to fill (use an until-loop, not a bare sleep)
until [ -s /mnt/c/Archlinux/claude-tmp/serial.log ]; do sleep 2; done

# 4. read it
cat /mnt/c/Archlinux/claude-tmp/serial.log
```

Boot to `address ready` takes roughly 25-30 s, most of it DHCP.

---

## 6. Always verify the right image booted

Flashing is unreliable and **reachability proves nothing**. Confirm by one of:

1. **Serial banner** — prints `dev <sha>-dirty <build timestamp>`, which
   matches the `kernel.img` mtime exactly. Strongest check, debug builds only.
2. **Behaviour** — a status code, counter or string that your change alters.
3. **Counter reset** — `/status` `Frames received` dropping from millions to
   hundreds means it really rebooted; a high count means it did not.

Waiting ~30 s unconditionally after a flash before judging is worth it.
Three separate occasions produced wrong conclusions from a stale image.

The third had a mechanism worth knowing: `firmware/kernel.img` is **tracked**,
so any `git checkout` touching it silently restores the committed image over
your build, and the next flash then sends firmware that predates your change.
It reads as "my fix did nothing" (or worse, "my fix works" — an image without
the feature cannot exhibit the bug either). `git status firmware/` coming back
*clean* right after a build is the tell: your build should have left it dirty.
Check the size and mtime against what the build printed before flashing.

Note the Pi's chronic **intermittent multi-second unreachability is an RF
link problem, not firmware** — do not read a transient drop as a reboot.
For network diagnosis drive Windows ping/arp via `powershell.exe`: WSL is
NAT'd on 172.26.x, while the Windows host is on the real LAN alongside the Pi.

---

## 7. Standing permissions

Flashing and rebooting the target Pi need **no check-in** — go ahead. The
flash script, serial capture and HTTP wrapper are all allow-listed in
`.claude/settings.local.json`.
