/* Host tests for wifi_time.c: the calendar conversion the WiFi ROM's *DATE
 * uses, and the ntp_offset shift. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "wifi_time.h"

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
   printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
   else { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The calendar, walked a day at a time: the reference. */
static bool ref_leap(uint32_t y)
{ return (y % 4u == 0u && y % 100u != 0u) || y % 400u == 0u; }
static uint32_t ref_month_length(uint32_t y, uint32_t m)
{
   static const uint8_t md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
   return md[m - 1u] + ((m == 2u && ref_leap(y)) ? 1u : 0u);
}

/* Local time: 2026-10-09 12:34:56. */
static int64_t at(uint32_t y, uint32_t m, uint32_t d, uint32_t hh, uint32_t mm, uint32_t ss)
{ return wifi_time_days_from_civil(y, m, d) * 86400 + (int64_t)(hh * 3600u + mm * 60u + ss); }

/* Apply a config value to `now` the way wifi_service.c does. */
static bool shifted(const char *text, int64_t now, int64_t *out)
{
   wifi_time_shift_t s;
   if (!wifi_time_shift_parse(text, &s)) return false;
   *out = now + wifi_time_shift_seconds(&s, now);
   return true;
}
static bool is(const char *text, uint32_t y, uint32_t m, uint32_t d, uint32_t hh, uint32_t mm, uint32_t ss)
{
   int64_t out;
   return shifted(text, at(2026, 10, 9, 12, 34, 56), &out) && out == at(y, m, d, hh, mm, ss);
}
static bool rejected(const char *text)
{
   wifi_time_shift_t s;
   bool ok = wifi_time_shift_parse(text, &s);
   return !ok && s.kind == WIFI_TIME_SHIFT_NONE;
}

int main(void)
{
   printf("== calendar: every day from 0001-01-01 to 9999-12-31, walked ==\n");
   {
      uint32_t y = 1u, m = 1u, d = 1u, bad = 0u, round = 0u;
      int64_t day, total = 0;
      for (day = WIFI_TIME_FIRST_DAY; day <= WIFI_TIME_LAST_DAY; day++, total++) {
         uint32_t cy, cm, cd;
         wifi_time_civil(day, &cy, &cm, &cd);
         if (cy != y || cm != m || cd != d) bad++;
         if (wifi_time_days_from_civil(y, m, d) != day) round++;
         if (++d > ref_month_length(y, m)) {
            d = 1u;
            if (++m > 12u) { m = 1u; y++; }
         }
      }
      CHECK(bad == 0u, "%lld days checked, %u differ from a calendar walked day by day", (long long)total, bad);
      CHECK(round == 0u, "days -> date -> days round-trips for all of them (%u differ)", round);
      CHECK(y == 10000u && m == 1u && d == 1u,
            "the walk from day -719162 ends on 9999-12-31 exactly, so both ends are right");
   }
   {
      uint32_t y, m, d;
      wifi_time_civil(0, &y, &m, &d);
      CHECK(y == 1970u && m == 1u && d == 1u, "day 0 is 1970-01-01");
      CHECK(wifi_time_days_from_civil(1u, 1u, 1u) == WIFI_TIME_FIRST_DAY
            && wifi_time_days_from_civil(9999u, 12u, 31u) == WIFI_TIME_LAST_DAY, "the end constants");
      wifi_time_civil(wifi_time_days_from_civil(2000u, 2u, 29u), &y, &m, &d);
      CHECK(y == 2000u && m == 2u && d == 29u, "2000 is a leap year");
      wifi_time_civil(wifi_time_days_from_civil(2100u, 3u, 1u) - 1, &y, &m, &d);
      CHECK(y == 2100u && m == 2u && d == 28u, "2100 is not");
      wifi_time_civil(wifi_time_days_from_civil(1900u, 3u, 1u) - 1, &y, &m, &d);
      CHECK(y == 1900u && m == 2u && d == 28u, "nor is 1900, before the epoch");
      wifi_time_civil(4294967295LL / 86400, &y, &m, &d);
      CHECK(y == 2106u && m == 2u && d == 7u, "the last day the 32-bit network count reaches: 2106-02-07");
   }

   printf("== split seconds into the fields shown ==\n");
   {
      wifi_time_fields_t f;
      wifi_time_split(0, &f);
      CHECK(f.year == 1970u && f.month == 1u && f.day == 1u && f.hour == 0u && f.minute == 0u && f.second == 0u,
            "0 is 1970-01-01 00:00:00");
      wifi_time_split(-1, &f);
      CHECK(f.year == 1969u && f.month == 12u && f.day == 31u && f.hour == 23u && f.minute == 59u && f.second == 59u,
            "-1 is the last second of 1969");
      wifi_time_split(at(1900, 3, 1, 6, 7, 8), &f);
      CHECK(f.year == 1900u && f.month == 3u && f.day == 1u && f.hour == 6u && f.minute == 7u && f.second == 8u,
            "a time in 1900 (the 29th of February never happened)");
      wifi_time_split(at(1, 1, 1, 0, 0, 0), &f);
      CHECK(f.year == 1u && f.month == 1u && f.day == 1u && f.hour == 0u, "the first second of year 1");
      wifi_time_split(at(1, 1, 1, 0, 0, 0) - 1, &f);
      CHECK(f.year == 1u && f.month == 1u && f.day == 1u && f.hour == 0u && f.second == 0u,
            "before it, the clock stops at the start of year 1");
      wifi_time_split(at(9999, 12, 31, 23, 59, 59), &f);
      CHECK(f.year == 9999u && f.month == 12u && f.day == 31u && f.hour == 23u && f.second == 59u,
            "the last second of year 9999");
      wifi_time_split(at(9999, 12, 31, 23, 59, 59) + 1, &f);
      CHECK(f.year == 9999u && f.month == 12u && f.day == 31u && f.hour == 23u && f.minute == 59u && f.second == 59u,
            "past it, the clock stops there");
   }

   printf("== time shift: parsing ==\n");
   {
      wifi_time_shift_t s;
      CHECK(wifi_time_shift_parse(NULL, &s) && s.kind == WIFI_TIME_SHIFT_NONE, "no key: no shift");
      CHECK(wifi_time_shift_parse("", &s) && s.kind == WIFI_TIME_SHIFT_NONE, "empty key: no shift");
      CHECK(wifi_time_shift_parse("s3600", &s) && s.kind == WIFI_TIME_SHIFT_SECONDS && s.seconds == 3600,
            "s3600 is seconds");
      CHECK(wifi_time_shift_parse("y2020", &s) && s.kind == WIFI_TIME_SHIFT_DATE && s.year == 2020u
            && s.month == 0u && s.day == 0u, "y2020 is a year");
      CHECK(wifi_time_shift_parse("y1", &s) && s.year == 1u && wifi_time_shift_parse("y9999", &s) && s.year == 9999u
            && wifi_time_shift_parse("y0001", &s) && s.year == 1u && wifi_time_shift_parse("y1969", &s) && s.year == 1969u,
            "any year from y1 to y9999");
      CHECK(wifi_time_shift_parse("m5", &s) && s.seconds == 300 && wifi_time_shift_parse("+m5", &s) && s.seconds == 300,
            "m5, with or without a +");
      CHECK(wifi_time_shift_parse("-s90", &s) && s.seconds == -90, "-s90");
      CHECK(wifi_time_shift_parse("-h2", &s) && s.seconds == -7200, "-h2");
      CHECK(wifi_time_shift_parse("d3", &s) && s.seconds == 3 * 86400, "d3");
      CHECK(wifi_time_shift_parse("d2020", &s) && s.kind == WIFI_TIME_SHIFT_SECONDS
            && s.seconds == 2020 * 86400, "d2020 is days, not a year");
      CHECK(wifi_time_shift_parse("31.12.1999", &s) && s.kind == WIFI_TIME_SHIFT_DATE
            && s.day == 31u && s.month == 12u && s.year == 1999u, "dd.mm.yyyy");
      CHECK(wifi_time_shift_parse("1.2.2000", &s) && s.day == 1u && s.month == 2u && s.year == 2000u,
            "d.m.yyyy without leading zeros");
      CHECK(wifi_time_shift_parse("04.2020", &s) && s.day == 0u && s.month == 4u && s.year == 2020u,
            "mm.yyyy");
      CHECK(rejected("3600") && rejected("2020") && rejected("1") && rejected("+2020") && rejected("-1"),
            "plain digits are refused: every number names its unit");
      CHECK(rejected("x1") && rejected("h") && rejected("+") && rejected("-") && rejected("h2h") && rejected("h2x")
            && rejected("h 2") && rejected("2h") && rejected("abc") && rejected("hh2"), "junk is refused and leaves no shift");
      CHECK(rejected("y0") && rejected("y10000") && rejected("y") && rejected("-y2020") && rejected("+y2020")
            && rejected("y20x"), "years out of range, unsigned only");
      CHECK(rejected("32.01.2020") && rejected("00.01.2020") && rejected("01.13.2020")
            && rejected("01.00.2020") && rejected("13.2020") && rejected("00.2020"),
            "day and month out of range are refused");
      CHECK(rejected("01.01.0000") && rejected("01.01.10000") && rejected("01.0000")
            && rejected("1.2.3.2020") && rejected(".2020") && rejected("01..2020")
            && rejected("01.2020.") && rejected("12.ab"), "year out of range and malformed dates are refused");
      CHECK(rejected("s2147483648") && rejected("s99999999999") && rejected("-h99999999999"),
            "numbers that do not fit in 31 bits are refused");
      CHECK(wifi_time_shift_parse("s2147483647", &s) && s.seconds == 2147483647, "the largest number of seconds is accepted");
      CHECK(wifi_time_shift_parse("d30000", &s) && s.seconds == 30000LL * 86400
            && wifi_time_shift_parse("-d2147483647", &s) && s.seconds == -2147483647LL * 86400,
            "days are not limited to what 32 bits of seconds holds");
   }

   printf("== time shift: applied to 2026-10-09 12:34:56 ==\n");
   {
      CHECK(is(NULL, 2026, 10, 9, 12, 34, 56), "no shift leaves the time alone");
      CHECK(is("31.12.1999", 1999, 12, 31, 12, 34, 56), "dd.mm.yyyy: that date, the time of day kept");
      CHECK(is("04.2020", 2020, 4, 9, 12, 34, 56), "mm.yyyy: the day of the month kept");
      CHECK(is("y2020", 2020, 10, 9, 12, 34, 56), "yYYYY: the month and day kept");
      CHECK(is("31.04.2021", 2021, 4, 30, 12, 34, 56), "31 April becomes 30 April");
      CHECK(is("29.02.2023", 2023, 2, 28, 12, 34, 56), "29 February in a common year becomes the 28th");
      CHECK(is("29.02.2024", 2024, 2, 29, 12, 34, 56), "29 February in a leap year stays");
      CHECK(is("02.2023", 2023, 2, 9, 12, 34, 56), "mm.yyyy with an ordinary day");
      CHECK(is("01.01.1970", 1970, 1, 1, 12, 34, 56), "the epoch day");
      CHECK(is("31.12.2105", 2105, 12, 31, 12, 34, 56), "a date past what the real clock reaches");
      CHECK(is("01.01.0001", 1, 1, 1, 12, 34, 56) && is("31.12.9999", 9999, 12, 31, 12, 34, 56),
            "the first and last day shown");
      CHECK(is("01.01.1900", 1900, 1, 1, 12, 34, 56) && is("y1600", 1600, 10, 9, 12, 34, 56),
            "years before the epoch");
      CHECK(is("29.02.1900", 1900, 2, 28, 12, 34, 56) && is("29.02.1600", 1600, 2, 29, 12, 34, 56),
            "1900 has no 29 February, 1600 has");
      CHECK(is("y1", 1, 10, 9, 12, 34, 56) && is("y9999", 9999, 10, 9, 12, 34, 56), "y1 and y9999");
      CHECK(is("+m90", 2026, 10, 9, 14, 4, 56), "+m90");
      CHECK(is("-h13", 2026, 10, 8, 23, 34, 56), "-h13 crosses midnight backwards");
      CHECK(is("d1", 2026, 10, 10, 12, 34, 56), "d1");
      CHECK(is("-d1000", 2024, 1, 13, 12, 34, 56), "-d1000");
      CHECK(is("s3600", 2026, 10, 9, 13, 34, 56), "s3600");
   }

   printf("== time shift: a date is fixed at its first use ==\n");
   {
      wifi_time_shift_t s;
      int64_t first = at(2026, 10, 9, 23, 59, 59), later = at(2026, 10, 10, 0, 0, 1);
      int64_t a, b;
      (void)wifi_time_shift_parse("01.01.2000", &s);
      a = wifi_time_shift_seconds(&s, first);
      b = wifi_time_shift_seconds(&s, later);
      CHECK(a == b, "the same shift is used afterwards (%lld, %lld)", (long long)a, (long long)b);
      CHECK(later + b == at(2000, 1, 1, 0, 0, 1) + 86400,
            "so the clock runs on from the chosen date: the next day is the 2nd");
      (void)wifi_time_shift_parse("01.01.2000", &s);
      b = wifi_time_shift_seconds(&s, later);
      CHECK(b != a, "a fresh shift (a reboot) is resolved again, from the new first use");
   }

   printf("== time zone: POSIX TZ strings ==" "\n");
   {
      /* UTC instants: transitions in 2026 */
      const int64_t mar_uk = at(2026, 3, 29, 1, 0, 0), oct_uk = at(2026, 10, 25, 1, 0, 0);
      wifi_time_zone_t z;

      CHECK(wifi_time_zone_parse("GMT0BST,M3.5.0/1,M10.5.0", &z) && z.has_dst
            && z.std_offset == 0 && z.dst_offset == 3600, "UK zone parses");
      CHECK(wifi_time_zone_offset(&z, mar_uk - 1u) == 0 && wifi_time_zone_offset(&z, mar_uk) == 3600,
            "UK: summer time starts at 01:00 UTC on the last Sunday of March");
      CHECK(wifi_time_zone_offset(&z, oct_uk - 1u) == 3600 && wifi_time_zone_offset(&z, oct_uk) == 0,
            "UK: and ends at 01:00 UTC on the last Sunday of October");
      CHECK(wifi_time_zone_offset(&z, at(2026, 1, 15, 12, 0, 0)) == 0
            && wifi_time_zone_offset(&z, at(2026, 7, 1, 12, 0, 0)) == 3600, "UK: winter and summer");

      CHECK(wifi_time_zone_parse("EST5EDT", &z) && z.std_offset == -18000 && z.dst_offset == -14400,
            "EST5EDT parses with the default US rules");
      CHECK(wifi_time_zone_offset(&z, at(2026, 3, 8, 6, 59, 59)) == -18000
            && wifi_time_zone_offset(&z, at(2026, 3, 8, 7, 0, 0)) == -14400,
            "US: starts at 02:00 EST on the second Sunday of March");
      CHECK(wifi_time_zone_offset(&z, at(2026, 11, 1, 5, 59, 59)) == -14400
            && wifi_time_zone_offset(&z, at(2026, 11, 1, 6, 0, 0)) == -18000,
            "US: ends at 02:00 EDT on the first Sunday of November");

      CHECK(wifi_time_zone_parse("AEST-10AEDT,M10.1.0,M4.1.0/3", &z) && z.std_offset == 36000,
            "Sydney zone parses (east of Greenwich is negative)");
      CHECK(wifi_time_zone_offset(&z, at(2026, 10, 3, 15, 59, 59)) == 36000
            && wifi_time_zone_offset(&z, at(2026, 10, 3, 16, 0, 0)) == 39600,
            "Sydney: summer time starts in October");
      CHECK(wifi_time_zone_offset(&z, at(2026, 4, 4, 15, 59, 59)) == 39600
            && wifi_time_zone_offset(&z, at(2026, 4, 4, 16, 0, 0)) == 36000,
            "Sydney: ends in April, so the period runs across New Year");
      CHECK(wifi_time_zone_offset(&z, at(2026, 1, 1, 0, 0, 0)) == 39600
            && wifi_time_zone_offset(&z, at(2026, 7, 1, 0, 0, 0)) == 36000, "Sydney: January and July");

      CHECK(wifi_time_zone_parse("CET-1CEST-2,M3.5.0,M10.5.0/3", &z) && z.dst_offset == 7200
            && wifi_time_zone_offset(&z, at(2026, 3, 29, 1, 0, 0)) == 7200
            && wifi_time_zone_offset(&z, at(2026, 10, 25, 0, 59, 59)) == 7200
            && wifi_time_zone_offset(&z, at(2026, 10, 25, 1, 0, 0)) == 3600,
            "central Europe, with a stated summer offset and a change at 03:00");
      CHECK(wifi_time_zone_parse("<+0545>-5:45", &z) && !z.has_dst && z.std_offset == 20700
            && wifi_time_zone_offset(&z, at(2026, 7, 1, 0, 0, 0)) == 20700, "Nepal: a quoted name, no summer time");
      CHECK(wifi_time_zone_parse("<-03>3", &z) && z.std_offset == -10800, "a quoted name with a sign");
      CHECK(wifi_time_zone_parse("UTC0", &z) && z.std_offset == 0 && !z.has_dst, "UTC0");

      CHECK(wifi_time_zone_parse("CET", &z) && z.has_dst && z.std_offset == 3600 && z.dst_offset == 7200
            && wifi_time_zone_offset(&z, at(2026, 3, 29, 0, 59, 59)) == 3600
            && wifi_time_zone_offset(&z, at(2026, 3, 29, 1, 0, 0)) == 7200
            && wifi_time_zone_offset(&z, at(2026, 10, 11, 10, 15, 41)) == 7200
            && wifi_time_zone_offset(&z, at(2026, 10, 25, 1, 0, 0)) == 3600,
            "CET, the tz database zone: CEST from the last Sunday of March to that of October");
      CHECK(wifi_time_zone_parse("MET", &z) && z.std_offset == 3600 && z.dst_offset == 7200, "MET");
      CHECK(wifi_time_zone_parse("BST", &z) && z.has_dst && z.std_offset == 0 && z.dst_offset == 3600
            && wifi_time_zone_offset(&z, at(2026, 3, 29, 0, 59, 59)) == 0
            && wifi_time_zone_offset(&z, at(2026, 3, 29, 1, 0, 0)) == 3600
            && wifi_time_zone_offset(&z, at(2026, 10, 25, 0, 59, 59)) == 3600
            && wifi_time_zone_offset(&z, at(2026, 10, 25, 1, 0, 0)) == 0,
            "BST is the UK: GMT, and BST between the last Sundays of March and October");
      CHECK(wifi_time_zone_parse("EET", &z) && z.std_offset == 7200 && z.dst_offset == 10800
            && wifi_time_zone_offset(&z, at(2026, 3, 29, 0, 59, 59)) == 7200
            && wifi_time_zone_offset(&z, at(2026, 3, 29, 1, 0, 0)) == 10800
            && wifi_time_zone_offset(&z, at(2026, 10, 25, 0, 59, 59)) == 10800
            && wifi_time_zone_offset(&z, at(2026, 10, 25, 1, 0, 0)) == 7200, "EET changes at 01:00 UTC");
      CHECK(wifi_time_zone_parse("WET", &z) && z.std_offset == 0 && z.dst_offset == 3600
            && wifi_time_zone_offset(&z, at(2026, 7, 1, 12, 0, 0)) == 3600, "WET");
      CHECK(wifi_time_zone_parse("UTC", &z) && !z.has_dst && z.std_offset == 0
            && wifi_time_zone_parse("GMT", &z) && !z.has_dst && z.std_offset == 0, "UTC and GMT");
      CHECK(wifi_time_zone_parse("EST", &z) && !z.has_dst && z.std_offset == -18000
            && wifi_time_zone_parse("MST", &z) && z.std_offset == -25200
            && wifi_time_zone_parse("HST", &z) && z.std_offset == -36000, "EST, MST, HST are fixed offsets");
      CHECK(!wifi_time_zone_parse("cet", &z) && !wifi_time_zone_parse("Cet", &z) && !wifi_time_zone_parse("CEST", &z)
            && !wifi_time_zone_parse("PST", &z), "IDs are exact: not cet, not CEST, not PST (use PST8PDT)");
      CHECK(wifi_time_zone_parse("GMT+8", &z) && z.std_offset == -28800
            && wifi_time_zone_parse("GMT-7:30", &z) && z.std_offset == 27000
            && wifi_time_zone_parse("UTC-3", &z) && z.std_offset == 10800
            && wifi_time_zone_parse("<+08>-8", &z) && z.std_offset == 28800,
            "POSIX signs: GMT+8 is 8 hours WEST (UTC-8), GMT-7:30 is east (UTC+7:30)");
      CHECK(!wifi_time_zone_parse("GMT-7.5", &z) && !wifi_time_zone_parse("GMT+8.5", &z),
            "decimal hours are not POSIX: write GMT-7:30");
      CHECK(!wifi_time_zone_parse(NULL, &z) && !wifi_time_zone_parse("", &z), "no string: no zone");
      CHECK(!wifi_time_zone_parse("GM0", &z) && !wifi_time_zone_parse("GMT+", &z)
            && !wifi_time_zone_parse("GMT0BST,M3.5.0", &z) && !wifi_time_zone_parse("GMT0BST,M13.1.0,M10.5.0", &z)
            && !wifi_time_zone_parse("GMT0BST,M3.0.0,M10.5.0", &z) && !wifi_time_zone_parse("GMT0BST,M3.5.7,M10.5.0", &z)
            && !wifi_time_zone_parse("GMT0X", &z) && !wifi_time_zone_parse("GMT25", &z)
            && !wifi_time_zone_parse("GMT0:60", &z) && !wifi_time_zone_parse("<>0", &z)
            && !wifi_time_zone_parse("GMT0BST,M3.5.0/1,M10.5.0x", &z) && !wifi_time_zone_parse("GMT0BST,J60,J300", &z)
            && !wifi_time_zone_parse("Europe/London", &z), "malformed strings and unsupported rule forms are refused");
      CHECK(z.has_dst == false && z.std_offset == 0, "and leave no zone");
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   printf(fails ? "WIFITIME TESTS FAILED\n" : "WIFITIME TESTS PASSED\n");
   return fails ? 1 : 0;
}
