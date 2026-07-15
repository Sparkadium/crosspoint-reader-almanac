#pragma once
//
// CalcActivity.h — graphing calculator for the Almanac (Xteink X4/X3).
//
// Six function slots plot together on a Y= screen, TI style: hold Confirm on
// the graph (or take the first menu item) to open the function list, pick a
// slot, and type on the stock KeyboardEntryActivity (its shift/symbol layers
// already carry + - * / ^ ( )). Each expression is compiled once by
// calc_engine.h and evaluated per pixel column. Line styles repeat past three
// slots, so every curve also gets a small numeric tag drawn beside it — the
// only identity that really survives 1-bit e-ink.
//
// calc_engine.h is display-agnostic pure logic, verified on the host against
// Python's math library by tools/almanac/calc_test.cpp — the same
// arrangement as sky_math.h and astropy. If the engine ever disagrees with
// that harness, the engine is wrong.
//
// Interaction: Confirm cycles Pan -> Zoom -> Trace; hold Confirm opens the
// function list. The D-pad pans the view, zooms it, or walks a cursor along a
// curve. Back returns to Pan first and exits from there; hold Back opens the
// menu (functions, evaluate an expression, zoom presets, degrees/radians,
// grid).
//
// The view and the expressions persist in NVS. Expressions are written when
// edited; the viewport only in onExit(), so holding a pan button repaints the
// screen, not the flash.
//
#include <Preferences.h>

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "calc_engine.h"
#include "util/ButtonNavigator.h"

class CalcActivity final : public Activity {
 public:
  CalcActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calculator", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Mode : uint8_t { PAN, ZOOM, TRACE, FUNCS, MENU };
  static constexpr int SLOTS = 6;
  static constexpr uint16_t LONG_PRESS_MS = 700;
  static constexpr float ZOOM_STEP = 1.5f;
  static constexpr float MIN_SPAN = 1e-4f;
  static constexpr float MAX_SPAN = 1e7f;

  // ---- view / state ----
  void zoomBoth(float k);
  void zoomX(float k);
  void pan(float dxFrac, float dyFrac);
  void zoomStandard();  // x -10..10, y matched to the pixel aspect
  void zoomTrig();      // x -2pi..2pi, y -2..2
  void squareScale();   // keep x, rescale y so one unit is square
  int firstActiveSlot() const;
  int cycleSlot(int from, int dir) const;

  // ---- persistence ----
  void loadState();
  void saveExpr(int slot);
  void saveView();

  // ---- edit / evaluate ----
  void editSlot(int slot);
  void calcExpression();

  // ---- UI ----
  void openMenu();
  void runMenuItem(int item);
  void plotArea(int& x0, int& y0, int& w, int& h) const;
  float niceStep(float span, int target) const;
  void drawCurve(int s, int px, int py, int pw, int ph);
  void drawPlot();
  void drawFuncs();
  void drawMenu();

  Preferences prefs;
  ButtonNavigator nav_;

  CalcExpr fx_[SLOTS];
  std::string src_[SLOTS];  // raw text, kept even when compile fails so the
                            // user reopens the editor on what they typed
  float xmin_ = -10, xmax_ = 10, ymin_ = -10, ymax_ = 10;
  bool degrees_ = false;
  bool grid_ = true;

  Mode mode = PAN;
  int menuSel = 0;
  int funcSel = 0;
  int traceCol = 0;   // pixel column within the plot area
  int traceSlot = 0;

  std::string status_;

  // Act on a release only if this activity saw the press; a release with no
  // matching press is left over from a child activity that just closed.
  bool backHeld = false, backLong = false;
  bool confirmHeld = false, confirmLong = false;
};
