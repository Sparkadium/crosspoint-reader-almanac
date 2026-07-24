#pragma once
//
// PlanetariumActivity.h — pretty spheres for the Almanac (Xteink X4/X3).
//
// A lean gallery of photographic globes: the Sun, planets, Earth (day,
// night, clouds), and the Moon, each an FS-dithered orthographic sphere you
// spin with the D-pad. No databases, no gazetteer, no find list — this
// activity deliberately owns NOTHING except the texture machinery, so the
// photo allocations land on the same fresh-entry heap that made the Moon's
// photo mode work where the Globe's could not.
//
// Textures are TEX1 files on the SD card (tools/almanac/make_texture.py,
// from Solar System Scope imagery, CC BY 4.0). A body with no file shows a
// polite note and the list moves on. Earth night overlays the Natural Earth
// coastlines/borders in white IF /globe.bin is present — borrowed art, not
// a dependency. The Sun opens rotated to its real Carrington longitude when
// the clock is set: the face you would see today, give or take a sunspot.
//
// Buttons: D-pad spins | Confirm next body | hold Confirm home view |
//          Back exits | hold Back menu (pick body, info, crosshair)
//
#include <cstdint>

#include <Preferences.h>

#include "HalStorage.h"
#include "activities/Activity.h"
#include "globe_math.h"
#include "util/ButtonNavigator.h"

namespace tex { struct Luts; struct ChunkedBuf; }

class PlanetariumActivity final : public Activity {
 public:
  PlanetariumActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Planetarium", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  static constexpr int N_BODIES = 12;  // public: the body table in the .cpp
                                       // (anonymous namespace) sizes on it

 private:
  static constexpr uint16_t LONG_PRESS_MS = 700;
  static constexpr float SPIN_DEG = 12.0f;

  enum Mode : uint8_t { VIEW, MENU_M };

  int radius() const;
  void goHome();
  bool ensureBody();
  void freePhoto();
  void drawBody(int cx, int cy, int R);
  void drawMenu();
  void runMenuItem(int item);

  Mode mode_ = VIEW;
  int menuSel_ = 0;
  uint8_t body_ = 0;          // index into BODIES, persisted
  uint8_t loadedBody_ = 255;  // which body the resident buffer holds
  bool info_ = true;
  bool cross_ = false;        // a gallery wants clean discs by default
  bool timeSet_ = false;
  float viewLat_ = 0, viewLon_ = 0;
  float homeLon_ = 0;         // Carrington longitude for the Sun, else 0

  Preferences prefs_;
  tex::ChunkedBuf* texBuf_ = nullptr;
  tex::Luts* texLuts_ = nullptr;
  int16_t* fsErr_ = nullptr;
  bool texOk_ = false;

  ButtonNavigator nav_;
  bool backHeld = false, backLong_ = false;
  bool confirmHeld = false, confirmLong_ = false;
  bool needFull_ = true;
};
