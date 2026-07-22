/* Minimal public surface of the MTP-over-FatFs backend (usb/mtp_fs.c).
   Kept free of TinyUSB headers so non-USB subsystems (e.g. the WebDAV
   server) can include it without pulling in <tusb.h>. */
#ifndef PI1MHZ_USB_MTP_FS_H
#define PI1MHZ_USB_MTP_FS_H

/* Signal that the SD filesystem changed underneath MTP (a file was created,
   deleted, or renamed by another subsystem via FatFs).  Invalidates MTP's
   in-memory object-handle cache so the next MTP request re-enumerates the
   card instead of serving stale handles.  Must be called from the main-loop
   poll context (where the webserver already runs); it is not ISR-safe. */
void mtp_fs_notify_fs_changed(void);

#endif /* PI1MHZ_USB_MTP_FS_H */
