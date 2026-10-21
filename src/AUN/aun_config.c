/* aun_config.c - parsers for the aun Pi1MHz.cfg options.
 * See aun_config.h for the option syntax. Pure C, host-testable. */

#include <stddef.h>

#include "aun_config.h"
#include "../lib/textparse.h"

/* Parse an unsigned decimal number, advancing *s. Returns false if no
 * digits are present or the value exceeds 'max'. */
static bool parse_num(const char **s, uint32_t max, uint32_t *out)
{
   const char *p = *s;
   uint32_t v = 0;
   bool any = false;

   while (*p >= '0' && *p <= '9') {
      v = v * 10u + (uint32_t)(*p - '0');
      if (v > max)
         return false;
      p++;
      any = true;
   }
   if (!any)
      return false;
   *s  = p;
   *out = v;
   return true;
}

bool aun_parse_station(const char *s, uint8_t *net, uint8_t *stn)
{
   uint32_t a, b;

   if (s == NULL || !parse_num(&s, 254, &a))
      return false;

   if (*s == '.') {
      s++;
      if (!parse_num(&s, 254, &b))
         return false;
   } else {
      b = a;          /* bare "stn": net defaults to 0 */
      a = 0;
   }
   if (*s != '\0' || b == 0)
      return false;   /* trailing junk, or station 0 */

   *net = (uint8_t)a;
   *stn = (uint8_t)b;
   return true;
}

bool aun_station_is_ip(const char *s, uint8_t *net, bool *net_from_ip)
{
   uint32_t n = 0;
   bool from_ip = false;
   const char *dot = NULL;
   const char *tail;

   if (s == NULL)
      return false;

   for (const char *p = s; *p != '\0'; p++)
      if (*p == '.') { dot = p; break; }

   if (dot != NULL) {
      if (dot - s == 2 &&
          (s[0] == 'i' || s[0] == 'I') && (s[1] == 'p' || s[1] == 'P')) {
         from_ip = true;
      } else {
         const char *p = s;
         if (!parse_num(&p, 254, &n) || p != dot)
            return false;
      }
      tail = dot + 1;
   } else {
      tail = s;
   }

   if ((tail[0] == 'i' || tail[0] == 'I') &&
       (tail[1] == 'p' || tail[1] == 'P') && tail[2] == '\0') {
      *net = (uint8_t)n;
      *net_from_ip = from_ip;
      return true;
   }
   return false;
}

bool aun_parse_net(const char *s, uint8_t *net)
{
   uint32_t v;

   if (s == NULL || !parse_num(&s, 254, &v) || *s != '\0')
      return false;
   *net = (uint8_t)v;
   return true;
}

bool aun_parse_machine(const char *s, uint8_t id[4])
{
   if (s == NULL)
      return false;
   for (int i = 0; i < 4; i++) {
      int hi = text_hex_nibble(*s++);
      if (hi < 0)
         return false;
      int lo = text_hex_nibble(*s++);
      if (lo < 0)
         return false;
      id[i] = (uint8_t)((hi << 4) | lo);
   }
   return *s == '\0';
}

/* Parse "a.b.c.d" into a network-byte-order u32, advancing *s. */
static bool parse_ip(const char **s, uint32_t *ip_be)
{
   uint8_t oct[4];
   const char *end = text_parse_ipv4(*s, oct);

   if (end == NULL)
      return false;
   *ip_be = (uint32_t)oct[0] | (uint32_t)oct[1] << 8 |        /* first octet in the low byte */
            (uint32_t)oct[2] << 16 | (uint32_t)oct[3] << 24;
   *s = end;
   return true;
}

int aun_parse_map(const char *s, aun_map_add_fn add, void *user)
{
   int count = 0;

   if (s == NULL)
      return 0;

   while (*s != '\0') {
      uint32_t net, stn, ip_be;
      uint16_t port = 0;

      if (!parse_num(&s, 254, &net) || *s != '.')
         return -1;
      s++;
      if (!parse_num(&s, 254, &stn) || stn == 0 || *s != '=')
         return -1;
      s++;
      if (!parse_ip(&s, &ip_be))
         return -1;
      if (*s == ':') {
         size_t len = 0;
         s++;
         while (s[len] >= '0' && s[len] <= '9')
            len++;
         if (!text_parse_port(s, len, &port))
            return -1;
         s += len;
      }
      if (!add(user, (uint8_t)net, (uint8_t)stn, ip_be, port))
         return -1;
      count++;

      if (*s == ',') {
         s++;
         if (*s == '\0')
            return -1;                /* trailing comma */
      } else if (*s != '\0') {
         return -1;
      }
   }
   return count;
}
