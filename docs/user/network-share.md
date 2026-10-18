# Network Share (SMB)

The Beeb's discs and files can live in a shared folder on another
computer - a Windows PC, a NAS, or a Linux box running Samba - instead
of the SD card. The Pi reaches the share over WiFi. Set
`storage=smb` and the share's details in `/Pi1MHz/Pi1MHz.cfg`, and the
Beeb uses the share for:

- its ADFS hard discs (`/BeebSCSI0`, `/BeebSCSI1`, ...)
- the VFS / LaserDisc discs (`/BeebVFS0`, ...), video included (see
  [Limits](#limits))
- files it reads and writes through the Pi: MMFS (`BEEB.MMB`), MMFS2's
  `.ssd`/`.dsd` images, and the FAT transfer directory
- [FujiNet](fujinet.md)'s files and Music 5000 recordings

The SD card still holds the `/Pi1MHz` folder (`Pi1MHz.cfg`, the helper
ROMs), the firmware and the WiFi settings. The
[web interface](web-interface.md) and
[USB file access](usb-file-access.md) show the SD card, not the share.

## Setting it up

1. **Share a folder** on the other computer, with read and write access
   for a user name and password. Any SMB 2 or 3 server works: Windows
   file sharing, a NAS, Samba. On Windows, the network must be set to
   *Private* or the PC won't accept the connection.
2. **Copy the folders across** in the same layout as on the SD card -
   the share's top level holds `BeebSCSI0`, `BeebVFS0`, `BEEB.MMB` and
   so on.
3. **Set up [WiFi](wifi.md)** if you haven't.
4. **Add the share to `/Pi1MHz/Pi1MHz.cfg`:**

   ```
   storage=smb
   smb_server=192.168.0.10
   smb_share=Beeb
   smb_user=beeb
   smb_password=secret
   ```

   `smb_server` is the computer's address, or a name your network's DNS
   knows. `smb_domain=` names a Windows domain if the user belongs to
   one. `smb_encrypt=1` encrypts every message, not just the logon.
   Messages are always signed.
5. **Power up.**

The password is kept in `Pi1MHz.cfg` as plain text. Give the Pi a user
of its own that can reach only this share.

## When the share is used

- **At power-on** the Pi waits up to about 20 seconds for WiFi before
  the Beeb's first disc access is answered, so the Beeb starts from the
  share.
- **If the Pi can't log on** (wrong password, server off, no WiFi), the
  Beeb uses the SD card. The share is tried again at every BREAK.
- **A lost connection** gives disc errors - `Disc error`, never a hang -
  until the next BREAK, which logs on again.
- One request waits at most 10 seconds for the server.
- A BREAK during a slow request is never missed: the request is dropped
  and the Beeb resets as usual.

## Switching from the Beeb

The Beeb can switch storage itself, whatever `storage=` says:

```
*FX147,202,2
*FX147,203,6        use the network share

*FX147,202,2
*FX147,203,4        use the SD card
```

Every file is closed first, then the discs come from the share - as a
card swap. The choice holds through BREAK until the Pi is switched off
or restarted. `6` is refused (`&FF`) when `smb_server` and `smb_share`
aren't set. If they are set but the Pi can't log on, it also reads
`&FF`, but the share stays chosen and every BREAK tries it again. To
see the result, select the register again and read it - `0` done,
`&FF` refused:

```
*FX147,202,2
A%=146:X%=203:PRINT ~(USR&FFF4 AND &FF0000) DIV &10000
```

Selecting the register again matters: reading `&FCCB` straight after
writing it only gives back the value written.

The status page shows a **Beeb storage** row (`SD card`, `USB drive` or
`SMB share`) and an **SMB share** row: `\\server\share connected`, or
why not.

## What works

Tested on a Master 128 with a Pi Zero W and a Pi Zero 2 W, against
Samba on a Raspberry Pi 5 and a Windows share:

- ADFS: `*CAT`, `*SAVE`, `*LOAD`; the server's file changes and the SD
  card's copy doesn't
- VFS discs and their video
- MMFS: `*DCAT`, `*DIN`, `*SAVE`/`*LOAD` into `BEEB.MMB`
- MMFS2: `*DIN` of a file in the top folder or a subfolder, writes,
  `*DACCESS` (sets the file's read-only flag on the server) and
  `*DRENAME` of a disc that is in a drive
- switching with `*FX147,203,6` and `4`, and BREAK while on the share
- the server switched off mid-use, and back on

Not yet tested on hardware: FujiNet and Music 5000 recordings on the
share.

## Limits

- **LaserDisc video plays at only about 11-14 frames a second** from a
  share over WiFi (25 are needed). The discs' data and still frames are
  fine.
- **Locking a disc that is already in a drive** (`*DACCESS` while it's
  mounted) doesn't stop writes to it until it is mounted again: the
  server checks read-only only when the file is opened.
- **Tools that read the card by sector number** (the FAT service's raw
  sector commands) are refused on the share; the MMFS and MMFS2 ROMs
  Pi1MHz ships use files, so they work.
- The web interface, WebDAV and USB file access still show the SD card.
- Only one share.
