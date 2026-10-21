/* textparse.c - see textparse.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "textparse.h"

bool text_parse_port(const char *s, size_t len, uint16_t *port)
{
   uint32_t v = 0u;

   if (s == NULL || len == 0u)
      return false;
   for (size_t i = 0u; i < len; i++) {
      if (s[i] < '0' || s[i] > '9')
         return false;
      v = v * 10u + (uint32_t)(s[i] - '0');
      if (v > 65535u)
         return false;
   }
   if (v == 0u)
      return false;
   *port = (uint16_t)v;
   return true;
}

bool text_parse_port_str(const char *s, uint16_t *port)
{
   size_t len = 0u;

   if (s == NULL)
      return false;
   while (s[len] != '\0')
      len++;
   return text_parse_port(s, len, port);
}

const char *text_parse_ipv4(const char *s, uint8_t oct[4])
{
   if (s == NULL)
      return NULL;
   for (unsigned int n = 0u; n < 4u; n++) {
      unsigned int v = 0u, digits = 0u;

      if (n != 0u) {
         if (*s != '.')
            return NULL;
         s++;
      }
      while (*s >= '0' && *s <= '9') {
         v = v * 10u + (unsigned int)(*s - '0');
         if (v > 255u || ++digits > 3u)
            return NULL;
         s++;
      }
      if (digits == 0u)
         return NULL;
      oct[n] = (uint8_t)v;
   }
   return s;
}

bool text_parse_ipv4_str(const char *s, uint8_t oct[4])
{
   uint8_t     tmp[4];
   const char *end = text_parse_ipv4(s, oct != NULL ? oct : tmp);

   return end != NULL && *end == '\0';
}
