/* Host tests for the services port (services_emulator.c) and the FAT
 * service's open-file interlock (fat_service.c).  Everything is driven
 * through the real dispatch: commands are built as the Beeb would build
 * them (block in the top JIM pages, dispatched by the FRED command
 * register callback), and results are read back from the FRED shadow.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Pi1MHz.h"
#include "services.h"
#include "config.h"
#include "BeebSCSI/fatfs/ff.h"
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"

/* config.c (linked in for the Beeb_write_protect tests) calls this. */
uint32_t filesystemReadFile(const char *f, uint8_t **a, unsigned int m)
{ (void)f; (void)a; (void)m; return 0; }

/* ---- Pi1MHz stubs ---- */
static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;

#define SVC_BASE 0xA6u
static callback_func_ptr write_cb[256];
static callback_func_ptr read_cb[256];

void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr fn)
{
   if (access == WRITE_FRED) write_cb[addr & 0xff] = fn;
   else                      read_cb[addr & 0xff] = fn;
}
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data)  { pi.Memory[addr & 0x1ff] = data; }
void Pi1MHz_MemoryWrite16(uint32_t addr, uint32_t data)
{ pi.Memory[addr & 0x1ff] = (uint8_t)data; pi.Memory[(addr + 1u) & 0x1ff] = (uint8_t)(data >> 8); }
void Pi1MHz_nIRQ_ASSERT(uint8_t src) { (void)src; }
void Pi1MHz_nIRQ_CLEAR(uint8_t src) { (void)src; }

/* ---- FatFs stubs: record calls, results settable per test ---- */
static FRESULT open_result = FR_OK;
static char    last_open_path[512];
static FRESULT chdir_result = FR_OK;
static char    cwd_value[128] = "/";
static FRESULT getcwd_result = FR_OK;

/* write-observing counters + open-mode capture (for Beeb_write_protect) */
static int f_write_calls, disk_write_calls, f_mkdir_calls, f_unlink_calls, f_rename_calls;
static uint8_t last_open_mode;

FRESULT f_open(FIL *fp, const char *path, uint8_t mode)
{ (void)fp; last_open_mode = mode; snprintf(last_open_path, sizeof last_open_path, "%s", path); return open_result; }
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_read(FIL *fp, void *b, UINT n, UINT *r) { (void)fp; (void)b; *r = n; return FR_OK; }
FRESULT f_write(FIL *fp, const void *b, UINT n, UINT *w) { (void)fp; (void)b; f_write_calls++; *w = n; return FR_OK; }
FRESULT f_lseek(FIL *fp, uint32_t ofs) { (void)fp; (void)ofs; return FR_OK; }
FRESULT f_opendir(DIR *dp, const char *p) { (void)dp; (void)p; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }
FRESULT f_readdir(DIR *dp, FILINFO *fno) { (void)dp; fno->fname[0] = 0; return FR_OK; }
FRESULT f_mkdir(const char *p) { (void)p; f_mkdir_calls++; return FR_OK; }
FRESULT f_chdir(const char *p) { (void)p; return chdir_result; }
FRESULT f_getcwd(char *buff, UINT len) { snprintf(buff, len, "%s", cwd_value); return getcwd_result; }
FRESULT f_rename(const char *a, const char *b) { (void)a; (void)b; f_rename_calls++; return FR_OK; }
bool webserver_sd_space_now(uint64_t *t, uint64_t *f) { *t = 0; *f = 0; return false; }
FRESULT f_getfree(const char *p, DWORD *n, FATFS **f) { (void)p; (void)n; (void)f; return FR_DISK_ERR; }
FRESULT f_unlink(const char *p) { (void)p; f_unlink_calls++; return FR_OK; }

DRESULT disk_read(uint8_t d, uint8_t *b, uint32_t s, unsigned int c)
{ (void)d; (void)b; (void)s; (void)c; return RES_OK; }
DRESULT disk_write(uint8_t d, const uint8_t *b, uint32_t s, unsigned int c)
{ (void)d; (void)b; (void)s; (void)c; disk_write_calls++; return RES_OK; }
unsigned char disk_type(void) { return 42; }

bool filesystemMount(void) { return true; }
bool filesystemDismount(void) { return true; }

/* ---- AUN-range test handler ---- */
static uint32_t aun_calls;
static uint8_t  aun_last_cmd;
static void test_aun_handler(uint32_t cp, uint32_t addr, uint8_t data)
{ (void)addr; (void)data; aun_calls++; aun_last_cmd = Pi1MHz->JIM_ram[cp]; }

/* ---- helpers driving the real dispatch ---- */
#define CMD_PAGE 0xF3u
static uint32_t cp_of(uint8_t page) { return DISC_RAM_BASE | 0xFF0000u | ((uint32_t)page << 8); }

static uint8_t dispatch(uint8_t page)
{
   write_cb[SVC_BASE + 4](TEST_GPIO(SVC_BASE + 4, page));
   return pi.Memory[SVC_BASE + 4];
}

/* Issue FAT command 2 (open) for handle h on `name`; returns FRESULT byte. */
static uint8_t do_open(uint8_t h, const char *name)
{
   uint32_t cp = cp_of((uint8_t)(0xF0u + h));
   Pi1MHz->JIM_ram[cp] = 2;
   Pi1MHz->JIM_ram[cp + 2] = FA_READ | FA_WRITE;
   strcpy((char *)&Pi1MHz->JIM_ram[cp + 3], name);
   return dispatch((uint8_t)(0xF0u + h));
}

static uint8_t do_simple(uint8_t h, uint8_t cmd)
{
   uint32_t cp = cp_of((uint8_t)(0xF0u + h));
   memset(&Pi1MHz->JIM_ram[cp], 0, 64);
   Pi1MHz->JIM_ram[cp] = cmd;
   return dispatch((uint8_t)(0xF0u + h));
}

static uint8_t do_chdir(const char *path)
{
   uint32_t cp = cp_of(CMD_PAGE);
   Pi1MHz->JIM_ram[cp] = 11;
   strcpy((char *)&Pi1MHz->JIM_ram[cp + 1], path);
   return dispatch(CMD_PAGE);
}

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

int main(void)
{
   pi.JIM_ram = calloc(1, 32u * 1024u * 1024u);
   pi.JIM_ram_size = 2;             /* DISC_RAM_BASE == 0 */
   assert(pi.JIM_ram != NULL);

   services_emulator_init(0, SVC_BASE);

   puts("== registration ==");
   ok(services_register(SERVICE_CMD_AUN_FIRST, SERVICE_CMD_AUN_LAST, test_aun_handler),
      "AUN range registers");
   ok(services_register(SERVICE_CMD_AUN_FIRST, SERVICE_CMD_AUN_LAST, test_aun_handler),
      "identical reset-time claim renews without consuming a slot");
   ok(!services_register(25, 35, test_aun_handler), "overlapping range refused");
   ok(!services_register(10, 5, test_aun_handler), "inverted range refused");
   ok(services_register(50, 59, test_aun_handler), "third range registers");
   ok(services_register(60, 69, test_aun_handler), "fourth range registers");
   ok(services_register(70, 79, test_aun_handler), "fifth range registers");
   ok(services_register(80, 89, test_aun_handler), "sixth range registers");
   ok(services_register(90, 99, test_aun_handler), "seventh range registers");
   ok(services_register(100, 109, test_aun_handler), "eighth range registers");
   ok(!services_register(110, 119, test_aun_handler), "table full refused");

   puts("== dispatch ==");
   ok(do_simple(0, 20) == 42, "FAT command 20 reaches disk_type");
   {
      uint32_t cp = cp_of(CMD_PAGE);
      Pi1MHz->JIM_ram[cp] = 31;
      (void)dispatch(CMD_PAGE);
      ok(aun_calls == 1 && aun_last_cmd == 31, "command 31 routed to the AUN handler");
      Pi1MHz->JIM_ram[cp] = 45;
      uint8_t echo = dispatch(CMD_PAGE);
      ok(echo == CMD_PAGE, "unclaimed command 45 ignored (echo only)");
   }

   puts("== open-file interlock ==");
   /* raw-sector flag may have been set by earlier dispatch tests; reset
      via unmount (command 15), which clears all tracking. */
   (void)do_simple(0, 15);
   ok(!fat_service_file_in_use("/BEEB.MMB"), "nothing busy after unmount");

   ok(do_open(3, "/discs/elite.ssd") == FR_OK, "absolute open succeeds");
   ok(fat_service_file_in_use("/discs/elite.ssd"), "open file is busy");
   ok(fat_service_file_in_use("/DISCS/ELITE.SSD"), "match is case-insensitive");
   ok(fat_service_file_in_use("discs/elite.ssd"), "leading slash optional");
   ok(fat_service_file_in_use("/discs"), "containing directory is busy");
   ok(fat_service_file_in_use("/"), "root contains the open file");
   ok(!fat_service_file_in_use("/discs/exile.ssd"), "sibling file is free");
   ok(!fat_service_file_in_use("/disc"), "directory prefix must be whole component");

   (void)do_simple(3, 3);           /* close handle 3 */
   ok(!fat_service_file_in_use("/discs/elite.ssd"), "close releases the file");

   ok(do_open(4, "/a.dat") == FR_OK && do_open(4, "/b.dat") == FR_OK,
      "re-open on the same handle succeeds");
   ok(!fat_service_file_in_use("/a.dat") && fat_service_file_in_use("/b.dat"),
      "re-open replaces the old record");

   open_result = FR_NO_FILE;
   ok(do_open(5, "/missing.dat") == FR_NO_FILE, "failed open reports its error");
   ok(!fat_service_file_in_use("/missing.dat"), "failed open records nothing");
   open_result = FR_OK;

   puts("== relative paths and cwd ==");
   strcpy(cwd_value, "/subdir");
   ok(do_chdir("subdir") == FR_OK, "chdir succeeds");
   ok(do_open(6, "game.ssd") == FR_OK, "relative open succeeds");
   ok(fat_service_file_in_use("/subdir/game.ssd"), "relative open joined to cwd");
   getcwd_result = FR_DISK_ERR;
   (void)do_chdir("elsewhere");
   ok(do_open(7, "other.ssd") == FR_OK, "open under unknown cwd succeeds");
   ok(!fat_service_file_in_use("/elsewhere/other.ssd")
      && !fat_service_file_in_use("other.ssd"),
      "unknown cwd fails open (unrecorded), never mismatched");
   getcwd_result = FR_OK;

   puts("== overlong path fails open ==");
   {
      char longname[300];
      memset(longname, 'x', sizeof longname); longname[0] = '/';
      longname[sizeof longname - 1] = 0;
      ok(do_open(8, longname) == FR_OK, "overlong open succeeds");
      ok(!fat_service_file_in_use(longname), "overlong path is not recorded");
   }

   puts("== raw sector access marks BEEB.MMB ==");
   ok(!fat_service_file_in_use("/BEEB.MMB"), "BEEB.MMB free before raw access");
   (void)do_simple(0, 0);           /* disk_read: raw access seen */
   ok(fat_service_file_in_use("/BEEB.MMB"), "BEEB.MMB busy after raw read");
   ok(fat_service_file_in_use("/beeb.mmb"), "case-insensitive");
   ok(fat_service_file_in_use("/mmfs/BEEB.MMB"), "any directory's BEEB.MMB");
   ok(fat_service_file_in_use("/"), "root busy (it holds BEEB.MMB)");
   ok(!fat_service_file_in_use("/OTHER.MMB"), "other names unaffected");
   (void)do_simple(0, 15);          /* unmount clears everything */
   ok(!fat_service_file_in_use("/BEEB.MMB"), "unmount clears the raw flag");

   puts("== Beeb reset (re-init) releases stale locks ==");
   /* A BBC RST re-runs init_emulator(), which re-runs services_emulator_init
      -> fat_service_init(): the Beeb has abandoned whatever it held open, so
      the interlock must not report ghosts of the pre-reset session forever. */
   ok(do_open(2, "/discs/held.ssd") == FR_OK, "open a file before reset");
   (void)do_simple(0, 0);           /* raw sector access: BEEB.MMB latched */
   ok(fat_service_file_in_use("/discs/held.ssd")
      && fat_service_file_in_use("/BEEB.MMB"),
      "both locks held before reset");
   services_emulator_init(0, SVC_BASE);   /* the reset */
   ok(!fat_service_file_in_use("/discs/held.ssd"), "reset releases the open-file lock");
   ok(!fat_service_file_in_use("/BEEB.MMB"), "reset releases the raw-sector latch");
   ok(do_simple(0, 20) == 42, "FAT service still dispatches after reset");

   /* KEEP THIS BLOCK LAST: it sets Beeb_write_protect in the shared config
      store and there is no config_reset(), so anything appended after it would
      silently inherit write-protect ON. */
   puts("== Beeb_write_protect ==");
   {
      /* Baseline (key absent -> accessor false): Beeb writes reach FatFs. */
      ok(!config_beeb_write_protected(), "accessor false when the key is absent");
      f_write_calls = disk_write_calls = f_mkdir_calls = f_unlink_calls = f_rename_calls = 0;
      ok(do_simple(0, 1)  == RES_OK && disk_write_calls == 1, "off: disk_write happens");
      ok(do_simple(0, 5)  == FR_OK && f_write_calls   == 1,   "off: f_write happens");
      ok(do_simple(0, 10) == FR_OK && f_mkdir_calls   == 1,   "off: f_mkdir happens");
      ok(do_simple(0, 12) == FR_OK && f_rename_calls  == 1,   "off: f_rename happens");
      ok(do_simple(0, 16) == FR_OK && f_unlink_calls  == 1,   "off: f_unlink happens");
      ok(do_open(9, "/rw.dat") == FR_OK && last_open_mode == (FA_READ | FA_WRITE),
         "off: open keeps the requested read/write mode");

      /* Turn write-protect on: every Beeb write is now a silent success. */
      static char wp[] = "Beeb_write_protect=1\n";
      config_parse(wp, sizeof wp - 1);
      ok(config_beeb_write_protected(), "accessor true once the key is set");

      f_write_calls = disk_write_calls = f_mkdir_calls = f_unlink_calls = f_rename_calls = 0;
      ok(do_simple(0, 1)  == RES_OK && disk_write_calls == 0, "on: disk_write ignored, reports OK");
      ok(do_simple(0, 5)  == FR_OK && f_write_calls   == 0,   "on: f_write ignored, reports OK");
      ok(do_simple(0, 10) == FR_OK && f_mkdir_calls   == 0,   "on: f_mkdir ignored, reports OK");
      ok(do_simple(0, 12) == FR_OK && f_rename_calls  == 0,   "on: f_rename ignored, reports OK");
      ok(do_simple(0, 16) == FR_OK && f_unlink_calls  == 0,   "on: f_unlink ignored, reports OK");
      ok(do_open(9, "/rw.dat") == FR_OK && last_open_mode == FA_READ,
         "on: open downgraded to read-only (write/create bits stripped)");
      ok(do_simple(0, 20) == 42, "on: reads/queries unaffected (disk_type)");

      /* an f_write with a real length must report the FULL length "written"
         (the caller believes it succeeded) while touching no FatFs. */
      {
         uint32_t cp = cp_of(0xF0u);
         memset(&Pi1MHz->JIM_ram[cp], 0, 64);
         Pi1MHz->JIM_ram[cp]     = 5;      /* f_write */
         Pi1MHz->JIM_ram[cp + 1] = 0x40;   /* buf_len = 0x40 (24-bit at cp[1..3]) */
         uint8_t r = dispatch(0xF0u);
         uint32_t echoed = Pi1MHz->JIM_ram[cp + 1]
                         | ((uint32_t)Pi1MHz->JIM_ram[cp + 2] << 8)
                         | ((uint32_t)Pi1MHz->JIM_ram[cp + 3] << 16);
         ok(r == FR_OK && f_write_calls == 0 && echoed == 0x40u,
            "on: f_write echoes the full length without writing");
      }
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
