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

int text_hex_nibble(int c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

bool text_parse_u32(const char *s, size_t len, uint32_t max, uint32_t *value)
{
   uint32_t v = 0u;
   bool     hex = false;
   size_t   i = 0u;

   if (s == NULL || len == 0u)
      return false;
   if (s[0] == '&') {
      hex = true;
      i = 1u;
   } else if (len >= 2u && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      hex = true;
      i = 2u;
   }
   if (i == len)
      return false;                       /* "0x" or "&" with no digits */
   for (; i < len; i++) {
      int      d = hex ? text_hex_nibble(s[i]) : ((s[i] >= '0' && s[i] <= '9') ? s[i] - '0' : -1);
      uint32_t base = hex ? 16u : 10u;
      if (d < 0)
         return false;
      if (v > (UINT32_MAX - (uint32_t)d) / base)
         return false;                    /* would wrap */
      v = v * base + (uint32_t)d;
   }
   if (v > max)
      return false;
   *value = v;
   return true;
}

static size_t text_len(const char *s)
{
   size_t n = 0u;
   while (s[n] != '\0')
      n++;
   return n;
}

bool text_parse_u32_str(const char *s, uint32_t max, uint32_t *value)
{
   return s != NULL && text_parse_u32(s, text_len(s), max, value);
}

bool text_parse_s32(const char *s, size_t len, int32_t min, int32_t max, int32_t *value)
{
   uint32_t mag;
   int64_t  v;

   if (s == NULL || len == 0u || min > max)
      return false;
   if (s[0] == '-') {
      if (!text_parse_u32(s + 1, len - 1u, UINT32_C(0x80000000), &mag))
         return false;
      v = -(int64_t)mag;
   } else {
      if (!text_parse_u32(s, len, UINT32_C(0x7FFFFFFF), &mag))
         return false;
      v = (int64_t)mag;
   }
   if (v < min || v > max)
      return false;
   *value = (int32_t)v;
   return true;
}

bool text_parse_s32_str(const char *s, int32_t min, int32_t max, int32_t *value)
{
   return s != NULL && text_parse_s32(s, text_len(s), min, max, value);
}

static int text_lower(int c)
{
   return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

const char *text_query_find(const char *query, const char *key, size_t *len)
{
   size_t klen;
   const char *p = query;

   if (query == NULL || key == NULL)
      return NULL;
   klen = text_len(key);
   while (*p != '\0') {
      size_t k = 0u;
      while (k < klen && p[k] != '\0' && text_lower(p[k]) == text_lower(key[k]))
         k++;
      if (k == klen && p[klen] == '=') {
         const char *v = p + klen + 1u;
         size_t      n = 0u;
         while (v[n] != '\0' && v[n] != '&')
            n++;
         *len = n;
         return v;
      }
      while (*p != '\0' && *p != '&')
         ++p;
      if (*p == '&')
         ++p;
   }
   return NULL;
}

bool text_query_param(const char *query, const char *key, char *out, size_t osz)
{
   size_t      n = 0u;
   const char *v = text_query_find(query, key, &n);

   if (v == NULL || osz == 0u)
      return false;
   if (n > osz - 1u)
      n = osz - 1u;
   for (size_t i = 0u; i < n; i++)
      out[i] = v[i];
   out[n] = '\0';
   return true;
}

bool text_url_decode(const char *src, char *dst, size_t dstsz)
{
   size_t o = 0u;
   while (*src != '\0' && o + 1u < dstsz) {
      unsigned char c;
      if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
         int hi = text_hex_nibble(src[1]);
         int lo = text_hex_nibble(src[2]);
         if (hi >= 0 && lo >= 0) {
            c = (unsigned char)((hi << 4) | lo);
            dst[o++] = (c < 0x20u || c == 0x7Fu) ? '_' : (char)c;
            src += 3;
            continue;
         }
      }
      c = (unsigned char)*src++;
      dst[o++] = (c < 0x20u || c == 0x7Fu) ? '_' : (char)c;
   }
   dst[o] = '\0';
   return *src == '\0';
}
