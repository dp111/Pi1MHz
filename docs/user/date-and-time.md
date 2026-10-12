# Date and time

The [1MHz-WiFi ROM](wifi-rom.md)'s `*DATE`, `*TIME` and `*WTIME` get the time
from an NTP server over WiFi. Three settings in `Pi1MHz.cfg` say which server,
which time zone, and (if you want one) a different date or time skew.

Everything here needs the 1MHz-WiFi ROM working, so `wifi_service_enable=1` and
`net_enable=1` must be set (see [WiFi ROM](wifi-rom.md)).

## What each answer comes from

Each question is answered by the first line that applies:

| Question | Setting | Answered by, in order |
|---|---|---|
| Which time server? | `ntp_server` | 1. the setting<br>2. the NTP server your network hands out with the Pi's address (DHCP option 42)<br>3. `pool.ntp.org` |
| Which time zone? | `ntp_timezone` | 1. the setting<br>2. the time zone your network hands out (DHCP option 100)<br>3. `wifi_service_utc_offset_minutes`, which is 0 (UTC) if you set nothing |
| Another date? | `time_shift` | the setting only; when not used the real date and time are shown |

Few routers hand out a time zone (option 100), so you will usually set
`ntp_timezone` yourself. The Pi's [status page](web-interface.md) shows the server and time zone in use and
where each came from, for example `192.168.0.1 (DHCP option 42)` or
`CET (config ntp_timezone)`.

If a setting cannot be understood it is ignored, as if it were not there, and
the status page says so (`ntp_timezone not understood`). The Beeb then shows UTC (or your
`wifi_service_utc_offset_minutes`), which is the usual sign of a typo in
`ntp_timezone`.

## ntp_server

The NTP server to ask, as a name or an IP address:

    ntp_server=192.168.0.1
    ntp_server=uk.pool.ntp.org

Use it when your network has its own time server, or blocks outside NTP.
If an ntp_server is not given, it defaults to pool.ntp.org

## ntp_timezone

Your time zone, including summer time. There are two ways to write it.

### A zone name

The easy way. Write one of these exactly as shown, in capitals:

| Write | Zone |
|---|---|
| `BST` | United Kingdom: GMT in winter, BST (UTC+1) in summer, from the last Sunday of March to the last Sunday of October |
| `CET` or `MET` | Central Europe: UTC+1, UTC+2 in summer (last Sunday of March to last Sunday of October) |
| `EET` | Eastern Europe: UTC+2, UTC+3 in summer |
| `WET` | Western Europe (Portugal, Canary Islands): UTC+0, UTC+1 in summer |
| `UTC` or `GMT` | UTC, no summer time |
| `EST`, `MST`, `HST` | Fixed UTC-5, UTC-7, UTC-10, no summer time |

For the United Kingdom use `BST`. `GMT` on its own never changes to summer time.

### A POSIX time zone string

For anything else, write the zone the way every Unix system does, as a
"POSIX TZ string". Build it in three steps; stop where your zone stops.

1. **Winter name and offset.** A name of three or more letters, then the
   offset **in hours west of Greenwich**. West, not east: a zone *ahead* of UTC
   has a *minus* sign.

   | Zone | Write | Because |
   |---|---|---|
   | UK winter | `GMT0` | UTC itself |
   | Germany winter | `CET-1` | one hour ahead of UTC, so -1 |
   | New York winter | `EST5` | five hours behind UTC, so 5 |
   | India | `<+0530>-5:30` | 5 h 30 min ahead; a name that is just a number goes in `< >` |

   Hours and minutes are written `5:30`. Decimals such as `5.5` are **not**
   allowed.

   If your zone has no summer time, this is all you need: `ntp_timezone=CET-1`.

2. **Add summer time.** Put its name straight after, for example `GMT0BST`
   or `CET-1CEST`. Summer time is one hour ahead of winter time. (Add an offset
   after the name only for a zone where the difference is not one hour.) On its
   own this uses the United States dates, so go on to step 3 outside the US.

3. **Say when summer time starts and ends.** Add a comma and the start, a
   comma and the end. Each is written `M` *month* `.` *week* `.` *day*, and can
   be followed by `/` and an hour:

   * *month* is 1 to 12;
   * *week* is 1 to 4, or **5 for the last** week of the month;
   * *day* is 0 for Sunday up to 6 for Saturday;
   * the optional `/hour` is the time of the change, on the clock that is
     showing *before* the change (2, meaning 02:00, if you leave it out).

   So `M3.5.0/1` means "the **5**th (last) week, day **0** (Sunday), of month
   **3** (March), at 01:00", and `M10.5.0` means "the last Sunday of October,
   at 02:00 (summer time)". The whole string for the UK is:

       GMT0BST,M3.5.0/1,M10.5.0

   The UK changes at 01:00 UTC both times. That is 01:00 on the clocks in March
   (they show GMT) but 02:00 in October (they show BST), which is why the start
   has `/1` and the end uses the 02:00 default.

Examples:

| Where | `ntp_timezone=` |
|---|---|
| UK, Ireland | `BST` (the same as `GMT0BST,M3.5.0/1,M10.5.0`) |
| Central Europe | `CET` (the same as `CET-1CEST,M3.5.0,M10.5.0/3`) |
| US East, Central, Mountain, Pacific | `EST5EDT`, `CST6CDT`, `MST7MDT`, `PST8PDT` |
| Sydney | `AEST-10AEDT,M10.1.0,M4.1.0/3` |
| China, Singapore, Western Australia (fixed UTC+8) | `<+08>-8` |
| India | `<+0530>-5:30` |

Two traps: `GMT+8` is **UTC-8** (west positive), and `CEST` on its own is not
a zone name. Use `CET`, or the full string.

`webdav_utc_offset_minutes` is a different setting: it is only for the file
times shown by the [web interface](web-interface.md).

## wifi_service_utc_offset_minutes

The older, simple setting: a fixed offset in minutes from UTC, with no summer
time (`60` for UTC+1). It was the only time zone setting before `ntp_timezone`
existed and is kept so that a `Pi1MHz.cfg` written for an earlier version goes
on working. It is separate from the Beeb and from the network: if your country
has summer time you have to change it by hand twice a year, which `ntp_timezone`
does for you, so prefer `ntp_timezone`. It is used only when there is no
`ntp_timezone` and the network gives none.

## time_shift

Shows a different date or time to the Beeb, for software that expects another
year or an earlier period. The real time is shifted; the Pi's clock is not
touched.

A date (parts you leave out come from today; a day the month does not have
becomes its last day; the time of day is kept):

| Write | Shows |
|---|---|
| `y1987` | the same day and month in 1987 |
| `04.1987` | the same day, in April 1987 |
| `31.12.1999` | 31 December 1999 |

A date is worked out once, the first time the Pi gets the time after power-up,
and kept: the clock then runs on from it. After a reboot it starts again from
the date you set.

An amount to skew the clock time by from the current date/time, written as a letter for the unit and a number,
with `-` in front for earlier:

| Write | Moves the clock by |
|---|---|
| `s30` | 30 seconds later |
| `m5` | 5 minutes later |
| `h2` | 2 hours later |
| `-d10` | 10 days earlier |

Plain numbers (`3600`) are refused: every number needs its letter.

Any year from 1 to 9999 can be used, including years before 1970. A clock that
would go past either end stops there.

Summer time follows the real date, not the date you shift to. With
`ntp_timezone=BST` and `time_shift=y1987` in July, the Beeb shows July 1987 an
hour ahead of UTC, because it is summer now.

## If it shows the wrong time

* **UTC when you expected local time:** `ntp_timezone` was not understood and
  was ignored. The status page says `UTC+00:00 (wifi_service_utc_offset_minutes)`
  plus `ntp_timezone not understood`.
* **An hour out in summer:** the rule dates are wrong, or the zone has no
  summer time written. Compare your string with the table above.
* **A different server than you expected:** the status page shows which setting
  won, and what the network handed out.
