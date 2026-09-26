/* fn_json.h - JSON translation for the network device (see fn_json.c). */
#ifndef FN_JSON_H
#define FN_JSON_H

#include <stdbool.h>
#include <stdint.h>

/* Translate the NUL-terminated `body` through the JSON Pointer `selector`.
   *out is a malloc'd NUL-terminated text of *len bytes (NULL when empty);
   the caller frees it.  False only when memory ran out. */
bool fn_json_translate(const char *body, const char *selector, char **out, uint32_t *len);

#endif
