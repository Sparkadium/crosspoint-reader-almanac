// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "CalcActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <variant>

#include "MappedInputManager.h"
#include "MathEntryActivity.h"
#include "ListLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SMALL = SMALL_FONT_ID;
constexpr bool BLACK = true;

constexpr float PI_F = 3.14159265358979323846f;

// %g, but never wider than the caller's buffer intends: tick labels and trace
// readouts both want "short and honest".
void fmtNum(char* out, size_t n, float v, int sig = 4) {
  char f[8];
  snprintf(f, sizeof(f), "%%.%dg", sig);
  snprintf(out, n, f, (double)v);
}

const char* MODE_NAMES[] = {"Pan", "Zoom", "Trace"};
}  // namespace

// ===========================================================================
//  View
// ===========================================================================

void CalcActivity::zoomBoth(float k) {
  const float cx = (xmin_ + xmax_) / 2, cy = (ymin_ + ymax_) / 2;
  float hx = (xmax_ - xmin_) / 2 * k, hy = (ymax_ - ymin_) / 2 * k;
  hx = std::min(std::max(hx, MIN_SPAN / 2), MAX_SPAN / 2);
  hy = std::min(std::max(hy, MIN_SPAN / 2), MAX_SPAN / 2);
  xmin_ = cx - hx; xmax_ = cx + hx;
  ymin_ = cy - hy; ymax_ = cy + hy;
}

void CalcActivity::zoomX(float k) {
  const float cx = (xmin_ + xmax_) / 2;
  float hx = (xmax_ - xmin_) / 2 * k;
  hx = std::min(std::max(hx, MIN_SPAN / 2), MAX_SPAN / 2);
  xmin_ = cx - hx; xmax_ = cx + hx;
}

void CalcActivity::pan(float dxFrac, float dyFrac) {
  const float dx = (xmax_ - xmin_) * dxFrac, dy = (ymax_ - ymin_) * dyFrac;
  xmin_ += dx; xmax_ += dx;
  ymin_ += dy; ymax_ += dy;
}

void CalcActivity::zoomStandard() {
  int px, py, pw, ph;
  plotArea(px, py, pw, ph);
  xmin_ = -10; xmax_ = 10;
  const float hy = 10.0f * ph / std::max(1, pw);  // square units
  ymin_ = -hy; ymax_ = hy;
}

void CalcActivity::zoomTrig() {
  xmin_ = -2 * PI_F; xmax_ = 2 * PI_F;
  ymin_ = -2; ymax_ = 2;
}

void CalcActivity::squareScale() {
  int px, py, pw, ph;
  plotArea(px, py, pw, ph);
  const float cy = (ymin_ + ymax_) / 2;
  const float hy = (xmax_ - xmin_) / 2 * ph / std::max(1, pw);
  ymin_ = cy - hy; ymax_ = cy + hy;
}

int CalcActivity::firstActiveSlot() const {
  for (int i = 0; i < SLOTS; i++)
    if (fx_[i].ok()) return i;
  return -1;
}

int CalcActivity::cycleSlot(int from, int dir) const {
  for (int k = 1; k <= SLOTS; k++) {
    const int i = (from + dir * k % SLOTS + SLOTS) % SLOTS;
    if (fx_[i].ok()) return i;
  }
  return from;
}

// ===========================================================================
//  Persistence
// ===========================================================================

void CalcActivity::loadState() {
  static const char* KEYS[SLOTS] = {"ex0", "ex1", "ex2", "ex3", "ex4", "ex5"};
  // First run: land on something plotted, not a blank plane.
  src_[0] = prefs.getString(KEYS[0], "sin(x)").c_str();
  for (int i = 1; i < SLOTS; i++) src_[i] = prefs.getString(KEYS[i], "").c_str();
  for (int i = 0; i < SLOTS; i++) fx_[i].compile(src_[i].c_str());

  xmin_ = prefs.getFloat("xmin", -10);
  xmax_ = prefs.getFloat("xmax", 10);
  ymin_ = prefs.getFloat("ymin", -10);
  ymax_ = prefs.getFloat("ymax", 10);
  if (!(xmax_ - xmin_ > 0) || !(ymax_ - ymin_ > 0) ||
      !std::isfinite(xmin_) || !std::isfinite(xmax_) ||
      !std::isfinite(ymin_) || !std::isfinite(ymax_)) {
    xmin_ = -10; xmax_ = 10; ymin_ = -10; ymax_ = 10;  // corrupt NVS: reset
  }
  degrees_ = prefs.getUChar("deg", 0) != 0;
  grid_ = prefs.getUChar("grid", 1) != 0;
}

void CalcActivity::saveExpr(int slot) {
  static const char* KEYS[SLOTS] = {"ex0", "ex1", "ex2", "ex3", "ex4", "ex5"};
  prefs.putString(KEYS[slot], src_[slot].c_str());
}

void CalcActivity::saveView() {
  prefs.putFloat("xmin", xmin_);
  prefs.putFloat("xmax", xmax_);
  prefs.putFloat("ymin", ymin_);
  prefs.putFloat("ymax", ymax_);
  prefs.putUChar("deg", degrees_ ? 1 : 0);
  prefs.putUChar("grid", grid_ ? 1 : 0);
}

// ===========================================================================
//  Lifecycle
// ===========================================================================

void CalcActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);
  backHeld = backLong = confirmHeld = false;
  prefs.begin("calc");
  loadState();
  int px, py, pw, ph;
  plotArea(px, py, pw, ph);
  traceCol = pw / 2;
  const int a = firstActiveSlot();
  traceSlot = a < 0 ? 0 : a;
  requestUpdate();
}

void CalcActivity::onExit() {
  Activity::onExit();
  saveView();
  prefs.end();
}

// ===========================================================================
//  Edit / evaluate
// ===========================================================================

void CalcActivity::editSlot(int slot) {
  auto handler = [this, slot](const ActivityResult& res) {
    // The keyboard consumed the press; its trailing release arrives without a
    // matching press and is discarded by the press/release matching in loop().
    backHeld = backLong = confirmHeld = false;
    if (!res.isCancelled) {
      const auto* kr = std::get_if<KeyboardResult>(&res.data);
      if (kr) {
        src_[slot] = kr->text;
        fx_[slot].compile(src_[slot].c_str());
        saveExpr(slot);
        if (fx_[slot].error()) {
          char b[96];
          snprintf(b, sizeof(b), "f%d: %s (at %d)", slot + 1, fx_[slot].error(),
                   fx_[slot].errorPos() + 1);
          status_ = b;
        } else {
          status_.clear();
          if (fx_[slot].ok()) traceSlot = slot;
        }
      }
    }
    mode = FUNCS;  // back to the Y= list, ready for the next slot
    requestUpdate(true);
  };
  char title[16];
  snprintf(title, sizeof(title), "f%d(x) =", slot + 1);
  startActivityForResult(
      std::make_unique<MathEntryActivity>(renderer, mappedInput, title, src_[slot], 64), handler);
}

void CalcActivity::calcExpression() {
  auto handler = [this](const ActivityResult& res) {
    backHeld = backLong = confirmHeld = false;
    if (!res.isCancelled) {
      const auto* kr = std::get_if<KeyboardResult>(&res.data);
      if (kr && !kr->text.empty()) {
        CalcExpr e;
        e.compile(kr->text.c_str());
        char b[96];
        if (e.error()) {
          snprintf(b, sizeof(b), "%s (at %d)", e.error(), e.errorPos() + 1);
        } else if (e.usesX()) {
          snprintf(b, sizeof(b), "Has x in it - put it in a slot to plot it");
        } else {
          char v[24];
          fmtNum(v, sizeof(v), e.eval(0, degrees_), 7);
          snprintf(b, sizeof(b), "= %s", v);
        }
        status_ = b;
      }
    }
    mode = PAN;
    requestUpdate(true);
  };
  startActivityForResult(
      std::make_unique<MathEntryActivity>(renderer, mappedInput, "Calculate", "", 64), handler);
}

// ===========================================================================
//  Menu
// ===========================================================================

namespace {
constexpr int MI_FUNCS = 0, MI_CALC = 1, MI_ZSTD = 2, MI_ZTRIG = 3, MI_ZSQ = 4,
              MI_ANGLE = 5, MI_GRID = 6, MI_CLEAR = 7;
constexpr int MENU_COUNT = 8;
}  // namespace

void CalcActivity::openMenu() {
  mode = MENU;
  menuSel = 0;
}

void CalcActivity::runMenuItem(int item) {
  switch (item) {
    case MI_FUNCS: mode = FUNCS; funcSel = 0; return;
    case MI_CALC: calcExpression(); return;
    case MI_ZSTD: zoomStandard(); break;
    case MI_ZTRIG: zoomTrig(); break;
    case MI_ZSQ: squareScale(); break;
    case MI_ANGLE:
      degrees_ = !degrees_;
      status_ = degrees_ ? "Trig in degrees" : "Trig in radians";
      break;
    case MI_GRID: grid_ = !grid_; break;
    case MI_CLEAR:
      for (int i = 0; i < SLOTS; i++) {
        src_[i].clear();
        fx_[i].clear();
        saveExpr(i);
      }
      status_ = "Functions cleared";
      break;
    default: break;
  }
  mode = PAN;
}

// ===========================================================================
//  Input
// ===========================================================================

void CalcActivity::loop() {
  // ---- Back: hold = menu; tap = back out one layer ------------------------
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { backHeld = true; backLong = false; }
  if (backHeld && !backLong && mode != MENU && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    backLong = true;
    openMenu();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!backHeld) return;  // leftover from a child
    const bool wasLong = backLong;
    backHeld = backLong = false;
    if (wasLong) return;
    status_.clear();
    switch (mode) {
      case MENU:
      case FUNCS:
      case ZOOM:
      case TRACE: mode = PAN; requestUpdate(); return;
      default: finish(); return;
    }
  }

  // ---- menu and the Y= function list ---------------------------------------
  if (mode == MENU || mode == FUNCS) {
    const int count = (mode == MENU) ? MENU_COUNT : SLOTS;
    int& sel = (mode == MENU) ? menuSel : funcSel;
    bool moved = false;
    nav_.onNext([&] { sel = ButtonNavigator::nextIndex(sel, count); moved = true; });
    nav_.onPrevious([&] { sel = ButtonNavigator::previousIndex(sel, count); moved = true; });
    if (moved) { requestUpdate(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHeld = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!confirmHeld) return;  // leftover from a child
      confirmHeld = false;
      if (mode == MENU) runMenuItem(menuSel);
      else editSlot(funcSel);
      requestUpdate();
    }
    return;
  }

  // ---- graph: D-pad meaning depends on the mode ----------------------------
  bool moved = false;
  int px, py, pw, ph;
  plotArea(px, py, pw, ph);

  switch (mode) {
    case PAN:
      nav_.onPressAndContinuous({MappedInputManager::Button::Right}, [&] { pan(+1.0f / 6, 0); moved = true; });
      nav_.onPressAndContinuous({MappedInputManager::Button::Left}, [&] { pan(-1.0f / 6, 0); moved = true; });
      nav_.onPressAndContinuous({MappedInputManager::Button::Up}, [&] { pan(0, +1.0f / 6); moved = true; });
      nav_.onPressAndContinuous({MappedInputManager::Button::Down}, [&] { pan(0, -1.0f / 6); moved = true; });
      break;
    case ZOOM:
      nav_.onPressAndContinuous({MappedInputManager::Button::Up}, [&] { zoomBoth(1.0f / ZOOM_STEP); moved = true; });
      nav_.onPressAndContinuous({MappedInputManager::Button::Down}, [&] { zoomBoth(ZOOM_STEP); moved = true; });
      nav_.onPressAndContinuous({MappedInputManager::Button::Right}, [&] { zoomX(1.0f / ZOOM_STEP); moved = true; });
      nav_.onPressAndContinuous({MappedInputManager::Button::Left}, [&] { zoomX(ZOOM_STEP); moved = true; });
      break;
    case TRACE: {
      const unsigned long held = mappedInput.getHeldTime();
      const int step = held > 3000 ? 8 : (held > 1200 ? 3 : 1);
      nav_.onPressAndContinuous({MappedInputManager::Button::Right},
                                [&] { traceCol = std::min(traceCol + step, pw - 1); moved = true; });
      nav_.onPressAndContinuous({MappedInputManager::Button::Left},
                                [&] { traceCol = std::max(traceCol - step, 0); moved = true; });
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) { traceSlot = cycleSlot(traceSlot, +1); moved = true; }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) { traceSlot = cycleSlot(traceSlot, -1); moved = true; }
      break;
    }
    default: break;
  }
  if (moved) { requestUpdate(); return; }

  // ---- Confirm: tap cycles Pan -> Zoom -> Trace, hold opens the Y= list ----
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) { confirmHeld = true; confirmLong = false; }
  if (confirmHeld && !confirmLong && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    confirmLong = true;
    status_.clear();
    mode = FUNCS;
    funcSel = 0;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!confirmHeld) return;  // leftover from a child
    const bool wasLong = confirmLong;
    confirmHeld = confirmLong = false;
    if (wasLong) return;
    status_.clear();
    mode = mode == PAN ? ZOOM : (mode == ZOOM ? TRACE : PAN);
    if (mode == TRACE && firstActiveSlot() < 0) mode = PAN;  // nothing to trace
    requestUpdate();
  }
}

// ===========================================================================
//  Rendering
// ===========================================================================

// The plot rect: left gutter carries y labels, bottom strip carries x labels,
// so the labels never sit on top of a curve.
void CalcActivity::plotArea(int& x0, int& y0, int& w, int& h) const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int gutterL = renderer.getTextWidth(SMALL, "-8888") + 8;
  const int stripB = renderer.getLineHeight(SMALL) + 4;
  x0 = m.contentSidePadding + gutterL;
  y0 = m.topPadding + m.headerHeight + m.verticalSpacing + 6;
  const int statusH = renderer.getLineHeight(SMALL) + 6;
  w = renderer.getScreenWidth() - m.contentSidePadding - x0 - 2;
  h = renderer.getScreenHeight() - m.buttonHintsHeight - m.verticalSpacing - statusH - stripB - y0;
}

// Tick spacing: largest of 1/2/5 x 10^k that yields at least `target` ticks.
float CalcActivity::niceStep(float span, int target) const {
  const float raw = span / target;
  const float mag = powf(10.0f, floorf(log10f(raw)));
  const float r = raw / mag;
  return (r <= 1 ? 1.0f : (r <= 2 ? 2.0f : (r <= 5 ? 5.0f : 10.0f))) * mag;
}

// Line styles cannot carry six identities on 1-bit e-ink, so beyond
// thin / thick / dashed they repeat — and every curve gets a small numeric tag
// beside it instead, at a column staggered per slot so tags rarely collide.
void CalcActivity::drawCurve(int s, int px, int py, int pw, int ph) {
  const float top = (float)py, bot = (float)(py + ph - 1);
  const bool thick = (s % 3) == 1;
  const bool dashed = (s % 3) == 2;
  auto toPy = [&](float y) { return py + (ymax_ - y) * ph / (ymax_ - ymin_); };

  float prevY = NAN;
  for (int col = 0; col < pw; col++) {
    const float x = xmin_ + (col + 0.5f) * (xmax_ - xmin_) / pw;
    const float y = fx_[s].eval(x, degrees_);
    const float Y = std::isfinite(y) ? toPy(y) : NAN;

    if (col > 0 && std::isfinite(Y) && std::isfinite(prevY)) {
      float y0 = prevY, y1 = Y;
      // An asymptote shows up as a segment that enters from above the plot
      // and leaves below it (or vice versa) within one column: draw nothing.
      const bool jump = (y0 < top && y1 > bot) || (y1 < top && y0 > bot);
      if (!jump && !(y0 < top && y1 < top) && !(y0 > bot && y1 > bot)) {
        const float x0f = (float)(px + col - 1), x1f = (float)(px + col);
        float cx0 = x0f, cx1 = x1f, cy0 = y0, cy1 = y1;
        // clip the segment's y to the plot rect (x is already inside)
        auto clipEnd = [&](float& cx, float& cy, float ox, float oy) {
          if (cy < top) { cx = ox + (cx - ox) * (top - oy) / (cy - oy); cy = top; }
          if (cy > bot) { cx = ox + (cx - ox) * (bot - oy) / (cy - oy); cy = bot; }
        };
        clipEnd(cx0, cy0, x1f, y1);
        clipEnd(cx1, cy1, x0f, y0);
        if (!dashed || ((col >> 2) & 1) == 0) {
          if (thick)
            renderer.drawLine((int)lroundf(cx0), (int)lroundf(cy0), (int)lroundf(cx1),
                              (int)lroundf(cy1), 3, BLACK);
          else
            renderer.drawLine((int)lroundf(cx0), (int)lroundf(cy0), (int)lroundf(cx1),
                              (int)lroundf(cy1), BLACK);
        }
      }
    }
    prevY = Y;
  }

  // numeric tag: at a slot-staggered column, boxed in white for legibility
  const int tagCol = pw * (s + 1) / (SLOTS + 1);
  const float tx = xmin_ + (tagCol + 0.5f) * (xmax_ - xmin_) / pw;
  const float ty = fx_[s].eval(tx, degrees_);
  if (std::isfinite(ty)) {
    const int Y = (int)lroundf(toPy(ty));
    if (Y >= py && Y < py + ph) {
      char t[4];
      snprintf(t, sizeof(t), "%d", s + 1);
      const int tw = renderer.getTextWidth(SMALL, t);
      const int lh = renderer.getLineHeight(SMALL);
      int lx = px + tagCol + 4, ly = Y - lh - 3;
      lx = std::min(lx, px + pw - tw - 3);
      ly = std::max(ly, py + 1);
      renderer.fillRect(lx - 2, ly - 1, tw + 4, lh + 2, !BLACK);
      renderer.drawRect(lx - 2, ly - 1, tw + 4, lh + 2, BLACK);
      renderer.drawText(SMALL, lx, ly, t);
    }
  }
}

void CalcActivity::drawPlot() {
  int px, py, pw, ph;
  plotArea(px, py, pw, ph);
  const float sx = pw / (xmax_ - xmin_);
  const float sy = ph / (ymax_ - ymin_);

  auto toPy = [&](float y) { return py + (ymax_ - y) * sy; };
  auto toPx = [&](float x) { return px + (x - xmin_) * sx; };

  // -- grid and ticks --------------------------------------------------------
  const float xstep = niceStep(xmax_ - xmin_, 5);
  const float ystep = niceStep(ymax_ - ymin_, 5);
  char lbl[16];

  for (float gx = ceilf(xmin_ / xstep) * xstep; gx <= xmax_; gx += xstep) {
    const int X = (int)lroundf(toPx(gx));
    if (X < px || X >= px + pw) continue;
    if (grid_)
      for (int Y = py; Y < py + ph; Y += 4) renderer.drawPixel(X, Y, BLACK);
    fmtNum(lbl, sizeof(lbl), fabsf(gx) < xstep * 1e-4f ? 0.0f : gx);
    const int tw = renderer.getTextWidth(SMALL, lbl);
    renderer.drawText(SMALL, std::min(std::max(X - tw / 2, px), px + pw - tw), py + ph + 3, lbl);
    renderer.drawLine(X, py + ph - 4, X, py + ph, BLACK);  // tick on the frame
  }
  for (float gy = ceilf(ymin_ / ystep) * ystep; gy <= ymax_; gy += ystep) {
    const int Y = (int)lroundf(toPy(gy));
    if (Y < py || Y >= py + ph) continue;
    if (grid_)
      for (int X = px; X < px + pw; X += 4) renderer.drawPixel(X, Y, BLACK);
    fmtNum(lbl, sizeof(lbl), fabsf(gy) < ystep * 1e-4f ? 0.0f : gy);
    const int tw = renderer.getTextWidth(SMALL, lbl);
    renderer.drawText(SMALL, px - tw - 5, Y - renderer.getLineHeight(SMALL) / 2, lbl);
    renderer.drawLine(px, Y, px + 4, Y, BLACK);
  }

  // -- axes, when in view ----------------------------------------------------
  if (ymin_ < 0 && 0 < ymax_) {
    const int Y = (int)lroundf(toPy(0));
    renderer.drawLine(px, Y, px + pw - 1, Y, BLACK);
  }
  if (xmin_ < 0 && 0 < xmax_) {
    const int X = (int)lroundf(toPx(0));
    renderer.drawLine(X, py, X, py + ph - 1, BLACK);
  }

  // -- curves ------------------------------------------------------------
  for (int s = 0; s < SLOTS; s++)
    if (fx_[s].ok()) drawCurve(s, px, py, pw, ph);

  // -- trace cursor ------------------------------------------------------
  if (mode == TRACE && fx_[traceSlot].ok()) {
    const float x = xmin_ + (traceCol + 0.5f) * (xmax_ - xmin_) / pw;
    const float y = fx_[traceSlot].eval(x, degrees_);
    const int X = px + traceCol;
    renderer.drawLine(X, py, X, py + ph - 1, BLACK);  // hairline column marker
    if (std::isfinite(y)) {
      const int Y = (int)lroundf(toPy(y));
      if (Y >= py && Y < py + ph) {
        renderer.drawRect(X - 4, Y - 4, 9, 9, 2, BLACK);
      }
    }
    char xs[20], ys[20], b[72];
    fmtNum(xs, sizeof(xs), x, 5);
    if (std::isfinite(y)) fmtNum(ys, sizeof(ys), y, 5);
    else snprintf(ys, sizeof(ys), "undefined");
    snprintf(b, sizeof(b), "f%d(%s) = %s", traceSlot + 1, xs, ys);
    status_ = b;
  }

  renderer.drawRect(px - 1, py - 1, pw + 2, ph + 2, BLACK);  // frame
}

void CalcActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  if (mode == MENU) { drawMenu(); return; }
  if (mode == FUNCS) { drawFuncs(); return; }

  char title[48];
  snprintf(title, sizeof(title), "Calculator   %s   %s", MODE_NAMES[mode],
           degrees_ ? "deg" : "rad");
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, title);

  drawPlot();  // may set status_ (trace readout)

  const int statusY = pageH - m.buttonHintsHeight - m.verticalSpacing - renderer.getLineHeight(SMALL) - 4;
  if (!status_.empty()) {
    renderer.drawCenteredText(SMALL, statusY, status_.c_str());
  } else {
    const char* hint;
    switch (mode) {
      case ZOOM: hint = "Up in - Down out - Right/Left stretch x"; break;
      case TRACE: hint = "Left/Right walk - Up/Down switch f"; break;
      default:
        hint = firstActiveSlot() < 0 ? "hold Confirm to enter functions"
                                     : "hold Confirm: functions - hold Back: menu";
        break;
    }
    renderer.drawCenteredText(SMALL, statusY, hint);
  }

  const char* confirmLabel = mode == PAN ? "Zoom" : (mode == ZOOM ? "Trace" : "Pan");
  const char* backLabel = mode == PAN ? "Back" : "Pan";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "Move", "Move");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void CalcActivity::drawMenu() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Calculator menu");

  const char* labels[MENU_COUNT];
  const char* subs[MENU_COUNT] = {nullptr};
  char funcsSub[24];
  int active = 0;
  for (int i = 0; i < SLOTS; i++)
    if (fx_[i].ok()) active++;
  snprintf(funcsSub, sizeof(funcsSub), "%d of %d in use", active, SLOTS);
  labels[MI_FUNCS] = "Functions";
  subs[MI_FUNCS] = funcsSub;
  labels[MI_CALC] = "Calculate expression";
  labels[MI_ZSTD] = "Zoom standard";
  subs[MI_ZSTD] = "x -10 to 10, square units";
  labels[MI_ZTRIG] = "Zoom trig";
  subs[MI_ZTRIG] = "x -2pi to 2pi, y -2 to 2";
  labels[MI_ZSQ] = "Square scale";
  subs[MI_ZSQ] = "rescale y so circles are round";
  labels[MI_ANGLE] = degrees_ ? "Switch to radians" : "Switch to degrees";
  labels[MI_GRID] = grid_ ? "Grid off" : "Grid on";
  labels[MI_CLEAR] = "Clear all functions";

  const ListLayout L = computeListLayout(renderer, MENU_COUNT, menuSel, /*wantBlurb=*/true);
  const int pad = m.contentSidePadding;

  for (int k = 0; k < L.rowsPerPage; k++) {
    const int i = L.firstVisible + k;
    if (i >= MENU_COUNT) break;
    const bool sel = (i == menuSel);
    if (sel) renderer.fillRect(pad - 6, L.rowTop(k), pageW - (pad - 6) * 2, L.boxH, true);
    renderer.drawText(L.titleFont, pad + 4, L.nameTop(k), labels[i], !sel);
    if (L.withBlurb && subs[i]) renderer.drawText(L.subFont, pad + 4, L.blurbTop(k), subs[i], !sel);
  }

  if (L.scrolls(MENU_COUNT)) {
    char pos[24];
    snprintf(pos, sizeof(pos), "%d of %d", menuSel + 1, MENU_COUNT);
    renderer.drawCenteredText(L.subFont, L.bottom + 4, pos);
  }

  const auto labelsBtn = mappedInput.mapLabels("Close", "Choose", "Up", "Down");
  GUI.drawButtonHints(renderer, labelsBtn.btn1, labelsBtn.btn2, labelsBtn.btn3, labelsBtn.btn4);
  renderer.displayBuffer();
}

// The Y= screen. Six rows, expression (or its compile error) as the blurb;
// Confirm opens the keyboard on the selected slot, Back returns to the graph.
void CalcActivity::drawFuncs() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Functions");

  char lbl[SLOTS][12], sub[SLOTS][64];
  for (int i = 0; i < SLOTS; i++) {
    const char* style = (i % 3) == 1 ? "thick" : ((i % 3) == 2 ? "dashed" : "thin");
    snprintf(lbl[i], sizeof(lbl[i]), "f%d(x)", i + 1);
    if (fx_[i].error())
      snprintf(sub[i], sizeof(sub[i]), "%s  (%s)", src_[i].c_str(), fx_[i].error());
    else if (fx_[i].ok())
      snprintf(sub[i], sizeof(sub[i]), "= %s   [%s]", src_[i].c_str(), style);
    else
      snprintf(sub[i], sizeof(sub[i]), "empty");
  }

  const ListLayout L = computeListLayout(renderer, SLOTS, funcSel, /*wantBlurb=*/true);
  const int pad = m.contentSidePadding;

  for (int k = 0; k < L.rowsPerPage; k++) {
    const int i = L.firstVisible + k;
    if (i >= SLOTS) break;
    const bool sel = (i == funcSel);
    if (sel) renderer.fillRect(pad - 6, L.rowTop(k), pageW - (pad - 6) * 2, L.boxH, true);
    renderer.drawText(L.titleFont, pad + 4, L.nameTop(k), lbl[i], !sel);
    if (L.withBlurb) renderer.drawText(L.subFont, pad + 4, L.blurbTop(k), sub[i], !sel);
  }

  if (L.scrolls(SLOTS)) {
    char pos[24];
    snprintf(pos, sizeof(pos), "%d of %d", funcSel + 1, SLOTS);
    renderer.drawCenteredText(L.subFont, L.bottom + 4, pos);
  }

  const auto labels = mappedInput.mapLabels("Graph", "Edit", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
