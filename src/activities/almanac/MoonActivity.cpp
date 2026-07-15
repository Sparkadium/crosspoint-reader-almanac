// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "MoonActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include <Preferences.h>

#include "FallbackMoment.h"
#include "MappedInputManager.h"
#include "TimeSource.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "moon_math.h"

namespace {
constexpr int HEAD_FONT = UI_12_FONT_ID;
constexpr int SMALL = SMALL_FONT_ID;
constexpr bool BLACK = true;

constexpr int BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
constexpr int NIGHT_DENSITY = 9;  // darker than Earth's: the far side of the
                                  // terminator is genuinely BLACK up there
}  // namespace

int MoonActivity::radius() const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(SMALL);
  const int bottom = renderer.getScreenHeight() - m.buttonHintsHeight - m.verticalSpacing -
                     lineH * 3 - 12;
  const int availH = bottom - (m.topPadding + m.headerHeight + m.verticalSpacing) - 10;
  const int base = std::min(pageW / 2 - m.contentSidePadding, availH / 2);
  static const float MUL[ZOOM_LEVELS] = {1.0f, 1.7f, 2.8f};
  return (int)(base * MUL[zoom_]);
}

void MoonActivity::goSubEarth() {
  viewLat_ = subELat_;
  viewLon_ = subELon_;
}

void MoonActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);
  backHeld = confirmHeld = confirmLong = false;
  backLong_ = false;
  needFull_ = true;
  {
    Preferences p;
    p.begin("almanac", true);
    chrome_ = p.getUChar("moonui", 1) != 0;
    p.end();
  }
  TimeSource::begin();
  timeSet_ = TimeSource::isSet();

  const time_t t = timeSet_ ? TimeSource::nowUtc() : fallback_moment::utc();
  int y, mo, d, hh, mm;
  TimeSource::utcParts(t, y, mo, d, hh, mm);
  const MoonGeo g = moon_geo(sky_julian(y, mo, d, hh + mm / 60.0));
  subELat_ = (float)g.subEarthLat;
  subELon_ = (float)g.subEarthLon;
  subSLat_ = (float)g.subSolarLat;
  subSLon_ = (float)g.subSolarLon;
  illum_ = (float)g.illum;
  goSubEarth();

  mFile = Storage.open("/moon.bin", O_RDONLY);
  haveData_ = false;
  if (mFile) {
    uint8_t h[6];
    if (mFile.read(h, 6) == 6 && memcmp(h, "MON1", 4) == 0) {
      nFeat_ = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
      haveData_ = true;
    }
  }
  requestUpdate();
}

void MoonActivity::onExit() {
  Activity::onExit();
  if (mFile) mFile.close();
}

void MoonActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { backHeld = true; backLong_ = false; }
  if (backHeld && !backLong_ && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    backLong_ = true;  // hold Back: all chrome on/off
    chrome_ = !chrome_;
    Preferences p;
    p.begin("almanac", false);
    p.putUChar("moonui", chrome_ ? 1 : 0);
    p.end();
    needFull_ = true;  // big field change: clear it properly
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!backHeld) return;  // leftover from the Almanac menu
    const bool wasLong = backLong_;
    backHeld = false;
    backLong_ = false;
    if (wasLong) return;
    finish();
    return;
  }

  const float step = SPIN_DEG / (zoom_ + 1);
  bool moved = false;
  nav_.onPressAndContinuous({MappedInputManager::Button::Right}, [&] {
    viewLon_ += step;
    if (viewLon_ > 180) viewLon_ -= 360;
    moved = true;
  });
  nav_.onPressAndContinuous({MappedInputManager::Button::Left}, [&] {
    viewLon_ -= step;
    if (viewLon_ < -180) viewLon_ += 360;
    moved = true;
  });
  nav_.onPressAndContinuous({MappedInputManager::Button::Up}, [&] {
    viewLat_ = std::min(viewLat_ + step, 85.0f);
    moved = true;
  });
  nav_.onPressAndContinuous({MappedInputManager::Button::Down}, [&] {
    viewLat_ = std::max(viewLat_ - step, -85.0f);
    moved = true;
  });
  if (moved) { requestUpdate(); return; }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) { confirmHeld = true; confirmLong = false; }
  if (confirmHeld && !confirmLong && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    confirmLong = true;  // hold: back to the face the Moon shows Earth
    goSubEarth();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!confirmHeld) return;  // leftover from the Almanac menu
    const bool wasLong = confirmLong;
    confirmHeld = confirmLong = false;
    if (wasLong) return;
    zoom_ = (zoom_ + 1) % ZOOM_LEVELS;
    requestUpdate();
  }
}

void MoonActivity::drawMoon(int cx, int cy, int R) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(SMALL);
  const float xl = 0, xr = (float)pageW;
  // chrome hidden: space runs edge to edge; the disc itself stays anchored
  // at the chrome-on layout either way (see render()).
  const float yt = chrome_ ? (float)(m.topPadding + m.headerHeight + m.verticalSpacing - 6) : 0.0f;
  const float yb = chrome_ ? (float)(renderer.getScreenHeight() - m.buttonHintsHeight -
                                     m.verticalSpacing - lineH * 3 - 13)
                           : (float)(renderer.getScreenHeight() - 1);

  const globe::Basis B = globe::viewBasis(viewLat_, viewLon_);

  // ---- space, always: black band, white disc, still stars -------------------
  renderer.fillRect(0, (int)yt, pageW, (int)(yb - yt) + 1, BLACK);
  const int rowTop = std::max(cy - R, (int)yt), rowBot = std::min(cy + R, (int)yb);
  for (int Y = rowTop; Y <= rowBot; Y++) {
    const float dy = (float)(Y - cy) / R;
    const int dx = (int)(R * sqrtf(std::max(0.0f, 1.0f - dy * dy)));
    const int X0 = std::max(cx - dx, (int)xl), X1 = std::min(cx + dx, (int)xr - 1);
    if (X1 >= X0) renderer.drawLine(X0, Y, X1, Y, false);
  }
  uint32_t s = 0x5EED5EED;
  for (int i = 0; i < 110; i++) {
    s = s * 1664525u + 1013904223u;
    const int X = (int)((s >> 16) % (uint32_t)pageW);
    s = s * 1664525u + 1013904223u;
    const int Y = (int)yt + (int)((s >> 16) % (uint32_t)std::max(1, (int)(yb - yt)));
    const long ddx = X - cx, ddy = Y - cy;
    if (ddx * ddx + ddy * ddy <= (long)(R + 3) * (R + 3)) continue;
    renderer.drawPixel(X, Y, false);
  }

  // ---- night side from the real sub-solar point ------------------------------
  {
    float sw[3], sv[3];
    globe::unitVec(subSLat_, subSLon_, sw);
    for (int i = 0; i < 3; i++)
      sv[i] = B.m[i * 3] * sw[0] + B.m[i * 3 + 1] * sw[1] + B.m[i * 3 + 2] * sw[2];
    for (int Y = rowTop; Y <= rowBot; Y++) {
      const float py = (float)(cy - Y) / R;
      float spans[4];
      const int nSpan = globe::nightSpans(sv, py, 0.0f, spans);
      for (int sp = 0; sp < nSpan; sp++) {
        int X0 = cx + (int)ceilf(spans[sp * 2] * R), X1 = cx + (int)floorf(spans[sp * 2 + 1] * R);
        X0 = std::max(X0, (int)xl);
        X1 = std::min(X1, (int)xr - 1);
        for (int X = X0; X <= X1; X++)
          if (BAYER[Y & 3][X & 3] < NIGHT_DENSITY) renderer.drawPixel(X, Y, BLACK);
      }
    }
  }

  // ---- selenographic graticule, dotted every 30 degrees ----------------------
  auto plotDot = [&](float lat, float lon) {
    float v[3];
    globe::unitVec(lat, lon, v);
    const float sx = B.m[0] * v[0] + B.m[1] * v[1] + B.m[2] * v[2];
    const float sy = B.m[3] * v[0] + B.m[4] * v[1] + B.m[5] * v[2];
    const float sz = B.m[6] * v[0] + B.m[7] * v[1] + B.m[8] * v[2];
    if (sz <= 0) return;
    const int X = cx + (int)(sx * R), Y = cy - (int)(sy * R);
    if (X >= (int)xl && X < (int)xr && Y >= (int)yt && Y < (int)yb)
      renderer.drawPixel(X, Y, BLACK);
  };
  for (int glat = -60; glat <= 60; glat += 30)
    for (int i = 0; i < 360; i += 3) plotDot((float)glat, (float)i - 180);
  for (int glon = -180; glon < 180; glon += 30)
    for (int i = -87; i <= 87; i += 3) plotDot((float)i, (float)glon);

  // ---- features from moon.bin, nearest one named ------------------------------
  nearName_[0] = 0;
  nearDist_ = 999;
  if (haveData_) {
    mFile.seek(6);
    for (uint16_t i = 0; i < nFeat_; i++) {
      uint8_t hd[2];
      if (mFile.read(hd, 2) != 2) break;
      const uint8_t rank = hd[0], nl = hd[1];
      char name[44] = {0};
      mFile.read(name, nl < 43 ? nl : 43);
      if (nl >= 43) mFile.seekCur(nl - 43);
      int16_t ll[2];
      uint16_t diam;
      mFile.read(ll, 4);
      mFile.read(&diam, 2);
      const float la = ll[0] / 100.0f, lo = ll[1] / 100.0f;

      float v[3];
      globe::unitVec(la, lo, v);
      const float sx = B.m[0] * v[0] + B.m[1] * v[1] + B.m[2] * v[2];
      const float sy = B.m[3] * v[0] + B.m[4] * v[1] + B.m[5] * v[2];
      const float sz = B.m[6] * v[0] + B.m[7] * v[1] + B.m[8] * v[2];
      if (sz <= 0) continue;

      // angular distance to the reticle (view center): acos of the forward dot
      const float ang = acosf(std::min(1.0f, sz)) * 57.29578f;
      if (ang < nearDist_ && ang < 6.0f) {
        nearDist_ = ang;
        strncpy(nearName_, name, sizeof(nearName_) - 1);
      }

      const int px = (int)(diam / 2.0f / MOON_RADIUS_KM * R);  // km -> pixels
      // level of detail: small features appear as you zoom in
      if (px < 2 && rank >= 2) continue;
      if (px < 1 && rank >= 1) continue;
      const int X = cx + (int)(sx * R), Y = cy - (int)(sy * R);
      if (X < (int)xl || X >= (int)xr || Y < (int)yt || Y >= (int)yb) continue;
      // circles for everything -- craters ARE circles. Midpoint circle like
      // the limb, since GfxRenderer has no circle primitive; maria get a
      // sparse dotted ring so their (much larger) extent reads as a region,
      // craters a solid rim.
      const int r0 = std::max(px, 2);
      if (rank == 0) {
        for (int a = 0; a < 360; a += 12)
          renderer.drawPixel(X + (int)(px * cosf(a * 0.0174533f)),
                             Y + (int)(px * sinf(a * 0.0174533f)), BLACK);
      } else {
        int cxr = r0, cyr = 0, e = 1 - r0;
        auto putc = [&](int PX, int PY) {
          if (PX >= (int)xl && PX < (int)xr && PY >= (int)yt && PY < (int)yb)
            renderer.drawPixel(PX, PY, BLACK);
        };
        while (cxr >= cyr) {
          putc(X + cxr, Y + cyr); putc(X + cyr, Y + cxr);
          putc(X - cyr, Y + cxr); putc(X - cxr, Y + cyr);
          putc(X - cxr, Y - cyr); putc(X - cyr, Y - cxr);
          putc(X + cyr, Y - cxr); putc(X + cxr, Y - cyr);
          cyr++;
          if (e < 0) e += 2 * cyr + 1;
          else { cxr--; e += 2 * (cyr - cxr) + 1; }
        }
      }
    }
  }

  // ---- limb (white against space) + reticle ----------------------------------
  int x = R, y0 = 0, err = 1 - R;
  auto put = [&](int X, int Y) {
    if (X >= (int)xl && X < (int)xr && Y >= (int)yt && Y < (int)yb)
      renderer.drawPixel(X, Y, false);
  };
  while (x >= y0) {
    put(cx + x, cy + y0); put(cx + y0, cy + x); put(cx - y0, cy + x); put(cx - x, cy + y0);
    put(cx - x, cy - y0); put(cx - y0, cy - x); put(cx + y0, cy - x); put(cx + x, cy - y0);
    y0++;
    if (err < 0) err += 2 * y0 + 1;
    else { x--; err += 2 * (y0 - x) + 1; }
  }
  renderer.drawLine(cx - 16, cy, cx - 6, cy, 2, BLACK);
  renderer.drawLine(cx + 6, cy, cx + 16, cy, 2, BLACK);
  renderer.drawLine(cx, cy - 16, cx, cy - 6, 2, BLACK);
  renderer.drawLine(cx, cy + 6, cx, cy + 16, 2, BLACK);
}

void MoonActivity::render(RenderLock&&) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(SMALL);
  renderer.clearScreen();

  if (chrome_) GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Moon");

  const int R = radius();
  const int contentTop = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int barTop = pageH - m.buttonHintsHeight - m.verticalSpacing - lineH * 3 - 12;
  const int cx = pageW / 2, cy = contentTop + (barTop - contentTop) / 2;

  drawMoon(cx, cy, R);

  drawInfoBar(barTop);

  if (needFull_) {
    // Ghost scrub, disc only. A whole-panel FULL_REFRESH takes seconds, but
    // the ghosting lives only under the disc's dither -- so push one frame
    // with the disc solid black (driving those pixels through a full swing,
    // which is what actually erases residue), then the real frame. Two FAST
    // refreshes, and only the disc visibly blinks.
    needFull_ = false;
    for (int Y = cy - R; Y <= cy + R; Y++) {
      const float dy = (float)(Y - cy) / R;
      const int dx = (int)(R * sqrtf(std::max(0.0f, 1.0f - dy * dy)));
      if (dx > 0 && Y >= 0 && Y < pageH)
        renderer.drawLine(std::max(0, cx - dx), Y, std::min(pageW - 1, cx + dx), Y, BLACK);
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    // now the real frame: redraw everything from scratch
    renderer.clearScreen();
    if (chrome_) GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Moon");
    drawMoon(cx, cy, R);
    drawInfoBar(barTop);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void MoonActivity::drawInfoBar(int barTop) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(SMALL);
  if (chrome_) {
  renderer.drawLine(m.contentSidePadding, barTop, pageW - m.contentSidePadding, barTop, BLACK);
  char l1[80], l2[64], l3[64];
  const bool home = fabsf(viewLat_ - subELat_) < 0.5f && fabsf(viewLon_ - subELon_) < 0.5f;
  snprintf(l1, sizeof(l1), "%s   %.1f%c %.1f%c", nearName_[0] ? nearName_ : "--",
           fabsf(viewLat_), viewLat_ >= 0 ? 'N' : 'S', fabsf(viewLon_),
           viewLon_ >= 0 ? 'E' : 'W');
  snprintf(l2, sizeof(l2), "%d%% lit%s%s", (int)lroundf(illum_ * 100),
           home ? "   facing Earth" : "",
           haveData_ ? "" : "   (moon.bin not on SD)");
  if (timeSet_)
    snprintf(l3, sizeof(l3), "hold Confirm: face Earth   zoom %dx", zoom_ + 1);
  else
    snprintf(l3, sizeof(l3), "clock not set - sep 23 2023 moon shown");
  renderer.drawText(SMALL, m.contentSidePadding, barTop + 6, l1);
  renderer.drawText(SMALL, m.contentSidePadding, barTop + 6 + lineH + 2, l2);
  renderer.drawText(SMALL, m.contentSidePadding, barTop + 6 + 2 * (lineH + 2), l3);

  const auto labels = mappedInput.mapLabels("Back", "Zoom", "Spin", "Spin");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}
