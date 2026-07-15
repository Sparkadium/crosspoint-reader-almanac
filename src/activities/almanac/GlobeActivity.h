#pragma once
//
// GlobeActivity.h — a spinning Earth for the Almanac (Xteink X4/X3).
//
// Orthographic globe, fixed reticle at screen center: the D-pad rotates the
// PLANET under the reticle rather than moving a cursor over a map. The night
// hemisphere is shaded live from the sun's real position, so the terminator
// is where it actually is right now.
//
// Data: /globe.bin on the SD card — Natural Earth 1:110m coastlines (public
// domain) packed by tools/almanac/make_globe.py as PRE-COMPUTED unit vectors,
// so a full redraw is nine multiplications per point and zero per-point trig.
// The night side costs one small quadratic per screen ROW (globe_math.h
// nightSpan), never per pixel. globe_test.cpp verified format, projection,
// sun position, and the terminator solver on the host.
//
// The reticle is linked to the CIA World Factbook: countries.bin (Natural
// Earth admin-0 borders, public domain, built by make_countries.py) names the
// country under the reticle via streamed point-in-polygon (cty_math.h,
// host-verified), and Confirm opens its gazetteer.cdb entry -- read with the
// same WcdbReader the Factbook itself uses -- in an overlay.
//
// Buttons: D-pad spins | Confirm opens the Factbook entry under the reticle
//          hold Confirm cycles zoom | Back exits | hold Back opens the menu
//          (find a country, fly home, borders, night shading) | overlay:
//          Up/Down scroll, Back closes
//
#include <cstdint>

#include <Preferences.h>
#include <string>

#include "HalStorage.h"
#include "activities/Activity.h"
#include "activities/dictionary/DictionaryActivity.h"  // WcdbReader
#include "globe_math.h"
#include "util/ButtonNavigator.h"

class GlobeActivity final : public Activity {
 public:
  GlobeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Globe", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr uint16_t LONG_PRESS_MS = 700;
  static constexpr float SPIN_DEG = 12.0f;
  static constexpr int ZOOM_LEVELS = 3;

  enum Mode : uint8_t { GLOBE, ENTRY, MENU_M, FIND };

  int radius() const;
  int contentBottom() const;  // last drawable row: varies with info/space
  int layoutBottom() const;   // globe anchor: FIXED at the info-on position
  void goHome();
  void drawGlobe(int cx, int cy, int R);
  void resolveCountry();
  void openEntry();
  void drawEntry();
  void drawMenu();
  void runMenuItem(int item);
  void openFind();
  void drawFind();

  HalFile gFile;
  HalFile ctyFile_;
  WcdbReader gaz_;
  uint16_t nLines_ = 0;
  bool loadError_ = false;

  Mode mode_ = GLOBE;
  bool haveCountry_ = false;
  char countryKey_[32] = {0};
  char countryName_[44] = {0};
  std::string entryTitle_, entryText_;
  std::string status_;
  int scroll_ = 0;
  int menuSel_ = 0;
  // The find list is STREAMED from countries.bin per render, never held in
  // RAM: 205 entries x 84 bytes was a ~17KB contiguous allocation, and a
  // vector regrowth past its reserve asked the fragmented C3 heap for twice
  // that -- new fails, -fno-exceptions turns that into abort(). Only a
  // first-letter index (for the letter jumps) and the visible rows live here.
  static constexpr int MAX_FIND = 256;
  int findCount_ = 0;
  char findLetters_[MAX_FIND] = {0};
  int findSel_ = 0;
  bool borders_ = true;
  bool night_ = true;
  bool space_ = false;
  bool infoText_ = true;
  bool crosshair_ = true;
  uint8_t factFont_ = 1;  // 0 small, 1 medium, 2 large (see FACT_FONTS)
  Preferences prefs_;

  float viewLat_ = 45.0f, viewLon_ = -75.0f;
  int zoom_ = 0;
  float sunLat_ = 0, sunLon_ = 0;
  bool timeSet_ = false;

  ButtonNavigator nav_;
  // Act on a release only if this activity saw the press; a release with no
  // matching press is left over from a parent that just opened us.
  bool backHeld = false, backLong = false;
  bool needFull_ = true;  // FULL refresh on entry: clears menu ghosts under dither
  bool confirmHeld = false, confirmLong = false;
};
