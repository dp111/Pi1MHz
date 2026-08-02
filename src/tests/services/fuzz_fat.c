/* Fuzz the FAT service's Beeb-facing command dispatch: random command
 * blocks with hostile offsets, lengths and unterminated strings, under
 * ASan/UBSan.  The commands guard an arbitrary read/write primitive into
 * JIM RAM (see the untrusted-input note in fat_service.c), so the FatFs
 * stubs here deliberately TOUCH every byte of any buffer they are handed:
 * a bounds check that lets a bad offset/length through becomes an ASan
 * out-of-bounds fault instead of a silent pass. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Pi1MHz.h"
#include "services.h"
#include "BeebSCSI/fatfs/ff.h"
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"

static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;

#define SVC_BASE 0xA6u
static callback_func_ptr write_cb[256];
static callback_func_ptr read_cb[256];
void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr fn)
{ if (access == WRITE_FRED) write_cb[addr & 0xff] = fn; else read_cb[addr & 0xff] = fn; }
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data)  { pi.Memory[addr & 0x1ff] = data; }
void Pi1MHz_MemoryWrite16(uint32_t addr, uint32_t data)
{ pi.Memory[addr & 0x1ff] = (uint8_t)data; pi.Memory[(addr + 1u) & 0x1ff] = (uint8_t)(data >> 8); }

/* ---- FatFs stubs that touch what they are given ---- */
static void touch_read(const void *b, size_t n)   /* force ASan reads  */
{ volatile uint8_t sink = 0; const uint8_t *p = b; for (size_t i = 0; i < n; i++) sink ^= p[i]; (void)sink; }

FRESULT f_open(FIL *fp, const char *path, uint8_t mode)
{ (void)fp; (void)mode; touch_read(path, strlen(path) + 1); return (path[0] & 1) ? FR_NO_FILE : FR_OK; }
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_read(FIL *fp, void *b, UINT n, UINT *r)
{ (void)fp; memset(b, 0xAA, n); *r = (n > 3u) ? n - 3u : n; return FR_OK; }
FRESULT f_write(FIL *fp, const void *b, UINT n, UINT *w)
{ (void)fp; touch_read(b, n); *w = n; return FR_OK; }
FRESULT f_lseek(FIL *fp, uint32_t ofs) { (void)fp; return (ofs & 0x10000u) ? FR_DISK_ERR : FR_OK; }
FRESULT f_opendir(DIR *dp, const char *p) { (void)dp; touch_read(p, strlen(p) + 1); return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }
FRESULT f_readdir(DIR *dp, FILINFO *fno)
{ (void)dp; memset(fno->fname, 'x', 200); fno->fname[200] = 0; return FR_OK; }
FRESULT f_mkdir(const char *p) { touch_read(p, strlen(p) + 1); return FR_OK; }
FRESULT f_chdir(const char *p) { touch_read(p, strlen(p) + 1); return (p[0] & 1) ? FR_NO_PATH : FR_OK; }
FRESULT f_getcwd(char *buff, UINT len) { snprintf(buff, len, "/fuzzdir"); return FR_OK; }
FRESULT f_rename(const char *a, const char *b)
{ touch_read(a, strlen(a) + 1); touch_read(b, strlen(b) + 1); return FR_OK; }
FRESULT f_getfree(const char *p, DWORD *n, FATFS **f)
{ (void)p; static FATFS fs = { 8 }; *n = 1000; *f = &fs; return FR_OK; }
FRESULT f_unlink(const char *p) { touch_read(p, strlen(p) + 1); return FR_OK; }

DRESULT disk_read(uint8_t d, uint8_t *b, uint32_t s, unsigned int c)
{ (void)d; (void)s; memset(b, 0x55, (size_t)c * 512u); return RES_OK; }
DRESULT disk_write(uint8_t d, const uint8_t *b, uint32_t s, unsigned int c)
{ (void)d; (void)s; touch_read(b, (size_t)c * 512u); return RES_OK; }
unsigned char disk_type(void) { return 1; }

bool filesystemMount(void) { return true; }
bool filesystemDismount(void) { return true; }

/* xorshift, deterministic */
static uint32_t rng = 0x1234567u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(void)
{
   pi.JIM_ram = malloc(32u * 1024u * 1024u);
   pi.JIM_ram_size = 2;             /* DISC_RAM_BASE == 0 */
   assert(pi.JIM_ram != NULL);
   memset(pi.JIM_ram, 0xFF, 32u * 1024u * 1024u);   /* no free terminators */

   services_emulator_init(0, SVC_BASE);

   for (unsigned int iter = 0; iter < 300000u; iter++) {
      uint8_t page = (uint8_t)(0xF0u + (rnd() & 0x0Fu));
      uint32_t cp = DISC_RAM_BASE | 0xFF0000u | ((uint32_t)page << 8);

      /* Hostile command block: random bytes, biased to huge offsets and
         lengths, sometimes all-0xFF (no string terminator anywhere). */
      for (unsigned int k = 0; k < 40u; k++) {
         uint32_t r = rnd();
         pi.JIM_ram[cp + k] = (uint8_t)r;
         if ((r % 5u) == 0u) pi.JIM_ram[cp + k] = 0xFF;
         if ((r % 7u) == 0u) pi.JIM_ram[cp + k] = 0x00;
      }
      /* Command byte: mostly the FAT range, sometimes past it. */
      pi.JIM_ram[cp] = (uint8_t)((rnd() % 3u == 0u) ? (rnd() & 0xFFu)
                                                    : (rnd() % 30u));

      write_cb[SVC_BASE + 4](TEST_GPIO(SVC_BASE + 4, page));

      /* Exercise the address window and data port too. */
      if ((iter & 0xFu) == 0u) {
         write_cb[SVC_BASE + 0](TEST_GPIO(SVC_BASE + 0, rnd() & 0xFF));
         write_cb[SVC_BASE + 1](TEST_GPIO(SVC_BASE + 1, rnd() & 0xFF));
         write_cb[SVC_BASE + 2](TEST_GPIO(SVC_BASE + 2, rnd() & 0xFF));
         write_cb[SVC_BASE + 3](TEST_GPIO(SVC_BASE + 3, rnd() & 0xFF));
         read_cb[SVC_BASE + 3](TEST_GPIO(SVC_BASE + 3, 0));
      }

      /* And the interlock query with hostile paths. */
      if ((iter & 0x1Fu) == 0u) {
         char q[64];
         unsigned int len = rnd() % (sizeof q - 1u);
         for (unsigned int k = 0; k < len; k++)
            q[k] = (char)(rnd() % 96u + 32u);
         q[len] = 0;
         (void)fat_service_file_in_use(q);
      }
   }

   puts("fat fuzz: 300k hostile command blocks, no crashes");
   return 0;
}
