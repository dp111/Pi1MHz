/* wifi_time.c - see wifi_time.h */
#include <stddef.h>
#include <string.h>

#include "wifi_time.h"

#define SECONDS_PER_DAY 86400

static bool leap_year(uint32_t year)
{
   return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static uint32_t month_length(uint32_t year, uint32_t month)
{
   static const uint8_t length[] = {31,28,31,30,31,30,31,31,30,31,30,31};
   return length[month - 1u] + ((month == 2u && leap_year(year)) ? 1u : 0u);
}

/* The proleptic Gregorian day count, after H. Hinnant's civil-date
   algorithms; the year starts in March so the leap day is the year's last.
   Signed, so that days before 1970 count down from it. */
int64_t wifi_time_days_from_civil(uint32_t year, uint32_t month, uint32_t day)
{
   int64_t y = month <= 2u ? (int64_t)year - 1 : (int64_t)year;
   int64_t era = (y >= 0 ? y : y - 399) / 400;
   int64_t yoe = y - era * 400;
   int64_t doy = (153 * (month > 2u ? (int64_t)month - 3 : (int64_t)month + 9) + 2) / 5
               + (int64_t)day - 1;
   int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   return era * 146097 + doe - 719468;
}

void wifi_time_civil(int64_t days, uint32_t *year, uint32_t *month,
                     uint32_t *day)
{
   int64_t z = days + 719468;
   int64_t era = (z >= 0 ? z : z - 146096) / 146097;
   int64_t doe = z - era * 146097;
   int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
   int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
   int64_t mp = (5 * doy + 2) / 153;
   int64_t m = mp < 10 ? mp + 3 : mp - 9;
   *day = (uint32_t)(doy - (153 * mp + 2) / 5 + 1);
   *month = (uint32_t)m;
   *year = (uint32_t)(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

void wifi_time_split(int64_t seconds, wifi_time_fields_t *fields)
{
   int64_t days = seconds / SECONDS_PER_DAY;
   int64_t rest = seconds % SECONDS_PER_DAY;
   uint32_t year, month, day;

   if (rest < 0) {                      /* floor, not truncate, before 1970 */
      rest += SECONDS_PER_DAY;
      days--;
   }
   if (days < WIFI_TIME_FIRST_DAY) {
      days = WIFI_TIME_FIRST_DAY;
      rest = 0;
   } else if (days > WIFI_TIME_LAST_DAY) {
      days = WIFI_TIME_LAST_DAY;
      rest = SECONDS_PER_DAY - 1;
   }
   wifi_time_civil(days, &year, &month, &day);
   fields->year = (uint16_t)year;
   fields->month = (uint8_t)month;
   fields->day = (uint8_t)day;
   fields->hour = (uint8_t)(rest / 3600);
   fields->minute = (uint8_t)(rest / 60 % 60);
   fields->second = (uint8_t)(rest % 60);
}

/* Digits at *text, at most `max` of them.  Advances past them. */
static bool parse_number(const char **text, uint32_t max, uint32_t *value,
                         uint32_t *digits)
{
   uint32_t n = 0u, v = 0u;
   while (**text >= '0' && **text <= '9') {
      if (++n > 10u) return false;
      v = v * 10u + (uint32_t)(**text - '0');
      if (v > max) return false;
      (*text)++;
   }
   *value = v;
   *digits = n;
   return n > 0u;
}

static bool parse_date(const char *text, wifi_time_shift_t *shift)
{
   uint32_t part[3], digits, n = 0u;
   uint32_t day = 0u, month = 0u, year;

   for (;;) {
      if (n == 3u || !parse_number(&text, 9999u, &part[n], &digits))
         return false;
      n++;
      if (*text != '.') break;
      text++;
   }
   if (*text != '\0' || n < 2u) return false;
   year = part[n - 1u];
   month = part[n - 2u];
   if (n == 3u) day = part[0];
   if (year < WIFI_TIME_FIRST_YEAR || year > WIFI_TIME_LAST_YEAR
       || month < 1u || month > 12u || (n == 3u && (day < 1u || day > 31u)))
      return false;
   shift->kind = WIFI_TIME_SHIFT_DATE;
   shift->year = (uint16_t)year;
   shift->month = (uint8_t)month;
   shift->day = (uint8_t)day;
   return true;
}

bool wifi_time_shift_parse(const char *text, wifi_time_shift_t *shift)
{
   bool negative = false, signed_form = false;
   uint32_t value, digits;
   int64_t seconds, unit;

   memset(shift, 0, sizeof *shift);
   if (text == NULL || text[0] == '\0') return true;
   if (strchr(text, '.') != NULL) {
      if (parse_date(text, shift)) return true;
      memset(shift, 0, sizeof *shift);
      return false;
   }
   if (*text == '+' || *text == '-') {
      negative = *text++ == '-';
      signed_form = true;
   }
   switch (*text++) {
      case 'y':                                   /* a year, never signed */
         if (signed_form || !parse_number(&text, WIFI_TIME_LAST_YEAR, &value, &digits)
             || *text != '\0' || value < WIFI_TIME_FIRST_YEAR)
            return false;
         shift->kind = WIFI_TIME_SHIFT_DATE;
         shift->year = (uint16_t)value;
         return true;
      case 's': unit = 1; break;
      case 'm': unit = 60; break;
      case 'h': unit = 3600; break;
      case 'd': unit = SECONDS_PER_DAY; break;
      default: return false;
   }
   if (!parse_number(&text, 0x7FFFFFFFu, &value, &digits) || *text != '\0')
      return false;
   seconds = (int64_t)value * unit;
   shift->kind = WIFI_TIME_SHIFT_SECONDS;
   shift->seconds = negative ? -seconds : seconds;
   return true;
}

int64_t wifi_time_shift_seconds(wifi_time_shift_t *shift,
                                int64_t local_seconds)
{
   if (shift->kind == WIFI_TIME_SHIFT_DATE && !shift->resolved) {
      int64_t now_days = local_seconds / SECONDS_PER_DAY;
      uint32_t year, month, day, last;
      if (local_seconds % SECONDS_PER_DAY < 0) now_days--;
      wifi_time_civil(now_days, &year, &month, &day);
      if (shift->year != 0u) year = shift->year;
      if (shift->month != 0u) month = shift->month;
      if (shift->day != 0u) day = shift->day;
      last = month_length(year, month);
      if (day > last) day = last;
      shift->seconds = (wifi_time_days_from_civil(year, month, day) - now_days)
                       * SECONDS_PER_DAY;
      shift->resolved = true;
   }
   return shift->kind == WIFI_TIME_SHIFT_NONE ? 0 : shift->seconds;
}
