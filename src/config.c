/* config.c - Pi1MHz.cfg key/value configuration store. See config.h. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "config.h"
#include "rpi/rpi.h"               /* LOG_DEBUG          */
#include "BeebSCSI/filesystem.h"   /* filesystemReadFile */

#define CONFIG_MAX_KEYS 96

typedef struct {
   const char *key;
   const char *value;
} config_entry_t;

static config_entry_t config_entries[CONFIG_MAX_KEYS];
static int            config_count;
static char          *config_buf;     /* retained NUL-terminated image */
static bool           config_loaded;

static char lower(char c)
{
   return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ci_equal(const char *a, const char *b)
{
   while (*a != '\0' && *b != '\0') {
      if (lower(*a) != lower(*b))
         return false;
      a++;
      b++;
   }
   return *a == '\0' && *b == '\0';
}

void config_parse(char *buf, size_t len)
{
   size_t i = 0;
   while (i < len) {
      size_t ls = i;
      while (i < len && buf[i] != '\n' && buf[i] != '\r')
         i++;
      size_t le = i;
      while (i < len && (buf[i] == '\n' || buf[i] == '\r'))
         i++;

      if (le == ls)
         continue;                              /* blank line            */
      if (buf[ls] == '#')
         continue;                              /* comment line          */
      if (buf[ls] == ' ' || buf[ls] == '\t')
         continue;                              /* key must be at column 0 */

      /* key: up to whitespace or '=' */
      size_t p = ls;
      while (p < le && buf[p] != ' ' && buf[p] != '\t' && buf[p] != '=')
         p++;
      size_t kend = p;

      /* optional whitespace then optional '=' then optional whitespace */
      while (p < le && (buf[p] == ' ' || buf[p] == '\t'))
         p++;
      if (p < le && buf[p] == '=') {
         p++;
         while (p < le && (buf[p] == ' ' || buf[p] == '\t'))
            p++;
      }

      /* value: rest of line, stopping at a trailing '#' comment */
      size_t vstart = p;
      size_t vend = le;
      for (size_t q = vstart; q < le; q++) {
         if (buf[q] == '#') {
            vend = q;
            break;
         }
      }
      while (vend > vstart && (buf[vend - 1] == ' ' || buf[vend - 1] == '\t'))
         vend--;

      buf[kend] = '\0';
      buf[vend] = '\0';

      if (kend > ls && config_count < CONFIG_MAX_KEYS) {
         config_entries[config_count].key   = &buf[ls];
         config_entries[config_count].value = &buf[vstart];
         config_count++;
      }
   }
}

const char *config_get(const char *key)
{
   for (int k = 0; k < config_count; k++) {
      if (ci_equal(config_entries[k].key, key))
         return config_entries[k].value;
   }
   return NULL;
}

int config_emulator_override(const char *name, uint8_t *addr)
{
   char key[128];
   size_t n = strlen(name);
   if (n > sizeof(key) - 6u)        /* leave room for "_addr" + NUL */
      n = sizeof(key) - 6u;
   memcpy(key, name, n);
   memcpy(key + n, "_addr", 6u);    /* 5 chars + the NUL terminator */

   const char *v = config_get(key);
   if (v == NULL)
      return 0;
   long t = strtol(v, NULL, 0);
   if (t < 0)
      return -1;
   *addr = (uint8_t)t;
   return 1;
}

void config_load(const char *filename)
{
   if (config_loaded)
      return;
   config_loaded = true;

   uint8_t *raw = NULL;
   uint32_t n = filesystemReadFile(filename, &raw, 0);
   if (n == 0 || raw == NULL) {
      free(raw);
      return;
   }

   config_buf = malloc((size_t)n + 1u);
   if (config_buf == NULL) {
      free(raw);
      return;
   }
   memcpy(config_buf, raw, (size_t)n);
   config_buf[n] = '\0';
   free(raw);

   config_parse(config_buf, (size_t)n);
   LOG_DEBUG("CONFIG: %s parsed, %d key(s)\r\n", filename, config_count);
}
