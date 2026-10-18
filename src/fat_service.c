/*
  The FAT/SD service: commands 0-20 on the services port (&FCA6).
  17 = readdir-ex (fixed records for the SD explorer), 18 = getcwd.

  Enables the Beeb to access the SDCARD.  16Mbytes of the JIM buffer is
  available as the transfer buffer; command blocks live in its top pages.
*/

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "Pi1MHz.h"

#include "ram_emulator.h"
#include "M5000_emulator.h"		/* M5000_recording_path_busy */
#include "fujibus_service.h"		/* fujibus_service_path_busy */
#include "services.h"
#include "config.h"				/* Beeb_write_protect */
#include "BeebSCSI/fatfs/ff.h"			/* Obtains integer types */
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"
#include "fsroute.h"			/* f_* by path: the card, the USB drive or the share */
#include "wifi/webserver.h"			/* cached SD free space - see case 13 */

NOINIT_SECTION static FIL fileObject[16];
NOINIT_SECTION static DIR dirObject[16];

/* 32-bit access into JIM RAM. The command block is always page-aligned
 * and JIM_ram is malloc'd, so &JIM_ram[off] is 4-byte aligned at every
 * call site below. memcpy + assume_aligned yields a single LDR/STR on
 * every CPU, with no strict-aliasing UB and no -Wcast-align warning. */
static inline uint32_t jim_read32(uint32_t off)
{
   uint32_t v;
   memcpy(&v, __builtin_assume_aligned(&Pi1MHz->JIM_ram[off], 4), sizeof v);
   return v;
}

static inline void jim_write32(uint32_t off, uint32_t v)
{
   memcpy(__builtin_assume_aligned(&Pi1MHz->JIM_ram[off], 4), &v, sizeof v);
}

/* The command structure handled by fat_service_command() is filled in
 * by the host (the Beeb), so every buffer offset, length and path string it
 * supplies is untrusted. Without the checks below a host-supplied offset is
 * added straight to base_addr and used to index JIM_ram[], which is an
 * arbitrary read/write primitive. These helpers keep every host-supplied
 * access inside the disc RAM region. */

/* FatFs is configured with a fixed 512-byte sector size (FF_MIN_SS ==
 * FF_MAX_SS == 512 in ffconf.h); disk_read()/disk_write() transfer whole
 * sectors of this size. */
#define DISC_SECTOR_SIZE 512u

/* Longest path string accepted from the host (well above any real FatFs
 * path); also bounds the terminator scan so a missing NUL cannot turn into
 * a multi-megabyte loop. */
#define DISC_MAX_PATH 1024u

/* ---- open-file tracking for the webserver's in-use interlock ------------
   The Beeb opens files here (FIQ context); the webserver asks from the main
   loop whether a path is one of them before overwriting, deleting or moving
   it - the same protection the SCSI LUN images already have.  Paths are
   recorded absolute: relative opens are joined against a cwd cache that is
   refreshed on the rare chdir (f_getcwd walks directories, so it is not
   called per open).  The valid flag is set last on open and cleared first
   on close, so a torn read from the main loop cannot see a half-written
   path as valid.  A path too long to record, or an open under an unknown
   cwd, is left unrecorded: the interlock fails open rather than ever
   matching the wrong file. */
#define FAT_OPEN_PATH_MAX 130u
static volatile bool fat_open_valid[16];
static char fat_open_path[16][FAT_OPEN_PATH_MAX];
static char fat_cwd[FAT_OPEN_PATH_MAX] = "/";
static bool fat_cwd_known = true;
/* MMFS does NOT keep its image store open: it resolves BEEB.MMB to a start
   sector once and then reads and writes RAW SECTORS (commands 0/1), which
   file-handle tracking cannot see - and which assumes the file never moves.
   So once raw sector access has been used, BEEB.MMB is reported busy until
   a remount or reboot: replacing it would re-allocate clusters under a
   filing system holding absolute sector numbers. */
static volatile bool fat_raw_sector_seen;

/* Which fileObject[]/dirObject[] slots actually hold an opened FatFs object.
   Those arrays live in .noinit, which is deliberately NOT zeroed, so an
   un-opened slot holds whatever the previous session - or, on a cold boot,
   uninitialised DRAM - left there.  FatFs's validate() gate dereferences
   obj->fs, so a command naming a handle the ROM never opened made it load
   through a wild pointer, in FIQ context.  The handle is (data & 15), taken
   straight from the Beeb's command register, so it is entirely host-chosen.

   fat_open_valid[] cannot serve as this gate: it records whether the PATH was
   captured for the webserver interlock, and fat_open_record() gives up -
   leaving it false - on an unresolvable relative name even though the f_open
   succeeded.  These two live in .bss, so a cold boot, a watchdog reset and a
   chain-boot all start them false, which is the safe direction: refuse until
   the ROM re-opens. */
static volatile bool fat_file_open[16];
static volatile bool fat_dir_open[16];

static void fat_open_record(unsigned int handle, const char *name)
{
   char joined[FAT_OPEN_PATH_MAX];
   const char *path = name;

   fat_open_valid[handle] = false;
   if (name[0] >= '0' && name[0] <= '9' && name[1] == ':') {
      /* names its own volume: recorded as it is */
   } else if (!filesystemPathOnCard(name)) {
      /* On the drive or the share (as fat_beeb_path opened it): its volume
         in front keeps it from matching a card path.  fat_cwd is that
         volume's directory. */
      const char *vol = filesystemStorageOnSmb() ? "2:" : "1:";
      int n = (name[0] == '/')
            ? snprintf(joined, sizeof joined, "%s%s", vol, name)
            : !fat_cwd_known ? -1
            : snprintf(joined, sizeof joined, "%s%s/%s", vol,
                       (fat_cwd[0] == '/' && fat_cwd[1] == '\0') ? "" : fat_cwd, name);
      if (n < 0 || (size_t)n >= sizeof joined)
         return;
      path = joined;
   } else if (name[0] != '/') {
      /* On the card.  With the storage on the drive or the share this is
         the Pi's own folder ("Pi1MHz/<n>.rom"), relative to the card's
         directory, which the Beeb's directory changes (all on the other
         volume) leave at the root. */
      int n;
      const char *cwd = filesystemStorageOffCard() ? "/" : fat_cwd;
      if (!filesystemStorageOffCard() && !fat_cwd_known)
         return;
      n = snprintf(joined, sizeof joined, "%s/%s",
                   (cwd[0] == '/' && cwd[1] == '\0') ? "" : cwd, name);
      if (n < 0 || (size_t)n >= sizeof joined)
         return;
      path = joined;
   }
   if (strlen(path) >= sizeof fat_open_path[0])
      return;
   strcpy(fat_open_path[handle], path);
   fat_open_valid[handle] = true;
}

static void fat_open_clear_all(void)
{
   for (unsigned int i = 0; i < 16u; i++) {
      fat_open_valid[i] = false;
      fat_file_open[i] = false;
      fat_dir_open[i] = false;
   }
   strcpy(fat_cwd, "/");
   fat_cwd_known = true;
   fat_raw_sector_seen = false;
}

static bool fat_service_eject(void);    /* below: close all, forget the kept */

/* FAT names are case-insensitive, and either side may carry a "0:" drive
   prefix or leading slashes. */
static const char *fat_path_norm(const char *p)
{
   if (p[0] == '0' && p[1] == ':')
      p += 2;
   while (*p == '/')
      p++;
   return p;
}

/* ---- The Beeb's storage: the SD card, the USB drive (storage=usb) or the
   network share (storage=smb) ----
   Every name from the Beeb gets the storage root (filesystemStoragePath), so
   with the drive in use "1:" is put in front: an absolute name is then on the
   drive, and a relative one is relative to the drive's own current directory
   (FatFs keeps one per volume).  The share has no current directory of its
   own: this service keeps it (fat_cwd) and joins relative names to it.  A
   name with its own volume, or in the Pi's own folder, is left alone. */
static char fat_path_a[DISC_MAX_PATH + 3], fat_path_b[DISC_MAX_PATH + 3];

static bool fat_join(char *out, size_t size, const char *dir, const char *name);

/* Which volume the Beeb's storage is on: deciding it, if this is the first
   use since a BBC reset. */
static bool fat_on_share(void)
{
   return filesystemStorageRoot()[0] == '2';
}

static bool fat_off_card(void)
{
   return filesystemStorageRoot()[0] != '\0';
}

static bool fat_names_volume(const char *name)
{
   return name[0] >= '0' && name[0] <= '9' && name[1] == ':';
}

static const char *fat_beeb_path(uint32_t off, char *buf, size_t size)
{
   const char *name = (const char *)&Pi1MHz->JIM_ram[off];
   if (fat_on_share() && name[0] != '/' && !fat_names_volume(name) &&
       !filesystemPiFolder(name)) {
      char abs[DISC_MAX_PATH + 3];
      if (!fat_join(abs, sizeof abs, fat_cwd, name))
         return name;                    /* too long: fails on the card's FatFs */
      return filesystemStoragePath(abs, buf, size) ? buf : name;
   }
   return filesystemStoragePath(name, buf, size) ? buf : name;
}

/* The current directory of the Beeb's volume, without its "N:" prefix (as
   the Beeb has always seen it).  f_getcwd reports the current drive's, so
   the drive's is asked for with it made current just for the call; the
   share's is this service's own. */
static FRESULT fat_getcwd(char *buf, UINT len)
{
   if (fat_on_share()) {
      if (strlen(fat_cwd) >= len)
         return FR_NOT_ENOUGH_CORE;
      strcpy(buf, fat_cwd);
      return FR_OK;
   }
   bool usb = fat_off_card();
   if (usb)
      (void)f_chdrive("1:");
   FRESULT result = f_getcwd(buf, len);
   if (usb)
      (void)f_chdrive("0:");
   if (result == FR_OK && buf[0] >= '0' && buf[0] <= '9' && buf[1] == ':')
      memmove(buf, buf + 2, strlen(buf + 2) + 1u);
   return result;
}

/* chdir on the share: name joined to fat_cwd, and it must be a directory. */
static FRESULT fat_share_chdir(const char *name)
{
   char abs[FAT_OPEN_PATH_MAX];
   char path[FAT_OPEN_PATH_MAX + 3];
   FILINFO fi;
   if (!fat_join(abs, sizeof abs, fat_cwd, name))
      return FR_INVALID_NAME;
   if (strcmp(abs, "/") != 0) {
      FRESULT r = fsr_stat(filesystemStoragePath(abs, path, sizeof path) ? path : abs, &fi);
      if (r == FR_NO_FILE)
         r = FR_NO_PATH;
      if (r != FR_OK)
         return r;
      if (!(fi.fattrib & AM_DIR))
         return FR_NO_PATH;
   }
   strcpy(fat_cwd, abs);
   fat_cwd_known = true;
   return FR_OK;
}

/* Raw sectors (MMFS reading BEEB.MMB with its own FAT code): the Beeb's
   drive 0 is the storage - the card, or the USB drive when the storage is
   on it.  No other drive is the Beeb's: diskio answers RES_PARERR for this
   one, as it did for every drive but 0 before there was a second, so the
   Beeb cannot write raw sectors under FatFs's mounted drive.  A share has
   no sectors: the ROMs built for the Pi1MHz use files (commands 2-6). */
#define FAT_RAW_NO_DRIVE 0xFFu
static BYTE fat_raw_drive(uint8_t beeb_drive)
{
   if (beeb_drive != 0u || fat_on_share())
      return FAT_RAW_NO_DRIVE;
   return fat_off_card() ? 1u : 0u;
}

bool fat_service_file_in_use(const char *host_path)
{
   const char *q = fat_path_norm(host_path);
   size_t qlen = strlen(q);

   /* The raw-sector client's store (see fat_raw_sector_seen above).  MMFS
      reads BEEB.MMB from the ROOT of the card (its own minimal FAT reader
      scans the root directory for that fixed name), so the file can only
      live there - matching a BEEB.MMB basename (case-insensitive) and the
      root path covers it.  We do not need to walk parent directories: MMFS
      cannot reach an MMB in a subdirectory, so no such directory can hold
      the sectors it cached. */
   if (fat_raw_sector_seen) {
      static const char mmb[] = "BEEB.MMB";
      size_t base = qlen;
      while (base > 0u && q[base - 1u] != '/')
         base--;
      if (qlen == 0u)
         return true;              /* the root holds BEEB.MMB */
      if (qlen - base == sizeof mmb - 1u) {
         size_t k;
         for (k = 0; k < sizeof mmb - 1u; k++) {
            char c = q[base + k];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (c != mmb[k])
               break;
         }
         if (k == sizeof mmb - 1u)
            return true;
      }
   }

   for (unsigned int i = 0; i < 16u; i++) {
      const char *p;
      size_t j;

      if (!fat_open_valid[i])
         continue;
      p = fat_path_norm(fat_open_path[i]);
      for (j = 0; j < qlen; j++) {
         char ca = p[j], cb = q[j];
         if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
         if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
         if (ca != cb)
            break;
      }
      /* Equal - or host_path is a directory containing the open file, so
         a recursive DELETE/MOVE of the folder is refused too. */
      if (j == qlen && (p[qlen] == '\0' || p[qlen] == '/' || qlen == 0u))
         return true;
   }
   return false;
}

/* The one predicate every host-side writer (WebDAV, MTP) asks before it
   overwrites, deletes or renames a path: is the Beeb using it by ANY route -
   a started SCSI LUN image (or a directory holding one), a file open
   through this service, or the WAV the Music 5000 is still writing out.  A host write that lands on a running LUN's cluster
   chain is silent corruption, so the two halves live together here rather
   than being re-joined at each call site. */
bool beeb_path_busy(const char *host_path)
{
   /* Those two keep their paths without a volume: with the storage on the
      USB drive their files are not the card's. */
   bool card = filesystemPathOnCard(host_path);
   return filesystemHostPathBusy(host_path) || fat_service_file_in_use(host_path)
       || (card && M5000_recording_path_busy(host_path))   /* a WAV still being flushed */
       || (card && fujibus_service_path_busy(host_path));  /* a FujiNet-mounted image */
}

/* ---- readdir-ex (command 17) record ------------------------------------
   A fixed 128-byte record per directory entry, laid out for a 6502 client:
   power-of-two size so entry N is a shift away, and a ready-to-print
   38-column display line so the Beeb never has to format sizes itself.

     +0        attribute byte (AM_DIR etc., see FatFs FILINFO.fattrib)
     +1..3     reserved (0)
     +4..7     file size, little-endian (0 for a directory)
     +8..46    display line: exactly FAT_DIRENT_DISP printable chars
               (name, then right-aligned size or <DIR>), NUL terminated
     +48..127  raw name for fopen/fchdir, NUL terminated (truncated if
               longer than 79 chars)                                      */
#define FAT_DIRENT_SIZE  128u
#define FAT_DIRENT_DISP   38u
#define FAT_DIRENT_NAME  (FAT_DIRENT_SIZE - 48u)
#define FAT_CWD_MAX      128u

static void fat_dirent_record(uint8_t *rec, const FILINFO *info)
{
   char size_text[12];
   char name[FAT_DIRENT_DISP];     /* display copy, truncated + sanitized */
   size_t n;

   memset(rec, 0, FAT_DIRENT_SIZE);
   rec[0] = info->fattrib;
   rec[4] = (uint8_t)(info->fsize);
   rec[5] = (uint8_t)(info->fsize >> 8);
   rec[6] = (uint8_t)(info->fsize >> 16);
   rec[7] = (uint8_t)(info->fsize >> 24);

   /* Right-hand column: <DIR>, bytes, KB, MB or GB - always fits 7 chars
      (each branch is under 1000000; the % only shows the compiler so). */
   if (info->fattrib & AM_DIR)
      strcpy(size_text, "<DIR>");
   else if (info->fsize < 1000000u)
      snprintf(size_text, sizeof size_text, "%lu", (unsigned long)(info->fsize % 1000000u));
   else if ((info->fsize >> 10) < 1000000u)
      snprintf(size_text, sizeof size_text, "%luK", (unsigned long)((info->fsize >> 10) % 1000000u));
   else if ((info->fsize >> 20) < 1000000u)
      snprintf(size_text, sizeof size_text, "%luM", (unsigned long)((info->fsize >> 20) % 1000000u));
   else                       /* exFAT: a terabyte and up */
      snprintf(size_text, sizeof size_text, "%luG", (unsigned long)((info->fsize >> 30) % 1000000u));

   /* Display name: truncate to the name column, mark directories with a
      trailing '/', and replace anything a teletext VDU stream would
      interpret as a control code. */
   n = 0;
   while (n < sizeof name - 2u && info->fname[n]) {
      char c = info->fname[n];
      name[n] = (c < 32 || c > 126) ? '?' : c;
      n++;
   }
   if ((info->fattrib & AM_DIR) && n < sizeof name - 1u)
      name[n++] = '/';
   name[n] = '\0';

   sprintf((char *)&rec[8], "%-*.*s%7s",
           (int)(FAT_DIRENT_DISP - 7u), (int)(FAT_DIRENT_DISP - 7u),
           name, size_text);

   /* Raw name, for the Beeb to hand back to fopen/fchdir. */
   n = strlen(info->fname);
   if (n >= FAT_DIRENT_NAME)
      n = FAT_DIRENT_NAME - 1u;
   memcpy(&rec[48], info->fname, n);
   rec[48u + n] = '\0';
}

/* ---- Slots with their own directory, and files kept across a BBC reset --
   For a filing system that lives in this service (MMFS and MMFS2 built for
   the Pi1MHz): it holds its image files open here instead of reading raw
   card sectors, so they may be fragmented and on any storage.

   A slot's own directory (command 22) is where its relative names are
   resolved - MMFS2's current directory, which the SD explorer browsing the
   shared one (command 11) must not move.  "" = the shared one, as before.

   An open with FAT_MODE_KEEP remembers the file's path and mode.  A BBC
   reset still closes it (the remount invalidates every FIL, and closing
   releases the host-write interlock), but the slot's next read, write or
   size reopens it by that path: the Beeb's filing system keeps its drives
   over a soft BREAK, as with a real card.  A file gone meanwhile, or on
   storage that has changed (the card, the USB drive or the share), fails
   the request.
   A rename here follows it (fat_rename_follow).  Both live in .bss: a Pi
   boot forgets them, and so do an eject and the Beeb's own mount or
   unmount (fat_forget_kept); the boot id (command 20) changes with them. */
#define FAT_MODE_KEEP 0x80u             /* open mode bit 7: FatFs uses 0-5 */
static char fat_slot_dir[16][FAT_OPEN_PATH_MAX];
static char fat_keep_path[16][FAT_OPEN_PATH_MAX];
static BYTE fat_keep_mode[16];          /* 0 = nothing kept (FA_READ is 1) */
static char fat_keep_vol[16];           /* the storage it was opened on: its root's
                                           first character ('\0' the card) */
static uint16_t fat_id;                 /* the boot id; 0 = not taken yet */

/* The directory a slot's relative names are resolved against. */
static const char *fat_slot_base(unsigned int slot)
{
   if (fat_slot_dir[slot][0] != '\0')
      return fat_slot_dir[slot];
   return fat_cwd_known ? fat_cwd : "/";
}

/* dir + name as an absolute path with "." and ".." resolved ("/" for the
   root); name absolute: dir is ignored.  False if it does not fit. */
static bool fat_join(char *out, size_t size, const char *dir, const char *name)
{
   char tmp[FAT_OPEN_PATH_MAX * 2];
   size_t len = 0;
   int n = (name[0] == '/') ? snprintf(tmp, sizeof tmp, "%s", name)
                            : snprintf(tmp, sizeof tmp, "%s/%s", dir, name);
   if (n < 0 || (size_t)n >= sizeof tmp || size < 2u)
      return false;
   out[0] = '\0';
   char *save = NULL;
   for (char *c = strtok_r(tmp, "/", &save); c != NULL; c = strtok_r(NULL, "/", &save)) {
      /* MMFS2 writes a name with no extension "NAME.", and so ".." "...":
         one trailing '.' is not part of the name */
      size_t cl0 = strlen(c);
      if (cl0 > 1u && c[cl0 - 1u] == '.' && strcmp(c, "..") != 0)
         c[cl0 - 1u] = '\0';
      if (strcmp(c, ".") == 0)
         continue;
      if (strcmp(c, "..") == 0) {
         while (len > 0u && out[len - 1u] != '/')
            len--;
         if (len > 0u)
            len--;                      /* the '/' before it */
         out[len] = '\0';
         continue;
      }
      size_t cl = strlen(c);
      if (len + 1u + cl + 1u > size)
         return false;
      out[len++] = '/';
      memcpy(&out[len], c, cl + 1u);
      len += cl;
   }
   if (len == 0u)
      strcpy(out, "/");
   return true;
}

/* The storage-rooted name FatFs opens for an absolute Beeb path. */
static const char *fat_storage(const char *abs, char *buf, size_t size)
{
   return filesystemStoragePath(abs, buf, size) ? buf : abs;
}

/* The 8.3 name the Beeb's filing system sees for an entry, "NAME.EXT":
   FatFs's own short name on a FAT volume, which opens as it is; exFAT has
   none, so one is made - the long name when it is a valid 8.3 name, else
   its first four characters, '~' and three hex digits of a hash of the
   whole name.  fat_slot_find() turns a made one back into the long name. */
static void fat_short_name(const FILINFO *fi, char out[13])
{
   if (fi->altname[0] != '\0') {
      snprintf(out, 13, "%s", fi->altname);
      return;
   }
   const char *name = fi->fname;
   const char *dot = strrchr(name, '.');
   size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
   size_t ext_len = dot ? strlen(dot + 1) : 0u;
   bool fits = base_len >= 1u && base_len <= 8u && ext_len <= 3u;
   for (const char *c = name; *c && fits; c++) {
      unsigned char u = (unsigned char)*c;
      if (c == dot)
         continue;
      if (!(isalnum(u) || strchr("!#$%&'()-@^_`{}~", u)))
         fits = false;
   }
   size_t o = 0;
   if (fits) {
      for (size_t i = 0; i < base_len; i++)
         out[o++] = (char)toupper((unsigned char)name[i]);
   } else {
      uint32_t h = 2166136261u;          /* FNV-1a */
      for (const char *c = name; *c; c++)
         h = (h ^ (unsigned char)*c) * 16777619u;
      for (const char *c = name; *c && o < 4u && c != dot; c++)
         if (isalnum((unsigned char)*c))
            out[o++] = (char)toupper((unsigned char)*c);
      if (o == 0u)
         out[o++] = '_';
      o += (size_t)snprintf(&out[o], 13u - o, "~%03X", (unsigned)(h & 0xFFFu));
   }
   if (ext_len > 0u) {
      out[o++] = '.';
      for (size_t i = 0; i < ext_len && i < 3u; i++)
         out[o++] = (char)toupper((unsigned char)dot[1 + i]);
   }
   out[o] = '\0';
}

/* This boot's id (command 20): never 0, the same until the Pi restarts.
   Taken at the first ask, from the time - by then the poll loop is
   running, and when the Beeb first asks varies from boot to boot. */
static uint16_t fat_boot_id(void)
{
   if (fat_id == 0u) {
      fat_id = (uint16_t)(Pi1MHz_now_us ^ (Pi1MHz_now_us >> 16));
      if (fat_id == 0u)
         fat_id = 1u;
   }
   return fat_id;
}

/* An eject, or the Beeb's own mount or unmount (commands 14, 15): the
   card may be another one, so the slots forget their kept files and
   directories, and the boot id changes - a client keeping state over a
   reset (MMFS2) then starts again, as after a Pi restart.  A BBC reset
   keeps them (fat_close_all only). */
static void fat_forget_kept(void)
{
   for (unsigned int i = 0; i < 16u; i++) {
      fat_keep_mode[i] = 0u;
      fat_slot_dir[i][0] = '\0';
   }
   if (fat_id != 0u && ++fat_id == 0u)
      fat_id = 1u;
}

/* A rename of from to to (absolute paths): a kept file, or a slot's
   directory, at or under from moves with it, so a BBC reset reopens the
   file under its new name - and the host-write interlock follows. */
static void fat_rename_follow(const char *from, const char *to)
{
   size_t n = strlen(from);
   for (unsigned int i = 0; i < 16u; i++) {
      char *p[2] = { fat_keep_mode[i] != 0u ? fat_keep_path[i] : NULL, fat_slot_dir[i] };
      for (unsigned int k = 0; k < 2u; k++) {
         char moved[FAT_OPEN_PATH_MAX];
         if (p[k] == NULL || strncasecmp(p[k], from, n) != 0 ||
             (p[k][n] != '\0' && p[k][n] != '/'))
            continue;
         int m = snprintf(moved, sizeof moved, "%s%s", to, p[k] + n);
         if (m < 0 || (size_t)m >= sizeof moved)
            continue;
         strcpy(p[k], moved);
         if (k == 0u && fat_file_open[i])
            fat_open_record(i, p[k]);
      }
   }
}

/* In directory dir (absolute), the long name of the entry whose 8.3 name is
   short83 (case-insensitive): a made-up name back to the real one. */
static bool fat_slot_find(const char *dir, const char *short83, char *name, size_t size)
{
   DIR d;
   FILINFO fi;
   char buf[FAT_OPEN_PATH_MAX + 3];
   bool found = false;
   if (fsr_opendir(&d, fat_storage(dir, buf, sizeof buf)) != FR_OK)
      return false;
   while (fsr_readdir(&d, &fi) == FR_OK && fi.fname[0] != '\0') {
      char sn[13];
      fat_short_name(&fi, sn);
      if (strcasecmp(sn, short83) == 0 && strlen(fi.fname) < size) {
         strcpy(name, fi.fname);
         found = true;
         break;
      }
   }
   (void)fsr_closedir(&d);
   return found;
}

/* The absolute path of name on a slot.  When it does not exist as given,
   its last part may be an 8.3 name made by fat_short_name(): look it up. */
static bool fat_slot_resolve(unsigned int slot, const char *name, char *abs, size_t size)
{
   char buf[FAT_OPEN_PATH_MAX + 3];
   FILINFO fi;
   if (!fat_join(abs, size, fat_slot_base(slot), name))
      return false;
   if (strcmp(abs, "/") == 0 || fsr_stat(fat_storage(abs, buf, sizeof buf), &fi) == FR_OK)
      return true;
   char *last = strrchr(abs, '/');
   char parent[FAT_OPEN_PATH_MAX], real[FAT_OPEN_PATH_MAX];
   size_t plen = (size_t)(last - abs);
   memcpy(parent, abs, plen);
   parent[plen] = '\0';
   if (plen == 0u)
      strcpy(parent, "/");
   if (fat_slot_find(parent, last + 1, real, sizeof real) &&
       strlen(real) < size - (size_t)(last + 1 - abs))
      strcpy(last + 1, real);
   return true;                         /* else as given: the call reports it missing */
}

/* A kept file closed by a BBC reset: open it again, by its path. */
static FRESULT fat_slot_reopen(unsigned int slot)
{
   char buf[FAT_OPEN_PATH_MAX + 3];
   if (fat_file_open[slot] || fat_keep_mode[slot] == 0u)
      return fat_file_open[slot] ? FR_OK : FR_INVALID_OBJECT;
   if (fat_keep_vol[slot] != filesystemStorageRoot()[0])
      return FR_NO_FILE;                /* not the storage it was opened on */
   const char *path = fat_storage(fat_keep_path[slot], buf, sizeof buf);
   FRESULT r = fsr_open(&fileObject[slot], path, fat_keep_mode[slot]);
   if (r == FR_DENIED && (fat_keep_mode[slot] & FA_WRITE))
      r = fsr_open(&fileObject[slot], path, FA_READ);   /* locked since: read only */
   if (r == FR_OK) {
      fat_file_open[slot] = true;
      fat_open_record(slot, fat_keep_path[slot]);
   }
   return r;
}

/* Before a read, write or size: a kept file a reset closed is reopened.
   False (the result already written) if that failed. */
static bool fat_slot_ready(unsigned int slot, uint32_t addr)
{
   if (fat_file_open[slot] || fat_keep_mode[slot] == 0u)
      return true;
   FRESULT r = fat_slot_reopen(slot);
   if (r == FR_OK)
      return true;
   Pi1MHz_MemoryWrite(addr, r);
   return false;
}

/* ---- directory block (command 21) ---------------------------------------
   Entries [16n, 16n+16) of a slot's directory as 32-byte FAT directory
   entries, the form MMFS2's catalogue code reads: 8.3 name, attributes,
   times, size.  A subdirectory starts with "." and "..", as on FAT; names
   starting with '.' (host files such as .DS_Store) are left out.  The
   cluster field carries a 20-bit hash of the name (nonzero; ".." of a
   first-level directory 0, the root, as on FAT): MMFS2 compares it to see
   whether an image is already in the other drive, and never reads it as a
   cluster.  Unused entries are zero: the first is the end of the
   directory.  The whole directory is read from the start each time. */
static void fat_put_dirent(uint8_t *e, const char *short83, BYTE attr, uint32_t size,
                           uint16_t date, uint16_t time, uint32_t id)
{
   memset(e, ' ', 11);
   const char *dot = strchr(short83, '.');
   size_t bl = dot ? (size_t)(dot - short83) : strlen(short83);
   if (strcmp(short83, ".") == 0 || strcmp(short83, "..") == 0)
      bl = strlen(short83), dot = NULL;
   memcpy(e, short83, bl > 8u ? 8u : bl);
   if (dot)
      memcpy(e + 8, dot + 1, strlen(dot + 1) > 3u ? 3u : strlen(dot + 1));
   e[11] = (uint8_t)(attr & (AM_RDO | AM_HID | AM_SYS | AM_DIR | AM_ARC));
   e[14] = e[22] = (uint8_t)time;  e[15] = e[23] = (uint8_t)(time >> 8);
   e[16] = e[18] = e[24] = (uint8_t)date;  e[17] = e[19] = e[25] = (uint8_t)(date >> 8);
   e[20] = (uint8_t)(id >> 16);  e[21] = 0;
   e[26] = (uint8_t)id;  e[27] = (uint8_t)(id >> 8);
   e[28] = (uint8_t)size; e[29] = (uint8_t)(size >> 8);
   e[30] = (uint8_t)(size >> 16); e[31] = (uint8_t)(size >> 24);
}

static uint32_t fat_name_id(const char *dir, const char *name)
{
   uint32_t h = 2166136261u;
   for (const char *c = dir; *c; c++)
      h = (h ^ (unsigned char)tolower((unsigned char)*c)) * 16777619u;
   h = (h ^ '/') * 16777619u;
   for (const char *c = name; *c; c++)
      h = (h ^ (unsigned char)tolower((unsigned char)*c)) * 16777619u;
   h &= 0xFFFFFu;
   return h ? h : 1u;
}

static FRESULT fat_dir_block(unsigned int slot, uint32_t block, uint8_t *out)
{
   const char *dir = fat_slot_base(slot);
   char buf[FAT_OPEN_PATH_MAX + 3];
   uint32_t first = block * 16u, index = 0;
   bool root = strcmp(dir, "/") == 0;
   DIR d;
   FILINFO fi;

   memset(out, 0, 512);
   FRESULT r = fsr_opendir(&d, fat_storage(dir, buf, sizeof buf));
   if (r != FR_OK)
      return r;
   if (!root) {
      char parent[FAT_OPEN_PATH_MAX];
      (void)fat_join(parent, sizeof parent, dir, "..");
      for (int k = 0; k < 2; k++, index++)
         if (index >= first && index < first + 16u)
            fat_put_dirent(out + (index - first) * 32u, k ? ".." : ".", AM_DIR, 0, 0, 0,
                           k == 0 ? fat_name_id(dir, "")
                                  : (strcmp(parent, "/") == 0 ? 0u : fat_name_id(parent, "")));
   }
   while (index < first + 16u && (r = fsr_readdir(&d, &fi)) == FR_OK && fi.fname[0] != '\0') {
      if (fi.fname[0] == '.')
         continue;
      if (index >= first) {
         char sn[13];
         fat_short_name(&fi, sn);
         fat_put_dirent(out + (index - first) * 32u, sn, fi.fattrib,
                        (fi.fattrib & AM_DIR) || fi.fsize > 0xFFFFFFFFu ? 0u : (uint32_t)fi.fsize,
                        fi.fdate, fi.ftime, fat_name_id(dir, fi.fname));
      }
      index++;
   }
   (void)fsr_closedir(&d);
   return r;
}

/* Runs on the main loop (fat_service_poll): every FatFs and SD call below
   may block for milliseconds, which the FIQ cannot afford - see
   fat_service_command. */
static void fat_service_execute(uint32_t command_pointer, uint32_t addr, uint8_t data)
{
   uint32_t base_addr = DISC_RAM_BASE ;

   switch (  Pi1MHz->JIM_ram[command_pointer] )
   {
    case 0 :
    {
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t sectors = jim_read32(command_pointer+12);
        BYTE pdrv = fat_raw_drive(Pi1MHz->JIM_ram[command_pointer+1]);
        if (pdrv == 0u)
            fat_raw_sector_seen = true;      /* the card's BEEB.MMB is in use */
        // disk_read transfers 'sectors' x 512-byte blocks into the buffer
        if ((sectors > (DISC_RAM_SIZE / DISC_SECTOR_SIZE)) ||
            !service_buffer_ok(buf_off, sectors * DISC_SECTOR_SIZE))
        {
            Pi1MHz_MemoryWrite(addr, RES_PARERR);
            break;
        }
        Pi1MHz_MemoryWrite(addr,
            disk_read( pdrv,
                        &Pi1MHz->JIM_ram[buf_off+base_addr],
                        jim_read32(command_pointer+8),
                        sectors
                        ) );
        break;
    }
    case 1 :
    {
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t sectors = jim_read32(command_pointer+12);
        BYTE pdrv = fat_raw_drive(Pi1MHz->JIM_ram[command_pointer+1]);
        if (pdrv == 0u)
            fat_raw_sector_seen = true;
        // disk_write transfers 'sectors' x 512-byte blocks from the buffer
        if (config_beeb_write_protected())      // Beeb writes ignored: report OK
        {
            Pi1MHz_MemoryWrite(addr, RES_OK);
            break;
        }
        if ((sectors > (DISC_RAM_SIZE / DISC_SECTOR_SIZE)) ||
            !service_buffer_ok(buf_off, sectors * DISC_SECTOR_SIZE))
        {
            Pi1MHz_MemoryWrite(addr, RES_PARERR);
            break;
        }
        Pi1MHz_MemoryWrite(addr,
            disk_write( pdrv,
                        &Pi1MHz->JIM_ram[buf_off+base_addr],
                        jim_read32(command_pointer+8) ,
                        sectors )
                        );
        break;
    }
    case 2 :
    {
        FRESULT result;
        // Filename defined to be zero terminated string at command_pointer+3, mode in command_pointer+2
        if (!service_string_ok(command_pointer+3, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        fat_open_valid[data & 15] = false;   /* re-open replaces any record */
        BYTE mode = Pi1MHz->JIM_ram[command_pointer+2];
        bool keep = (mode & FAT_MODE_KEEP) != 0u;
        mode &= (BYTE)~FAT_MODE_KEEP;
        if (config_beeb_write_protected())
            mode = FA_READ;                  /* strip write/create bits: read-only open */
        fat_keep_mode[data & 15] = 0u;       /* a new open forgets the kept file */
        if (keep || fat_slot_dir[data & 15][0] != '\0') {
            /* by absolute path: the slot's own directory, a made-up 8.3
               name resolved, and the path kept to reopen after a reset */
            char abs[FAT_OPEN_PATH_MAX];
            if (fat_file_open[data & 15]) {
                fat_file_open[data & 15] = false;
                (void)fsr_close(&fileObject[data & 15]);
            }
            if (!fat_slot_resolve(data & 15, (char *)&Pi1MHz->JIM_ram[command_pointer+3],
                                  abs, sizeof abs)) {
                Pi1MHz_MemoryWrite(addr, FR_INVALID_NAME);
                break;
            }
            result = fsr_open(&fileObject[data & 15], fat_storage(abs, fat_path_a, sizeof fat_path_a),
                            mode);
            if (result == FR_OK) {
                fat_file_open[data & 15] = true;
                fat_open_record(data & 15, abs);
                if (keep) {
                    strcpy(fat_keep_path[data & 15], abs);
                    fat_keep_mode[data & 15] = mode & (BYTE)(FA_READ | FA_WRITE);
                    fat_keep_vol[data & 15] = filesystemStorageRoot()[0];
                }
            }
            Pi1MHz_MemoryWrite(addr, result);
            break;
        }
        result = fsr_open( &fileObject[data & 15],
                         fat_beeb_path(command_pointer+3, fat_path_a, sizeof fat_path_a),
                         mode );
        if (result == FR_OK) {
            fat_file_open[data & 15] = true;
            fat_open_record(data & 15, (char * )&Pi1MHz->JIM_ram[command_pointer+3]);
        }
        Pi1MHz_MemoryWrite(addr, result);
        break;
    }
    case 3 :
        if (!fat_file_open[data & 15] && fat_keep_mode[data & 15] != 0u)
        {
            fat_keep_mode[data & 15] = 0u;   /* kept, closed by a reset: forget it */
            Pi1MHz_MemoryWrite(addr, FR_OK);
            break;
        }
        fat_keep_mode[data & 15] = 0u;
        if (!fat_file_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        fat_open_valid[data & 15] = false;
        fat_file_open[data & 15] = false;
        Pi1MHz_MemoryWrite(addr,
             fsr_close( &fileObject[data & 15] ) );
        break;
    case 4 :
    {
        FRESULT result;
        UINT length;
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t buf_len = jim_read32(command_pointer)>>8;
        if (!service_buffer_ok(buf_off, buf_len))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        if (!fat_slot_ready(data & 15, addr))
            break;
        if (!fat_file_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        result = fsr_lseek( &fileObject[data & 15], jim_read32(command_pointer+8) );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        result = fsr_read( &fileObject[data & 15], &Pi1MHz->JIM_ram[buf_off+base_addr] , buf_len , &length);
        if (result == FR_OK && length < buf_len)   /* past the end: zeros, not stale bytes */
            memset(&Pi1MHz->JIM_ram[buf_off + base_addr + length], 0, buf_len - length);
        jim_write32(command_pointer, (length << 8 ) | Pi1MHz->JIM_ram[command_pointer]);
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if ( length < buf_len )
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }
    case 5 :
    {
        FRESULT result;
        UINT length;
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t buf_len = jim_read32(command_pointer)>>8;
        if (!service_buffer_ok(buf_off, buf_len))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        if (!fat_slot_ready(data & 15, addr))
            break;
        if (!fat_file_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        if (config_beeb_write_protected())      // Beeb write ignored: claim it all landed
        {
            jim_write32(command_pointer, (buf_len << 8 ) | Pi1MHz->JIM_ram[command_pointer]);
            Pi1MHz_MemoryWrite(addr, FR_OK);
            break;
        }
        result = fsr_lseek( &fileObject[data & 15], jim_read32(command_pointer+8) );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        result = fsr_write( &fileObject[data & 15], &Pi1MHz->JIM_ram[buf_off+base_addr] , buf_len , &length);
        jim_write32(command_pointer, (length << 8 ) | Pi1MHz->JIM_ram[command_pointer]);
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if ( length < buf_len )
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }
    case 6 : // fsize
    {
        if (!fat_slot_ready(data & 15, addr))
            break;
        if (!fat_file_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        /* 32 bits for the Beeb: an exFAT file of 4 GB or more reads as
           &FFFFFFFF rather than wrapping to a small size. */
        FSIZE_t size = f_size( &fileObject[data & 15] );
        jim_write32(command_pointer + 8, size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size);
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 7 : // fopendir
        if (!service_string_ok(command_pointer + 1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        {
            FRESULT dresult = fsr_opendir( (DIR * )&dirObject[data & 15],
                                         fat_beeb_path(command_pointer + 1, fat_path_a, sizeof fat_path_a) );
            if (dresult == FR_OK)
                fat_dir_open[data & 15] = true;
            Pi1MHz_MemoryWrite(addr, dresult);
        }
        break;


    case 8: // fclosedir
        if (!fat_dir_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        fat_dir_open[data & 15] = false;
        Pi1MHz_MemoryWrite(addr,
             fsr_closedir( (DIR * )&dirObject[data & 15] ) );
        break;


    case 9 : // f readdir
    {
        FRESULT result;
        FILINFO fileInfo;
        if (!fat_dir_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        result = fsr_readdir( (DIR * )&dirObject[data & 15], &fileInfo );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if (fileInfo.fname[0] == 0)
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }

        {
            /* Cap to the 252 bytes left in this 256-byte command page: a
               255-char LFN + NUL otherwise spills 4 bytes into the next
               page's command block. */
            size_t nlen = strlen(fileInfo.fname);
            if (nlen > 251) nlen = 251;
            memcpy(&Pi1MHz->JIM_ram[command_pointer + 4], fileInfo.fname, nlen);
            Pi1MHz->JIM_ram[command_pointer + 4 + nlen] = 0;
        }
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 10 : // f mkdir
        if (!service_string_ok(command_pointer + 1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        Pi1MHz_MemoryWrite(addr, config_beeb_write_protected() ? FR_OK :
             fsr_mkdir( fat_beeb_path(command_pointer + 1, fat_path_a, sizeof fat_path_a) ) );
        break;

    case 11 : // fchdir
    {
        FRESULT result;
        if (!service_string_ok(command_pointer + 1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        const char *name = (const char *)&Pi1MHz->JIM_ram[command_pointer + 1];
        if (fat_on_share() && !fat_names_volume(name)) {
            Pi1MHz_MemoryWrite(addr, fat_share_chdir(name));
            break;
        }
        result = f_chdir( fat_beeb_path(command_pointer + 1, fat_path_a, sizeof fat_path_a) );
        if (result == FR_OK)
            fat_cwd_known = (fat_getcwd(fat_cwd, sizeof fat_cwd) == FR_OK);
        Pi1MHz_MemoryWrite(addr, result);
        break;
    }

    case 12 : // f_rename
    {
        // Two NUL-terminated names back to back; the second starts after the first.
        uint32_t name1 = command_pointer + 1;
        if (!service_string_ok(name1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        uint32_t name2 = name1 + (uint32_t)strlen((char * )&Pi1MHz->JIM_ram[name1]) + 1;
        if (!service_string_ok(name2, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        if (fat_slot_dir[data & 15][0] != '\0') {
            /* in the slot's own directory; the new name there too */
            char from[FAT_OPEN_PATH_MAX], to[FAT_OPEN_PATH_MAX];
            if (!fat_slot_resolve(data & 15, (char *)&Pi1MHz->JIM_ram[name1], from, sizeof from) ||
                !fat_join(to, sizeof to, fat_slot_base(data & 15), (char *)&Pi1MHz->JIM_ram[name2])) {
                Pi1MHz_MemoryWrite(addr, FR_INVALID_NAME);
                break;
            }
            if (config_beeb_write_protected()) {
                Pi1MHz_MemoryWrite(addr, FR_OK);
                break;
            }
            FRESULT r = fsr_rename(fat_storage(from, fat_path_a, sizeof fat_path_a),
                                 fat_storage(to, fat_path_b, sizeof fat_path_b));
            if (r == FR_OK)
                fat_rename_follow(from, to);
            Pi1MHz_MemoryWrite(addr, r);
            break;
        }
        Pi1MHz_MemoryWrite(addr, config_beeb_write_protected() ? FR_OK :
             fsr_rename( fat_beeb_path(name1, fat_path_a, sizeof fat_path_a),
                       fat_beeb_path(name2, fat_path_b, sizeof fat_path_b) ) );
        break;
    }

    case 13 : // fgetfree
    {
        /* The Beeb blocks on this, but so does everything else: f_getfree
           walks the whole FAT (FF_FS_NOFSINFO=3) and used to freeze the
           poll loop - audio, video and SCSI servicing included - for
           seconds each time a program asked for free space. Answer from
           the webserver's background sweep, which pays at most one
           blocking scan ever and refreshes itself afterwards. The figure
           can lag recent writes; it is advisory (a free-space display),
           and the alternative is a multi-second stall per query. */
        uint64_t total_bytes = 0, free_bytes = 0;
        if (fat_off_card()) {
            /* The drive has no background sweep: FatFs scans its FAT on the
               first ask (a few seconds on a big drive), then keeps the
               count up to date itself.  The share's server answers. */
            FRESULT result = fsr_free_bytes(filesystemStorageRoot(), &free_bytes);
            if (result != FR_OK) {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        }
        else if (!webserver_sd_space_now(&total_bytes, &free_bytes))
            {
                Pi1MHz_MemoryWrite(addr, FR_DISK_ERR);
                break;
            }
        jim_write32(command_pointer+8, (uint32_t)(free_bytes / 256u));  // free space in bytes/256
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 14 : // f mount
        (void)fat_service_eject();      /* close, not just forget: see below; and
                                           the card may be another one now */
        if (filesystemMount())
         {
            Pi1MHz_MemoryWrite(addr, FR_OK);
         }
         else
         {
            Pi1MHz_MemoryWrite(addr, FR_DISK_ERR);
         }
        break;

    case 15 : // f unmount
        /* Close what the Beeb has open before the dismount, so a file it was
           writing keeps what it wrote.  Clearing the tracking first (as this
           did) left the remount hook nothing to close.  Poll context, not
           FIQ, so the FatFs calls are safe here. */
        (void)fat_service_eject();
        if (filesystemDismount())
        {
            Pi1MHz_MemoryWrite(addr, FR_OK);
        }
        else
        {
            Pi1MHz_MemoryWrite(addr, FR_DISK_ERR);
        }
        break;
    case 16 : // f_unlink
        if (!service_string_ok(command_pointer + 1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        Pi1MHz_MemoryWrite(addr, config_beeb_write_protected() ? FR_OK :
             fsr_unlink( fat_beeb_path(command_pointer + 1, fat_path_a, sizeof fat_path_a) ) );
        break;

    case 17 : // readdir-ex: next entry as a fixed 128-byte record in the buffer
    {
        FRESULT result;
        FILINFO fileInfo;
        uint32_t buf_off = jim_read32(command_pointer+4);
        if (!service_buffer_ok(buf_off, FAT_DIRENT_SIZE))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        if (!fat_dir_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        result = fsr_readdir( (DIR * )&dirObject[data & 15], &fileInfo );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if (fileInfo.fname[0] == 0)
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }
        fat_dirent_record(&Pi1MHz->JIM_ram[buf_off + base_addr], &fileInfo);
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 18 : // getcwd: current directory as a string in the buffer
    {
        char cwd[FAT_CWD_MAX];
        uint32_t buf_off = jim_read32(command_pointer+4);
        FRESULT result;
        if (!service_buffer_ok(buf_off, FAT_CWD_MAX))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        result = fat_getcwd(cwd, sizeof cwd);
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        for (uint32_t i = 0; cwd[i]; i++)
            if ((uint8_t)cwd[i] < 32u || (uint8_t)cwd[i] > 126u)
                cwd[i] = '?';           /* keep the Beeb's VDU stream safe */
        memcpy(&Pi1MHz->JIM_ram[buf_off + base_addr], cwd, strlen(cwd)+1);
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 19 : // set attributes: +1 the bits to change, +2 their values, +3 the name
    {
        if (!service_string_ok(command_pointer + 3, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        BYTE mask = Pi1MHz->JIM_ram[command_pointer + 1] & (BYTE)(AM_RDO | AM_HID | AM_SYS | AM_ARC);
        BYTE attr = Pi1MHz->JIM_ram[command_pointer + 2];
        char abs[FAT_OPEN_PATH_MAX];
        const char *path;
        if (fat_slot_dir[data & 15][0] != '\0') {
            if (!fat_slot_resolve(data & 15, (char *)&Pi1MHz->JIM_ram[command_pointer + 3],
                                  abs, sizeof abs)) {
                Pi1MHz_MemoryWrite(addr, FR_INVALID_NAME);
                break;
            }
            path = fat_storage(abs, fat_path_a, sizeof fat_path_a);
        } else
            path = fat_beeb_path(command_pointer + 3, fat_path_a, sizeof fat_path_a);
        Pi1MHz_MemoryWrite(addr, (uint8_t)(config_beeb_write_protected() ? FR_OK :
                                           fsr_chmod(path, attr, mask)));
        break;
    }

    /* The card's type (SDHC: block addressed); the drive is block addressed
       too, and so is the share as far as the Beeb can tell.  +1,+2 of the block get this boot's id, so a client that keeps
       its own state over a BBC reset (MMFS2's drives and directory, held
       here by slot) can tell when the Pi has restarted and forgotten them.
       An older Pi leaves +1,+2 as the client wrote them. */
    case 20 :
        Pi1MHz->JIM_ram[command_pointer + 1] = (uint8_t)fat_boot_id();
        Pi1MHz->JIM_ram[command_pointer + 2] = (uint8_t)(fat_boot_id() >> 8);
        Pi1MHz_MemoryWrite(addr, fat_off_card() ? 1u : disk_type());
        break;

    case 21 : // directory block: +4 buffer (512 bytes), +8 block number
    {
        uint32_t buf_off = jim_read32(command_pointer + 4);
        if (!service_buffer_ok(buf_off, 512u))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        Pi1MHz_MemoryWrite(addr, fat_dir_block(data & 15, jim_read32(command_pointer + 8),
                                               &Pi1MHz->JIM_ram[buf_off + base_addr]));
        break;
    }

    case 22 : // the slot's own directory: +1 name, relative to it ("" or "/" = the root)
    {
        if (!service_string_ok(command_pointer + 1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        const char *name = (char *)&Pi1MHz->JIM_ram[command_pointer + 1];
        char abs[FAT_OPEN_PATH_MAX];
        FILINFO fi;
        if (fat_slot_dir[data & 15][0] == '\0')
            strcpy(fat_slot_dir[data & 15], "/");   /* first use: from the root */
        if (!fat_slot_resolve(data & 15, name[0] ? name : "/", abs, sizeof abs)) {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_NAME);
            break;
        }
        FRESULT r = FR_OK;
        if (strcmp(abs, "/") != 0) {
            r = fsr_stat(fat_storage(abs, fat_path_a, sizeof fat_path_a), &fi);
            if (r == FR_OK && !(fi.fattrib & AM_DIR))
                r = FR_NO_PATH;
            if (r == FR_NO_FILE)
                r = FR_NO_PATH;
        }
        if (r == FR_OK)
            strcpy(fat_slot_dir[data & 15], abs);
        Pi1MHz_MemoryWrite(addr, r);
        break;
    }

    default :
        /* 23-29 are reserved within the FAT range; ignored. */
        break;
   }

}

/* One-slot command mailbox latched in FIQ, drained by the poll (the net
   service's pattern).  The services port dispatches command-register
   writes from the FIQ; the FatFs work behind them reads the SD card for
   milliseconds, and a FIQ held that long lets the 8-slot bus post ring
   overrun whenever the Beeb keeps the bus busy meanwhile - the helper ROM
   loader executes from JIM, so every instruction fetch is a posted cycle.
   The ring then drops the writes that follow, and the loader hung on its
   next command.  The Beeb already polls bit 7 of the command register, so
   deferring the work changes nothing on its side. */
static volatile bool     fat_pending;
static volatile uint32_t fat_pending_cp;
static volatile uint32_t fat_pending_addr;
static volatile uint8_t  fat_pending_data;

static void fat_service_command(uint32_t command_pointer, uint32_t addr, uint8_t data)
{
   /* FIQ context: latch only.  The register must read busy (bit 7) until
      the poll writes the real result; every Beeb client sends a command
      byte with bit 7 set, so this is the byte it wrote back with the bit
      forced for safety. */
   fat_pending_cp   = command_pointer;
   fat_pending_addr = addr;
   fat_pending_data = data;
   fat_pending      = true;
   Pi1MHz_MemoryWrite(addr, (uint8_t)(data | 0x80u));
}

static void fat_service_poll(void)
{
   if (!fat_pending)
      return;
   uint32_t cp   = fat_pending_cp;
   uint32_t addr = fat_pending_addr;
   uint8_t  data = fat_pending_data;
   fat_pending = false;
   fat_service_execute(cp, addr, data);
}

/* SD card eject (filesystemEject): close what the Beeb has open here, so a
   file it was writing reaches the card before the card goes. */
static void fat_close_all(void)
{
   for (unsigned int i = 0; i < 16u; i++) {
      if (fat_file_open[i])
         (void)fsr_close(&fileObject[i]);
      if (fat_dir_open[i])
         (void)fsr_closedir(&dirObject[i]);
   }
   fat_open_clear_all();
}

static bool fat_service_eject(void)
{
   fat_close_all();
   fat_forget_kept();
   return true;
}

/* The volume is about to be re-registered (filesystemRegisterRemount: a BBC
   reset, the Beeb's f unmount, a mount after one), which would
   invalidate every FIL open here for good - an unsynced write dropped, the
   file keeping its old size, the clusters written orphaned.  On a BBC reset
   the hard disc's filesystemReset() does this before fat_service_init()
   runs again, so close them here, as at eject.  Main loop only: both run
   from init_emulator and the poll, never from the FIQ. */
static void fat_service_remount(void)
{
   fat_close_all();                     /* kept files stay kept: see FAT_MODE_KEEP */
}

void fat_service_init(void)
{
   /* Runs on every BBC RST (init_emulator re-runs the whole table), in the
      main loop.  The Beeb-side filing system restarts on reset and abandons
      whatever it had open through this service, so close those files - a
      file it was writing then keeps what reached it - and drop the
      open-file tracking and the raw-sector (BEEB.MMB) latch: otherwise the
      webserver's in-use interlock would report ghosts of a pre-reset
      session as busy until the Pi itself rebooted.  If the Beeb re-opens
      files after the reset, the tracking simply re-populates.  Usually the
      remount hook has closed them already and this finds nothing open.
      Kept files and slot directories survive it (FAT_MODE_KEEP). */
   fat_close_all();
   (void)services_register(SERVICE_CMD_FAT_FIRST, SERVICE_CMD_FAT_LAST,
                           fat_service_command);
   /* fat_pending is deliberately not cleared here: a command latched around
      the reset is still answered by the poll (against the now-closed
      handles), so the busy bit the FIQ wrote can never be stranded. */
   Pi1MHz_Register_Poll(fat_service_poll, "fatsvc");
   filesystemRegisterEject(fat_service_eject, NULL);
   filesystemRegisterRemount(fat_service_remount);
}
