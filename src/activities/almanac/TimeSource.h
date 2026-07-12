#pragma once
//
// TimeSource.h — wall-clock time for the Almanac.
//
//   X3      DS3231 at 0x68, battery-backed. Keeps time with the device off.
//   Sticky  PCF8563 at 0x51. Same.
//   X4      no RTC chip. BoardConfig gives it rtcAddr == 0.
//
// On a board WITH an RTC, that chip is the source of truth and the time survives
// power-off. Without one, the ESP32 system clock is all there is, and on these
// devices deep sleep powers the MCU down completely (HalPowerManager pulls the
// battery-latch MOSFET), so the time is lost on sleep. We do not pretend it
// isn't: the time reads as unset and the editor pre-fills from the last value.
//
// TWO SUBTLETIES THAT CAUSED AN X3 TO BEHAVE LIKE AN X4:
//
//   1. On the dual X3/X4 ESP32-C3 binary, BoardConfig::ACTIVE boots as X4 and is
//      swapped to X3 at RUNTIME by setDisplayX3() during display init. So the
//      RTC address is only correct AFTER that. We therefore (re)try rtc().begin()
//      lazily on every access until it succeeds, rather than probing once at a
//      fixed early moment and latching the result.
//
//   2. The library is optional at compile time. If the build does not define
//      ALMANAC_USE_FREEINK_RTC (and add the Rtc lib to lib_deps), the X3 has no
//      RTC support and silently behaves like an X4. rtcStatus() reports which
//      case you are in, so the Clock screen can say so instead of leaving you to
//      guess.
//
// platformio.ini, to enable the hardware clock on an X3:
//     lib_deps  += Rtc=symlink://freeink-sdk/libs/hardware/Rtc
//     build_flags+= -DALMANAC_USE_FREEINK_RTC=1
//
#include <Preferences.h>

#ifdef ALMANAC_USE_FREEINK_RTC
#include <Rtc.h>
#define ALMANAC_HAS_RTC_LIB 1
#endif

#include <ctime>
#include <sys/time.h>

#include "Location.h"  // also pulls in sky_math.h for sky_dowSakamoto

class TimeSource {
 public:
  static constexpr time_t MIN_VALID = 1735689600;  // 2025-01-01

  enum class Clock : uint8_t { NoLibrary, NoChip, Ready };

  // Reports what kind of clock this build+board actually has. Drives the honest
  // on-screen message, and re-probes the RTC each call so it becomes Ready as
  // soon as the runtime board swap has happened.
  static Clock rtcStatus() {
#ifndef ALMANAC_HAS_RTC_LIB
    return Clock::NoLibrary;   // built without the Rtc library
#else
    return ensureRtc() ? Clock::Ready : Clock::NoChip;
#endif
  }

  static bool hasHardwareClock() { return rtcStatus() == Clock::Ready; }

  // Seed the system clock from the RTC. Safe to call repeatedly; cheap once done.
  static void begin() {
#ifdef ALMANAC_HAS_RTC_LIB
    if (systemSeeded()) return;
    if (!ensureRtc()) return;  // board not swapped yet, or genuinely no chip

    Rtc::DateTime dt;
    if (!rtc().now(dt)) return;  // I2C error, or oscillator stopped
    if (dt.year < 2025) { systemSeeded() = true; return; }  // chip present but never set

    setSystemClock(epochFromUtcParts(dt.year, dt.month, dt.day, dt.hour, dt.minute) + dt.second);
    systemSeeded() = true;
#endif
  }

  static bool isSet() { return time(nullptr) >= MIN_VALID; }
  static time_t nowUtc() { return time(nullptr); }

  static void setUtc(time_t t) {
    setSystemClock(t);
    rememberUtc(t);
#ifdef ALMANAC_HAS_RTC_LIB
    if (!ensureRtc()) return;  // <-- ensures begin() ran; the old code checked
                               //     present() without it, so a first-run setUtc
                               //     silently skipped the chip.
    int y, mo, d, hh, mm;
    utcParts(t, y, mo, d, hh, mm);
    Rtc::DateTime dt;
    dt.year = (uint16_t)y;
    dt.month = (uint8_t)mo;
    dt.day = (uint8_t)d;
    dt.hour = (uint8_t)hh;
    dt.minute = (uint8_t)mm;
    dt.second = (uint8_t)(((t % 60) + 60) % 60);
    dt.weekday = (uint8_t)sky_dowSakamoto(y, mo, d);  // 0 = Sunday
    rtc().set(dt);
    systemSeeded() = true;
#endif
  }

  static time_t lastKnownUtc() {
    Preferences p;
    p.begin("almanac", true);
    const uint32_t v = p.getUInt("lastutc", 0);
    p.end();
    return (time_t)v;
  }

  // --- civil <-> epoch (Howard Hinnant; no timegm dependency) --------------
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
    long days = (long)(t / 86400), rem = (long)(t % 86400);
    if (rem < 0) { rem += 86400; days -= 1; }
    civilFromDays(days, y, mo, d);
    hh = (int)(rem / 3600);
    mm = (int)((rem % 3600) / 60);
  }

  static int localPartsMin(time_t utc, int& y, int& mo, int& d, int& hh, int& mm) {
    Location::begin();
    int uy, umo, ud, uh, um;
    utcParts(utc, uy, umo, ud, uh, um);
    int off = Location::utcOffsetMinutes(uy, umo, ud, uh);
    utcParts(utc + (long)off * 60L, y, mo, d, hh, mm);
    const int off2 = Location::utcOffsetMinutes(y, mo, d, hh);
    if (off2 != off) { off = off2; utcParts(utc + (long)off * 60L, y, mo, d, hh, mm); }
    return off;
  }
  static time_t epochFromLocalParts(int y, int mo, int d, int hh, int mm) {
    Location::begin();
    const int off = Location::utcOffsetMinutes(y, mo, d, hh);
    return epochFromUtcParts(y, mo, d, hh, mm) - (long)off * 60L;
  }

 private:
#ifdef ALMANAC_HAS_RTC_LIB
  static Rtc& rtc() { static Rtc r; return r; }
  // Try begin() until it takes. Cheap once begun_ is set inside the library;
  // before that, retrying is what lets an X3 come good after the runtime swap.
  static bool ensureRtc() {
    static bool ok = false;
    if (ok) return true;
    ok = rtc().begin();
    return ok;
  }
#endif
  static bool& systemSeeded() { static bool b = false; return b; }
  static void setSystemClock(time_t t) { timeval tv{}; tv.tv_sec = t; settimeofday(&tv, nullptr); }
  static void rememberUtc(time_t t) {
    Preferences p; p.begin("almanac", false); p.putUInt("lastutc", (uint32_t)t); p.end();
  }
};
