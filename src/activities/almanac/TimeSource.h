#pragma once
//
// TimeSource.h — wall-clock time for the Almanac.
//
// WHY THIS IS FIDDLY: these devices differ in whether they have a clock at all.
//
//   X3      DS3231 at 0x68, battery-backed. Keeps time with the device off.
//   Sticky  PCF8563 at 0x51. Same.
//   X4      no RTC chip. BoardConfig gives it rtcAddr == 0.
//
// On a board WITH an RTC, that chip is the source of truth and the time survives
// everything, including a flat battery.
//
// On a board WITHOUT one, the only clock is the ESP32's system clock, and on
// these devices that survives nothing: HalPowerManager::startDeepSleep() pulls
// GPIO13 low, which opens the battery latch MOSFET and powers the MCU down
// completely -- RTC domain included. Its own comment says so. Waking is a cold
// boot. Elapsed time cannot be recovered without a clock, so we do not pretend
// to: the time reads as unset, and the Clock module pre-fills its editor with
// whatever the user last entered, so re-setting it is quick.
//
// The freeink Rtc library drives both chips, reports present() == false on
// boards without one, and returns false from now() when the oscillator has
// stopped (low voltage, or never set) -- so one code path serves every device.
//
// Everything here is UTC. Local civil time comes from Location.
//
// Requires in platformio.ini:  Rtc=symlink://freeink-sdk/libs/hardware/Rtc
//
#include <Preferences.h>

// The freeink Rtc library is an optional dependency. Add this to platformio.ini
// to get a real hardware clock on boards that have one (X3, Sticky):
//
//     Rtc=symlink://freeink-sdk/libs/hardware/Rtc
//
// Without it everything still builds; the board simply reports no clock, which
// is already the truth on the X4.
// Opt in explicitly rather than sniffing for a header named Rtc.h -- that name
// is not unique on the include path.
#ifdef ALMANAC_USE_FREEINK_RTC
#include <Rtc.h>
#define ALMANAC_HAS_RTC_LIB 1
#endif

#include <ctime>
#include <sys/time.h>

#include "Location.h"  // also pulls in sky_math.h for sky_dowSakamoto

class TimeSource {
 public:
  // Epoch of 2025-01-01; anything earlier means "not set".
  static constexpr time_t MIN_VALID = 1735689600;

  // Call before reading the time. Idempotent and cheap. Seeds the system clock
  // from the hardware RTC when the board has one.
  static void begin() {
    if (started()) return;
    started() = true;
#ifdef ALMANAC_HAS_RTC_LIB
    if (!rtc().begin()) return;  // no RTC on this board (e.g. the X4)

    Rtc::DateTime dt;
    if (!rtc().now(dt)) return;  // I2C error, or the oscillator stopped
    if (dt.year < 2025) return;  // never set

    setSystemClock(epochFromUtcParts(dt.year, dt.month, dt.day, dt.hour, dt.minute) + dt.second);
#endif
  }

  // True when the board keeps time in hardware, so it survives power-off.
  static bool hasHardwareClock() {
#ifdef ALMANAC_HAS_RTC_LIB
    return rtc().present();
#else
    return false;
#endif
  }

  static bool isSet() { return time(nullptr) >= MIN_VALID; }
  static time_t nowUtc() { return time(nullptr); }

  // Sets the system clock and the hardware RTC (if any), and remembers the value
  // so a clockless board can pre-fill the editor after a cold boot.
  static void setUtc(time_t t) {
    setSystemClock(t);
    rememberUtc(t);
#ifdef ALMANAC_HAS_RTC_LIB
    if (!rtc().present()) return;

    int y, mo, d, hh, mm;
    utcParts(t, y, mo, d, hh, mm);
    Rtc::DateTime dt;
    dt.year = (uint16_t)y;
    dt.month = (uint8_t)mo;
    dt.day = (uint8_t)d;
    dt.hour = (uint8_t)hh;
    dt.minute = (uint8_t)mm;
    dt.second = (uint8_t)(((t % 60) + 60) % 60);
    dt.weekday = (uint8_t)sky_dowSakamoto(y, mo, d);  // 0 = Sunday, as Rtc expects
    rtc().set(dt);
#endif
  }

  // What the user last entered, or 0. NOT the current time: elapsed time cannot
  // be recovered without a clock. Only useful for pre-filling the editor.
  static time_t lastKnownUtc() {
    Preferences p;
    p.begin("almanac", true);
    const uint32_t v = p.getUInt("lastutc", 0);
    p.end();
    return (time_t)v;
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

 private:
#ifdef ALMANAC_HAS_RTC_LIB
  static Rtc& rtc() {
    static Rtc r;
    return r;
  }
#endif
  static bool& started() {
    static bool b = false;
    return b;
  }
  static void setSystemClock(time_t t) {
    timeval tv{};
    tv.tv_sec = t;
    settimeofday(&tv, nullptr);
  }
  static void rememberUtc(time_t t) {
    Preferences p;
    p.begin("almanac", false);
    p.putUInt("lastutc", (uint32_t)t);
    p.end();
  }
};
