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

/* As above, but also emit an asynchronous MTP event so a connected host
   (Windows Explorer) refreshes its own cached view immediately rather than
   waiting for the next session open / manual refresh.  `path` is the full SD
   path ("/dir/name", no trailing slash) of the affected object - the same
   form MTP hashes into its object handle.  Use _added for a newly created
   file/dir, _removed for a deletion, _changed when an existing object's
   contents/metadata changed (e.g. an overwrite).  Best-effort: the event is
   skipped if no MTP session is open and the cache invalidation still happens
   regardless.  Main-loop context only; not ISR-safe. */
void mtp_fs_notify_object_added(const char* path);
void mtp_fs_notify_object_removed(const char* path);
void mtp_fs_notify_object_changed(const char* path);

#endif /* PI1MHZ_USB_MTP_FS_H */
