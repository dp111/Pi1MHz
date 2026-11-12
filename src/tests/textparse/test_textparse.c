/* Host tests for src/lib/textparse.h. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

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

static int u32_is(const char *s, uint32_t max, uint32_t want)
{
   uint32_t v = 777u;
   return text_parse_u32_str(s, max, &v) && v == want;
}

static int u32_bad(const char *s, uint32_t max)
{
   uint32_t v = 777u;
   return !text_parse_u32_str(s, max, &v) && v == 777u;
}

static int s32_is(const char *s, int32_t lo, int32_t hi, int32_t want)
{
   int32_t v = 777;
   return text_parse_s32_str(s, lo, hi, &v) && v == want;
}

static int s32_bad(const char *s, int32_t lo, int32_t hi)
{
   int32_t v = 777;
   return !text_parse_s32_str(s, lo, hi, &v) && v == 777;
}

static void test_numbers(void)
{
   ok(u32_is("0", 10u, 0u), "u32 0");
   ok(u32_is("42", 100u, 42u), "u32 decimal");
   ok(u32_is("010", 100u, 10u), "u32: a leading 0 is decimal, not octal");
   ok(u32_is("0x1F", 100u, 31u), "u32 0x hex");
   ok(u32_is("0Xff", 255u, 255u), "u32 0X hex, either case");
   ok(u32_is("&FC", 255u, 0xFCu), "u32 & hex, the BBC's way");
   ok(u32_is("4294967295", UINT32_MAX, UINT32_MAX), "u32 max");
   ok(u32_is("0xFFFFFFFF", UINT32_MAX, UINT32_MAX), "u32 max in hex");
   ok(u32_bad("4294967296", UINT32_MAX), "u32 wraps: refused");
   ok(u32_bad("0x100000000", UINT32_MAX), "u32 hex wraps: refused");
   ok(u32_bad("256", 255u), "u32 over max");
   ok(u32_bad("", 9u), "u32 empty");
   ok(u32_bad("0x", 9u), "u32 0x alone");
   ok(u32_bad("&", 9u), "u32 & alone");
   ok(u32_bad("off", 9u), "u32 a word (atoi said 0)");
   ok(u32_bad("12abc", 100u), "u32 trailing text (atoi said 12)");
   ok(u32_bad(" 1", 9u), "u32 leading space");
   ok(u32_bad("1 ", 9u), "u32 trailing space");
   ok(u32_bad("-1", 9u), "u32 sign");
   ok(u32_bad("+1", 9u), "u32 plus");
   ok(u32_bad("12", 100u) == 0, "u32 control");
   ok(u32_bad("0x1G", 100u), "u32 bad hex digit");
   ok(u32_bad("1.5", 100u), "u32 decimal point");
   {
      uint32_t v = 0u;
      ok(text_parse_u32("123", 2u, 999u, &v) && v == 12u, "u32 takes only its span");
      ok(!text_parse_u32(NULL, 1u, 9u, &v), "u32 NULL");
      ok(!text_parse_u32_str(NULL, 9u, &v), "u32_str NULL");
   }

   ok(s32_is("-1", -1, 255, -1), "s32 -1");
   ok(s32_is("0x40", -1, 255, 0x40), "s32 hex");
   ok(s32_is("-&10", -100, 100, -16), "s32 negative hex");
   ok(s32_is("-2147483648", INT32_MIN, INT32_MAX, INT32_MIN), "s32 min");
   ok(s32_is("2147483647", INT32_MIN, INT32_MAX, INT32_MAX), "s32 max");
   ok(s32_bad("2147483648", INT32_MIN, INT32_MAX), "s32 over int32");
   ok(s32_bad("-2147483649", INT32_MIN, INT32_MAX), "s32 under int32");
   ok(s32_bad("-2", -1, 255), "s32 under min");
   ok(s32_bad("256", -1, 255), "s32 over max");
   ok(s32_bad("-", -1, 255), "s32 sign alone");
   ok(s32_bad("--1", -9, 9), "s32 two signs");
   ok(s32_bad("off", -1, 255), "s32 a word");
   ok(s32_bad("1", 5, 1), "s32 min > max");
}

static void test_query(void)
{
   size_t      n = 99u;
   const char *v;
   char        out[8];

   v = text_query_find("a=1&offset=42&b=", "offset", &n);
   ok(v != NULL && n == 2u && strncmp(v, "42", 2) == 0, "query: a middle key");
   v = text_query_find("OFFSET=7", "offset", &n);
   ok(v != NULL && n == 1u && *v == '7', "query: keys match in any case");
   ok(text_query_find("xoffset=1", "offset", &n) == NULL, "query: only at a parameter's start");
   ok(text_query_find("offset", "offset", &n) == NULL, "query: no '=' is no value");
   v = text_query_find("a=1&b=", "b", &n);
   ok(v != NULL && n == 0u, "query: an empty value");
   ok(text_query_find("", "a", &n) == NULL, "query: empty string");
   ok(text_query_find(NULL, "a", &n) == NULL, "query: NULL");
   ok(text_query_find("a=1&&b=2", "b", &n) != NULL, "query: an empty parameter between");
   ok(text_query_param("k=abcdefghij", "k", out, sizeof out) && strcmp(out, "abcdefg") == 0,
      "query_param: cut to fit, as the web server always did");
   ok(!text_query_param("k=1", "j", out, sizeof out), "query_param: absent");
   ok(!text_query_param("k=1", "k", out, 0u), "query_param: no room at all");
}

static void test_url(void)
{
   char d[16];
   ok(text_url_decode("%41%62c", d, sizeof d) && strcmp(d, "Abc") == 0, "url: escapes");
   ok(text_url_decode("a%3Ab%2cc", d, sizeof d) && strcmp(d, "a:b,c") == 0, "url: : and ,");
   ok(text_url_decode("%0d%0a", d, sizeof d) && strcmp(d, "__") == 0, "url: CR LF become _");
   ok(text_url_decode("100%", d, sizeof d) && strcmp(d, "100%") == 0, "url: a bare % stays");
   ok(text_url_decode("%zz", d, sizeof d) && strcmp(d, "%zz") == 0, "url: a bad escape stays");
   ok(!text_url_decode("0123456789abcdefXYZ", d, sizeof d) && strlen(d) == 15u, "url: too long is false, still terminated");
}

int main(void)
{
   test_port();
   test_ipv4();
   test_hex();
   test_numbers();
   test_query();
   test_url();
   if (failures != 0) {
      printf("textparse: %d failure(s)\n", failures);
      return 1;
   }
   printf("textparse: all passed\n");
   return 0;
}
