#pragma once
//
// SkyActivity.h — all-sky star chart for the Xteink X4, ported from the
// WatchyAlmanac sky module (sky.h).
//
// Ports cleanly and untouched:
//   sky_math.h — pure C, verified against astropy. Copy in as-is.
//   stars.h    — 904 stars (Yale BSC, V<=4.5) + constellation lines, FK5 J2033.
//                PROGMEM/memcpy_P are valid no-ops on the ESP32, so this
//                generated file needs no edits. Provenance preserved.
//
// Rewritten for this device:
//   - drawing: GxEPD2 calls -> GfxRenderer (which has no circle primitives,
//     so fillCircle/drawCircle are hand-rolled in the .cpp)
//   - input: raw digitalRead() polling -> MappedInputManager
//   - layout: 200x200 (disk R=74) -> 480x800 (disk R~220). At 220ppi the disk
//     is ~8x the area of the watch's, so stars get real size classes.
//   - time: PCF8563 -> TimeSource (the X4 has no RTC; see TimeSource.h)
//   - location: the watch hardcoded Ottawa. Latitude, longitude, UTC offset and
//     DST rule now come from Location (Almanac -> Location), so the chart is
//     correct anywhere on Earth.
//
// Buttons: Back exit | Left -30min | Right +30min | Confirm back to now
//          Up +1 day | Down -1 day | hold Confirm: hide/show all text
//
#include <ctime>

#include "activities/Activity.h"

class SkyActivity final : public Activity {
 public:
  SkyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sky Chart", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Project an equatorial point to screen; false if below the horizon.
  bool project(double raDeg, double decDeg, double jd, int& px, int& py) const;
  void drawMoonGlyph(int cx, int cy, int r, int phase) const;

  time_t baseUtc_ = 0;   // "now" when the activity opened
  long offsetMin_ = 0;   // user time-travel offset
  bool timeSet_ = false;
  double lat_ = 0, lon_ = 0;  // copied from Location on entry

  // Act on a release only if this activity saw the press.
  bool sawBackPress_ = false;
  bool sawConfirmPress_ = false;
  bool confirmLong_ = false;

  // One switch for ALL chrome -- header, date line, moon/sun bar, hints, and
  // the cardinal letters -- leaving just the sky disc. Hold Confirm to
  // toggle; kept in NVS. The disc's position and size are computed the same
  // way in both states, so toggling never moves the chart.
  bool infoUi_ = true;

  // Geometry, computed in render() from the real screen size.
  mutable int cx_ = 240, cy_ = 330, radius_ = 210;
};
