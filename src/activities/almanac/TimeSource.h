#pragma once
//
// TimeSource.h — wall-clock time for the Almanac modules on the Xteink X4.
//
// WHY THIS EXISTS: the X4 has NO hardware RTC. CrossPoint's HalClock talks to a
// DS3231 and returns early unless gpio.deviceIsX3(), which is why the stock
// firmware hides all clock settings on this device. The Watchy had a PCF8563;
// here there is nothing.
//
// So we drive the ESP32's own system clock with settimeofday(). That clock is
// maintained by the RTC timer across deep sleep, so the time survives sleeping
// and waking — but it is lost on a full power-off or battery death, exactly the
// same tradeoff as a PCF8563 with no backup cell. The Clock module lets the
// user re-set it; everything else reads it.
//
// All epochs are UTC. Local civil time is derived from Location (lat/lon, a
// standard-time UTC offset in minutes, and a DST rule) -- NOT from a hardcoded
// timezone. Offsets are handled in MINUTES so that :30 and :45 zones work.
//
#include <ctime>
#include <sys/time.h>

#include "Location.h"

class TimeSource {
 public:
  // Epoch of 2025-01-01; anything earlier means "never set this power cycle".
  static constexpr time_t MIN_VALID = 1735689600;

  static bool isSet() { return time(nullptr) >= MIN_VALID; }
  static time_t nowUtc() { return time(nullptr); }

  static void setUtc(time_t t) {
    timeval tv{};
    tv.tv_sec = t;
    settimeofday(&tv, nullptr);
  }

  // --- civil <-> epoch, without relying on timegm() (absent in some newlibs).
  // Howard Hinnant's days_from_civil; valid for any proleptic Gregorian date.
  static long daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
  }
  static void civilFromDays(long z, int& y, int& m, int& d) {
    z += 719468L;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long yr = (long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp + (mp < 10 ? 3 : -9));
    y = (int)(yr + (m <= 2));
  }

  static time_t epochFromUtcParts(int y, int mo, int d, int hh, int mm) {
    return daysFromCivil(y, mo, d) * 86400L + hh * 3600L + mm * 60L;
  }
  static void utcParts(time_t t, int& y, int& mo, int& d, int& hh, int& mm) {
    long days = (long)(t / 86400);
    long rem = (long)(t % 86400);
    if (rem < 0) {
      rem += 86400;
      days -= 1;
    }
    civilFromDays(days, y, mo, d);
    hh = (int)(rem / 3600);
    mm = (int)((rem % 3600) / 60);
  }

  // UTC epoch -> local civil parts. Returns the offset used, in MINUTES.
  static int localPartsMin(time_t utc, int& y, int& mo, int& d, int& hh, int& mm) {
    Location::begin();
    int uy, umo, ud, uh, um;
    utcParts(utc, uy, umo, ud, uh, um);
    // Guess the offset from the UTC date, then refine against the local date:
    // the DST boundary is defined in local time, so one pass is not enough.
    int off = Location::utcOffsetMinutes(uy, umo, ud, uh);
    utcParts(utc + (long)off * 60L, y, mo, d, hh, mm);
    const int off2 = Location::utcOffsetMinutes(y, mo, d, hh);
    if (off2 != off) {
      off = off2;
      utcParts(utc + (long)off * 60L, y, mo, d, hh, mm);
    }
    return off;
  }

  // Local civil parts -> UTC epoch (local = utc + off  =>  utc = local - off).
  static time_t epochFromLocalParts(int y, int mo, int d, int hh, int mm) {
    Location::begin();
    const int off = Location::utcOffsetMinutes(y, mo, d, hh);
    return epochFromUtcParts(y, mo, d, hh, mm) - (long)off * 60L;
  }
};
