/* Host tests for src/lib/textparse.h. */
#include <stdio.h>
#include <string.h>

#include "lib/textparse.h"

static int failures;

static void ok(int cond, const char *what)
{
   if (!cond) {
      printf("FAIL: %s\n", what);
      failures++;
   }
}

static int port_is(const char *s, unsigned want)
{
   uint16_t p = 12345u;
   return text_parse_port_str(s, &p) && p == want;
}

static int port_bad(const char *s)
{
   uint16_t p = 12345u;
   return !text_parse_port_str(s, &p) && p == 12345u;   /* untouched on failure */
}

static int ip_is(const char *s, unsigned a, unsigned b, unsigned c, unsigned d)
{
   uint8_t o[4];
   return text_parse_ipv4_str(s, o) && o[0] == a && o[1] == b && o[2] == c && o[3] == d;
}

static void test_port(void)
{
   uint16_t p = 0u;

   ok(port_is("1", 1u), "port 1");
   ok(port_is("23", 23u), "port 23");
   ok(port_is("65535", 65535u), "port 65535");
   ok(port_is("00080", 80u), "leading zeros are decimal");
   ok(port_bad("0"), "port 0 refused");
   ok(port_bad("65536"), "port 65536 refused");
   ok(port_bad("99999999999999999999"), "a long run does not wrap into range");
   ok(port_bad(""), "empty refused");
   ok(port_bad(NULL), "NULL refused");
   ok(port_bad("23abc"), "trailing junk refused");
   ok(port_bad(" 23"), "leading space refused");
   ok(port_bad("+23"), "sign refused");
   ok(port_bad("-1"), "negative refused");
   ok(text_parse_port("8080/x", 4u, &p) && p == 8080u, "a span inside a URL");
   ok(!text_parse_port("8080", 0u, &p), "empty span refused");
}

static void test_ipv4(void)
{
   uint8_t     o[4];
   const char *end;

   ok(ip_is("1.2.3.4", 1u, 2u, 3u, 4u), "dotted quad");
   ok(ip_is("255.255.255.255", 255u, 255u, 255u, 255u), "all 255");
   ok(ip_is("0.0.0.0", 0u, 0u, 0u, 0u), "all zero");
   ok(ip_is("192.168.001.010", 192u, 168u, 1u, 10u), "leading zeros are decimal, not octal");
   ok(!text_parse_ipv4_str("256.1.1.1", o), "octet 256 refused");
   ok(!text_parse_ipv4_str("0001.2.3.4", o), "four-digit octet refused");
   ok(!text_parse_ipv4_str("1.2.3", o), "three octets refused");
   ok(!text_parse_ipv4_str("1.2.3.4.5", o), "five octets refused");
   ok(!text_parse_ipv4_str("1..3.4", o), "empty octet refused");
   ok(!text_parse_ipv4_str("1.2.3.", o), "trailing dot refused");
   ok(!text_parse_ipv4_str(".1.2.3", o), "leading dot refused");
   ok(!text_parse_ipv4_str("10", o), "inet_aton short form refused");
   ok(!text_parse_ipv4_str("0x7f.0.0.1", o), "hex refused");
   ok(!text_parse_ipv4_str("1.2.3.4 ", o), "trailing space refused");
   ok(!text_parse_ipv4_str("", o), "empty refused");
   ok(!text_parse_ipv4_str(NULL, o), "NULL refused");
   ok(text_parse_ipv4_str("1.2.3.4", NULL), "octets not wanted");
   end = text_parse_ipv4("10.0.0.2:8080", o);
   ok(end != NULL && strcmp(end, ":8080") == 0 && o[3] == 2u, "stops at the port");
}

static void test_hex(void)
{
   int good = 1;

   for (int c = -1; c < 256; c++) {
      int want = (c >= '0' && c <= '9') ? c - '0'
               : (c >= 'a' && c <= 'f') ? c - 'a' + 10
               : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (text_hex_nibble(c) != want)
         good = 0;
   }
   ok(good, "hex nibble: every byte, EOF included");
}

int main(void)
{
   test_port();
   test_ipv4();
   test_hex();
   if (failures != 0) {
      printf("textparse: %d failure(s)\n", failures);
      return 1;
   }
   printf("textparse: all passed\n");
   return 0;
}
