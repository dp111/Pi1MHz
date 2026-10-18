/* Where a path the Beeb names lives: on the SD card, the USB flash drive
   (storage=usb, FatFs volume "1:") or the network share (storage=smb, "2:",
   fsroute.c).  The choice itself - filesystemStorageRoot, decided at each
   BBC reset - is in filesystem.c; this is only the rule for a path, apart so
   the host tests of its users (the FAT service) link the real one. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>

#include "filesystem.h"

/* /Pi1MHz is the Pi's own folder - Pi1MHz.cfg, the helpers' ROMs (which the
   6502 helper code opens through the FAT service as "Pi1MHz/<name>.rom"),
   6502code.bin - and stays on the card whatever storage= says. */
bool filesystemPiFolder(const char *path)
{
   while (*path == '/')
      path++;
   return strncasecmp(path, "Pi1MHz/", 7) == 0;
}

bool filesystemStoragePath(const char *path, char *buf, size_t size)
{
   /* A path naming its own volume ("0:...") is left as it is, and so is
      one in the Pi's own folder - without asking for the root, so neither
      ever waits for the power-on decision. */
   const char *root = ((path[0] >= '0' && path[0] <= '9' && path[1] == ':') || filesystemPiFolder(path))
                    ? "" : filesystemStorageRoot();
   int n = snprintf(buf, size, "%s%s", root, path);
   return n >= 0 && (size_t)n < size;
}

/* Is this path, as the Beeb names it, on the card?  Yes unless the storage
   is decided on the drive or the share - and even then, the Pi's own folder
   is.  Never decides, so never waits: undecided is the card, the cautious
   side for the host-write interlocks. */
bool filesystemPathOnCard(const char *path)
{
   return !filesystemStorageOffCard() || filesystemPiFolder(path);
}
