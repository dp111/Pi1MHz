/* wifi_time.h - calendar arithmetic and the clock shift for the WiFi ROM's
 * *DATE and *TIME.
 *
 * Pure functions, no I/O and no lwIP: wifi_service.c feeds them the seconds
 * from the network and the config strings, so they are host-testable.
 *
 * The shift (config key ntp_offset) moves the clock the Beeb is shown, for
 * a machine whose software expects another year or a period of time that
 * has long gone.  Accepted forms:
 *
 *    dd.mm.yyyy   that date; the time of day is kept
 *    mm.yyyy      that month and year, the day of the month kept
 *    yYYYY        that year (1-9999), the month and day kept: y2020
 *    sN mN hN dN  N seconds, minutes, hours or days later, with a - before
 *                 the letter for earlier: s3600  h2  -m30  +d10
 *
 * Plain digits mean nothing: every number says what it counts.
 *
 * Missing date parts are taken from the current local date.  A day the
 * month does not have is moved back to the month's last day.  A date is
 * turned into a whole number of days once, from the first time it is used,
 * and that number is kept, so the clock then runs on from the chosen date.
 *
 * Any year from 1 to 9999 can be shown (the Gregorian calendar carried back
 * and forward); a clock that would pass either end stops there.  The clock
 * the Pi gets from the network is a 32-bit count of seconds from 1970, so the
 * real time is within 1970-2106, but the date shown can be anywhere.
 */
#ifndef WIFI_TIME_H
#define WIFI_TIME_H

#include <stdbool.h>
#include <stdint.h>

/* The years that can be shown, and their first and last days counted from
   1970-01-01 (0001-01-01 is day -719162, 9999-12-31 is day 2932896). */
#define WIFI_TIME_FIRST_YEAR 1u
#define WIFI_TIME_LAST_YEAR  9999u
#define WIFI_TIME_FIRST_DAY  (-719162)
#define WIFI_TIME_LAST_DAY   2932896

/* Days since 1970-01-01 (negative before it) -> calendar date (month 1-12,
   day 1-31).  `days` must be in WIFI_TIME_FIRST_DAY..WIFI_TIME_LAST_DAY. */
void wifi_time_civil(int64_t days, uint32_t *year, uint32_t *month,
                     uint32_t *day);
/* Calendar date -> days since 1970-01-01. */
int64_t wifi_time_days_from_civil(uint32_t year, uint32_t month,
                                  uint32_t day);

typedef struct {
   uint16_t year;            /* 1-9999 */
   uint8_t month;            /* 1-12 */
   uint8_t day;              /* 1-31 */
   uint8_t hour, minute, second;
} wifi_time_fields_t;

/* Seconds since 1970-01-01 00:00:00 (negative before it) -> the fields to
   show.  Outside 0001-01-01..9999-12-31 it stops at that end. */
void wifi_time_split(int64_t seconds, wifi_time_fields_t *fields);

typedef enum {
   WIFI_TIME_SHIFT_NONE,
   WIFI_TIME_SHIFT_SECONDS,
   WIFI_TIME_SHIFT_DATE
} wifi_time_shift_kind_t;

typedef struct {
   wifi_time_shift_kind_t kind;
   int64_t seconds;          /* SECONDS: the shift; DATE: once resolved     */
   uint16_t year;            /* DATE: 0 = the current year                  */
   uint8_t month;            /* DATE: 0 = the current month                 */
   uint8_t day;              /* DATE: 0 = the current day of the month      */
   bool resolved;            /* DATE: seconds holds the whole days' shift   */
} wifi_time_shift_t;

/* Parse the config value.  NULL or empty is no shift and returns true; a
   value that is not one of the forms above, or out of range, returns false
   and leaves no shift. */
bool wifi_time_shift_parse(const char *text, wifi_time_shift_t *shift);

/* Seconds to add to `local_seconds` (seconds since 1970 in local time).  A
   date shift is resolved from `local_seconds` on its first call. */
int64_t wifi_time_shift_seconds(wifi_time_shift_t *shift,
                                int64_t local_seconds);

/* ---- Time zone with summer time: a POSIX TZ string ------------------------
 *
 * std offset [dst [offset] [,start[/time],end[/time]]], e.g.
 *    GMT0BST,M3.5.0/1,M10.5.0        UK
 *    CET-1CEST,M3.5.0,M10.5.0/3      central Europe
 *    EST5EDT                         US east, the default US rules
 *    <+0545>-5:45                    Nepal, a name in angle brackets
 * An offset is hours[:minutes[:seconds]] WEST of Greenwich, so east of it
 * is negative.  The dst offset defaults to one hour ahead of std.  Rules are
 * Mm.w.d only (month 1-12, week 1-5 with 5 meaning the last, day 0-6 from
 * Sunday); a time is in the local time in force before the change and
 * defaults to 02:00.  Without rules, M3.2.0,M11.1.0 (the US ones) apply.

 *
 * The tz database's legacy zone IDs are accepted by name, as the POSIX strings
 * they stand for: UTC, GMT, WET, CET, MET, EET, EST, MST, HST (exact case),
 * and BST for the UK (GMT0BST,M3.5.0/1,M10.5.0).
 */
typedef struct {
   uint8_t month;            /* 1-12 */
   uint8_t week;             /* 1-5 */
   uint8_t weekday;          /* 0 = Sunday */
   int32_t time;             /* seconds after local midnight */
} wifi_time_rule_t;

typedef struct {
   bool has_dst;
   int32_t std_offset;       /* seconds EAST of UTC */
   int32_t dst_offset;
   wifi_time_rule_t start, end;
} wifi_time_zone_t;

/* false (and *zone cleared) for NULL, empty or anything not understood. */
bool wifi_time_zone_parse(const char *text, wifi_time_zone_t *zone);

/* Seconds to add to UTC for local time at `utc_seconds`. */
int32_t wifi_time_zone_offset(const wifi_time_zone_t *zone, int64_t utc_seconds);

#endif
