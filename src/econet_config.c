/* econet_config.c - parsers for the econet cmdline.txt options.
 * See econet_config.h for the option syntax. Pure C, host-testable. */

#include <stddef.h>

#include "econet_config.h"

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

bool eco_parse_station(const char *s, uint8_t *net, uint8_t *stn)
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

bool eco_parse_net(const char *s, uint8_t *net)
{
   uint32_t v;

   if (s == NULL || !parse_num(&s, 254, &v) || *s != '\0')
      return false;
   *net = (uint8_t)v;
   return true;
}

bool eco_parse_machine(const char *s, uint8_t id[4])
{
   if (s == NULL)
      return false;
   for (int i = 0; i < 4; i++) {
      uint32_t v = 0;
      for (int n = 0; n < 2; n++) {
         char ch = *s++;
         if (ch >= '0' && ch <= '9') v = (v << 4) | (uint32_t)(ch - '0');
         else if (ch >= 'a' && ch <= 'f') v = (v << 4) | (uint32_t)(ch - 'a' + 10);
         else if (ch >= 'A' && ch <= 'F') v = (v << 4) | (uint32_t)(ch - 'A' + 10);
         else return false;
      }
      id[i] = (uint8_t)v;
   }
   return *s == '\0';
}

bool eco_parse_port(const char *s, uint16_t *port)
{
   uint32_t v;

   if (s == NULL || !parse_num(&s, 65535, &v) || *s != '\0' || v == 0)
      return false;
   *port = (uint16_t)v;
   return true;
}

/* Parse "a.b.c.d" into a network-byte-order u32, advancing *s. */
static bool parse_ip(const char **s, uint32_t *ip_be)
{
   uint32_t v = 0;

   for (int i = 0; i < 4; i++) {
      uint32_t octet;
      if (!parse_num(s, 255, &octet))
         return false;
      v |= octet << (8 * i);          /* first octet in the low byte */
      if (i < 3) {
         if (**s != '.')
            return false;
         (*s)++;
      }
   }
   *ip_be = v;
   return true;
}

int eco_parse_map(const char *s, eco_map_add_fn add, void *user)
{
   int count = 0;

   if (s == NULL)
      return 0;

   while (*s != '\0') {
      uint32_t net, stn, ip_be, port = 0;

      if (!parse_num(&s, 254, &net) || *s != '.')
         return -1;
      s++;
      if (!parse_num(&s, 254, &stn) || stn == 0 || *s != '=')
         return -1;
      s++;
      if (!parse_ip(&s, &ip_be))
         return -1;
      if (*s == ':') {
         s++;
         if (!parse_num(&s, 65535, &port) || port == 0)
            return -1;
      }
      if (!add(user, (uint8_t)net, (uint8_t)stn, ip_be, (uint16_t)port))
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
