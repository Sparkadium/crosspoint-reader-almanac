#pragma once
//
// MoonActivity.h — the Moon for the Almanac (Xteink X4/X3): an orthographic
// lunar globe with a fixed reticle, spun with the D-pad.
//
// The home view is the SUB-EARTH point — the exact face the Moon shows Earth
// right now, libration included, so tonight's tilt of Mare Crisium toward or
// away from the limb is real. The dark side is shaded from the real sub-solar
// point, so the terminator matches tonight's phase. Both come from
// moon_math.h (Meeus ch. 47/53 truncated), verified by tools/almanac/
// moon_test.cpp against JPL DE421's integrated librations: 501 dates,
// worst error 0.19 deg (~0.7 px).
//
// Named features come from /moon.bin (USGS/IAU Gazetteer of Planetary
// Nomenclature, built by tools/almanac/make_moon.py), streamed per render.
// No file: a clean phase globe, no complaint. The reticle names the nearest
// feature. If the clock is not set, the Moon of the fallback moment is shown
// and the info line says so.
//
// Rendering reuses globe_math.h wholesale: same view basis, same per-row
// terminator solver, same star-field space background idea (always on here —
// the Moon lives in space).
//
// Buttons: D-pad spins | Confirm cycles zoom | hold Confirm flies to the
//          sub-Earth view | Back exits | hold Back opens the menu (photo
//          view, terminator, features, graticule, info text, crosshair --
//          every piece of chrome toggles individually, so the Moon can be a
//          clean full-bleed photograph or a fully annotated chart)
//
#include <cstdint>

#include <Preferences.h>

#include "HalStorage.h"
#include "activities/Activity.h"
#include "globe_math.h"

#include "util/ButtonNavigator.h"

class MoonActivity final : public Activity {
 public:
  MoonActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Moon", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr uint16_t LONG_PRESS_MS = 700;
  static constexpr float SPIN_DEG = 12.0f;
  static constexpr int ZOOM_LEVELS = 3;
  static constexpr float MOON_RADIUS_KM = 1737.4f;

  enum Mode : uint8_t { MOON, MENU_M };

  int radius() const;
  void goSubEarth();
  void drawMoon(int cx, int cy, int R);
  void drawMenu();
  void runMenuItem(int item);

  HalFile mFile;
  uint16_t nFeat_ = 0;
  bool haveData_ = false;

  float viewLat_ = 0, viewLon_ = 0;
  int zoom_ = 0;
  float subELat_ = 0, subELon_ = 0;   // sub-Earth (home view)
  float subSLat_ = 0, subSLon_ = 0;   // sub-solar (terminator)
  float illum_ = 0;
  bool timeSet_ = false;

  char nearName_[44] = {0};           // nearest feature to the reticle
  float nearDist_ = 999;

  // Individually toggleable chrome and layers, NVS-persisted ("moon"
  // namespace). The disc's anchor never moves -- hiding chrome only lets the
  // space background run to the panel edges.
  Mode mode_ = MOON;
  int menuSel_ = 0;
  bool term_ = true;      // terminator (Bayer shade on map, solid on photo)
  bool features_ = true;  // IAU feature circles from /moon.bin
  bool grat_ = true;      // selenographic graticule
  bool info_ = true;      // header + info bar + hints
  bool cross_ = true;     // reticle crosshair
  Preferences prefs_;
  // e-ink hygiene: the dithered dark side shows ghost text from the previous
  // screen under FAST_REFRESH, so the first frame after entry or a chrome
  // toggle uses a FULL refresh, then fast refreshes for spins.
  bool needFull_ = true;

  ButtonNavigator nav_;
  // Act on a release only if this activity saw the press; a release with no
  // matching press is left over from the Almanac menu.
  bool backHeld = false, backLong_ = false;
  bool confirmHeld = false, confirmLong = false;
};
