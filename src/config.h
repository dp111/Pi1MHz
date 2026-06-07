/* config.h - Pi1MHz.cfg key/value configuration store.
 *
 * Splits the ever-growing set of options out of the kernel cmdline.txt
 * into a dedicated Pi1MHz.cfg file on the SD card. Rules:
 *   - one key per line, the key starts at the beginning of the line
 *   - "key=value" or "key value"; the value is the rest of the line, so
 *     it may contain spaces
 *   - '#' starts a comment (whole-line or trailing)
 *   - blank lines are ignored
 *
 * The file is read and parsed once (config_load); thereafter lookups are
 * a simple table search - the file is never re-parsed. Each emulation
 * module keeps owning its own keys: it just reads them with the existing
 * get_cmdline_prop(), which now transparently falls back to this store
 * (a key in cmdline.txt still overrides the file).
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* Read <filename> from the SD card and build the key table. Safe to call
 * more than once; only the first call does any work. Missing file or read
 * error simply leaves the store empty. */
void config_load(const char *filename);

/* Look up a key (case-insensitive). Returns the value string (which may
 * be empty for a bare flag) or NULL if the key is not present. The
 * returned pointer is owned by the store and stays valid. */
const char *config_get(const char *key);

/* Resolve an emulator's "<name>_addr" override from the config:
 *    returns  1 and writes *addr if an address override is present,
 *            -1 if the emulator is disabled (a negative address),
 *             0 if there is no "<name>_addr" key (leaving *addr untouched). */
int config_emulator_override(const char *name, uint8_t *addr);

/* Parse an in-memory config image into the key table. The buffer is
 * modified in place (keys/values are NUL-terminated within it) and must
 * remain live for as long as the store is used; buf[len] must be a valid
 * writable byte. Exposed for host testing. */
void config_parse(char *buf, size_t len);

#endif
