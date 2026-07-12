#pragma once
//
// Location.h — where the user is, and what their clock does.
//
// The Sky Chart and the Clock both need this: the chart needs latitude and
// longitude to project the sky, and both need the UTC offset (with a DST rule)
// to turn the stored UTC epoch into local civil time.
//
// Source of truth is NVS. If nothing is stored yet and a /location.txt exists on
// the SD card, it is read once and saved, so a location can be provisioned by
// dropping a file on the card. After that, the on-device editor
// (Almanac -> Location) owns the value.
//
// /location.txt — one key=value per line, '#' starts a comment:
//
//     lat = 45.3475        degrees, north positive
//     lon = -75.7566       degrees, EAST positive (so Ottawa is negative)
//     utc = -300           STANDARD-time offset in minutes (-300 = UTC-5:00)
//     dst = na             none | na | eu
//
// DST rules implemented:
//   none — offset is constant
//   na   — US/Canada, post-2007: 02:00 local on the 2nd Sunday of March
//          until 02:00 local on the 1st Sunday of November
//   eu   — European Union: last Sunday of March until last Sunday of October
//          (the real rule switches at 01:00 UTC; this approximates in local
//          time, so the hour either side of the change may be off by an hour)
//
// Southern-hemisphere DST (Australia, NZ, Chile...) is NOT implemented. Choose
// "none" and set the offset for the season, or leave DST off. Everything else —
// latitude, longitude, and a fixed offset — works anywhere on Earth.
//
#include <Preferences.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>  // strcasecmp

#include "HalStorage.h"
#include "sky_math.h"  // sky_dowSakamoto

struct LocationConfig {
  double lat = 45.3475;      // Ottawa (Bells Corners) — the historical default
  double lon = -75.7566;     // east positive
  int32_t baseOffsetMin = -300;  // standard-time offset from UTC, in minutes
  uint8_t dstRule = 1;       // 0 none, 1 north america, 2 europe
};

class Location {
 public:
  // NOTE: do NOT call these DST_NONE / DST_USA / etc. <sys/time.h> (pulled in by
  // Arduino.h) already defines DST_NONE, DST_USA, DST_AUST, DST_WET, DST_MET,
  // DST_EET and DST_CAN as macros, and a macro will happily rewrite your
  // constant into "0 = 0".
  static constexpr uint8_t RULE_NONE = 0, RULE_NA = 1, RULE_EU = 2;

  static LocationConfig& get() {
    static LocationConfig cfg;
    return cfg;
  }

  static bool loaded() { return loadedFlag(); }

  // Call once per activity that needs a location.
  static void begin() {
    if (loadedFlag()) return;
    Preferences p;
    p.begin("almanac", true);
    const bool has = p.isKey("loc");
    if (has) {
      LocationConfig c;
      if (p.getBytes("loc", &c, sizeof(c)) == sizeof(c)) get() = c;
    }
    p.end();
    if (!has && readFile()) save();  // seed from the SD card, once
    loadedFlag() = true;
  }

  static void save() {
    Preferences p;
    p.begin("almanac", false);
    p.putBytes("loc", &get(), sizeof(LocationConfig));
    p.end();
  }

  static int daysInMonth(int y, int m) {
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
  }

  // Is daylight saving in effect for this LOCAL civil date/hour?
  static bool dstActive(int y, int mo, int d, int hourLocal) {
    switch (get().dstRule) {
      case RULE_NA: {
        if (mo < 3 || mo > 11) return false;
        if (mo > 3 && mo < 11) return true;
        if (mo == 3) {
          const int firstDow = sky_dowSakamoto(y, 3, 1);
          const int secondSun = 1 + ((7 - firstDow) % 7) + 7;
          return d > secondSun || (d == secondSun && hourLocal >= 2);
        }
        const int firstDow = sky_dowSakamoto(y, 11, 1);
        const int firstSun = 1 + ((7 - firstDow) % 7);
        return !(d > firstSun || (d == firstSun && hourLocal >= 2));
      }
      case RULE_EU: {
        if (mo < 3 || mo > 10) return false;
        if (mo > 3 && mo < 10) return true;
        if (mo == 3) {
          const int lastSun = lastSunday(y, 3);
          return d > lastSun || (d == lastSun && hourLocal >= 2);
        }
        const int lastSun = lastSunday(y, 10);
        return !(d > lastSun || (d == lastSun && hourLocal >= 3));
      }
      default:
        return false;
    }
  }

  // Offset from UTC, in minutes, for a LOCAL civil date/hour.
  static int utcOffsetMinutes(int y, int mo, int d, int hourLocal) {
    return get().baseOffsetMin + (dstActive(y, mo, d, hourLocal) ? 60 : 0);
  }

  // "UTC-05:00" / "UTC+05:45"
  static void offsetLabel(char* buf, size_t n, int offMin) {
    const char sign = offMin < 0 ? '-' : '+';
    const int a = abs(offMin);
    snprintf(buf, n, "UTC%c%02d:%02d", sign, a / 60, a % 60);
  }

  static const char* dstRuleName(uint8_t rule) {
    switch (rule) {
      case RULE_NA: return "North America";
      case RULE_EU: return "Europe";
      default: return "none";
    }
  }

 private:
  static bool& loadedFlag() {
    static bool f = false;
    return f;
  }
  static int lastSunday(int y, int m) {
    const int ld = daysInMonth(y, m);
    return ld - sky_dowSakamoto(y, m, ld);
  }

  // Optional one-time seed from the SD card.
  static bool readFile() {
    HalFile f = Storage.open("/location.txt", O_RDONLY);
    if (!f) return false;
    char buf[512];
    const int n = f.read(buf, sizeof(buf) - 1);
    f.close();
    if (n <= 0) return false;
    buf[n] = '\0';

    LocationConfig c = get();
    bool any = false;
    char* save = nullptr;
    for (char* line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(nullptr, "\r\n", &save)) {
      char* hash = strchr(line, '#');
      if (hash) *hash = '\0';
      char* eq = strchr(line, '=');
      if (!eq) continue;
      *eq = '\0';
      char* key = trim(line);
      char* val = trim(eq + 1);
      if (!*key || !*val) continue;

      if (!strcasecmp(key, "lat")) { c.lat = atof(val); any = true; }
      else if (!strcasecmp(key, "lon")) { c.lon = atof(val); any = true; }
      else if (!strcasecmp(key, "utc")) { c.baseOffsetMin = atoi(val); any = true; }
      else if (!strcasecmp(key, "dst")) {
        if (!strcasecmp(val, "na")) c.dstRule = RULE_NA;
        else if (!strcasecmp(val, "eu")) c.dstRule = RULE_EU;
        else c.dstRule = RULE_NONE;
        any = true;
      }
    }
    if (!any) return false;
    if (c.lat < -90 || c.lat > 90 || c.lon < -180 || c.lon > 180) return false;
    if (c.baseOffsetMin < -720 || c.baseOffsetMin > 840) return false;
    get() = c;
    return true;
  }
  static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    return s;
  }
};
