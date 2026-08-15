// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "PlanetariumActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "MappedInputManager.h"
#include "TimeSource.h"
#include "TouchGestures.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "sky_math.h"
#include "tex_math.h"

namespace {
constexpr int HEAD_FONT = UI_12_FONT_ID;
constexpr int SMALL = SMALL_FONT_ID;
constexpr bool BLACK = true;

struct Body {
  const char* name;
  const char* file;
  const char* blend;   // second TEX1, SCREEN-composited at load (clouds)
  bool lines;          // overlay /globe.bin coasts+borders in white
};
// Order is the Confirm-cycling order: outward from the Sun, Earth's three
// faces together, the Moon between Earth and Mars where it belongs.
const Body BODIES[PlanetariumActivity::N_BODIES] = {
    {"Sun", "/tex_sun.bin", nullptr, false},
    {"Mercury", "/tex_mercury.bin", nullptr, false},
    {"Venus", "/tex_venus.bin", nullptr, false},
    {"Earth", "/tex_earth_day.bin", nullptr, false},
    {"Earth - clouds", "/tex_earth_day.bin", "/tex_earth_clouds.bin", false},
    {"Earth - night", "/tex_earth_night.bin", nullptr, true},
    {"Moon", "/tex_moon.bin", nullptr, false},
    {"Mars", "/tex_mars.bin", nullptr, false},
    {"Jupiter", "/tex_jupiter.bin", nullptr, false},
    {"Saturn", "/tex_saturn.bin", nullptr, false},
    {"Uranus", "/tex_uranus.bin", nullptr, false},
    {"Neptune", "/tex_neptune.bin", nullptr, false},
};
}  // namespace

int PlanetariumActivity::radius() const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(SMALL);
  const int bottom = renderer.getScreenHeight() - m.buttonHintsHeight - m.verticalSpacing -
                     lineH * 3 - 12;
  const int availH = bottom - (m.topPadding + m.headerHeight + m.verticalSpacing) - 10;
  return std::min(pageW / 2 - 8, availH / 2);
}

void PlanetariumActivity::goHome() {
  viewLat_ = 0;
  viewLon_ = (body_ == 0) ? homeLon_ : 0;  // the Sun faces you as it really does
}

void PlanetariumActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);
  backHeld = confirmHeld = false;
  backLong_ = confirmLong_ = false;
  needFull_ = true;
  mode_ = VIEW;
  menuSel_ = 0;

  prefs_.begin("planetarium");
  body_ = prefs_.getUChar("body", 0) % N_BODIES;
  info_ = prefs_.getUChar("info", 1) != 0;
  cross_ = prefs_.getUChar("cross", 0) != 0;
  loadedBody_ = 255;

  TimeSource::begin();
  timeSet_ = TimeSource::isSet();
  homeLon_ = 0;
  if (timeSet_) {
    int y, mo, d, hh, mm;
    TimeSource::utcParts(TimeSource::nowUtc(), y, mo, d, hh, mm);
    const double jd = sky_julian(y, mo, d, hh + mm / 60.0);
    // Carrington rotation: the Sun's synodic period is ~25.38 days; spin the
    // texture so "today's face" is today's, sunspots-as-of-the-photo aside.
    homeLon_ = (float)sky_norm360((jd - 2451545.0) / 25.38 * 360.0) - 180.0f;
  }

  // Claim the photo memory FIRST on the fresh entry heap -- the entire
  // reason this activity exists as its own module.
  ensureBody();
  goHome();
  requestUpdate();
}

void PlanetariumActivity::onExit() {
  Activity::onExit();
  prefs_.putUChar("body", body_);
  prefs_.putUChar("info", info_ ? 1 : 0);
  prefs_.putUChar("cross", cross_ ? 1 : 0);
  prefs_.end();
  freePhoto();
}

void PlanetariumActivity::freePhoto() {
  if (texBuf_) { texBuf_->release(); delete texBuf_; texBuf_ = nullptr; }
  if (texLuts_) texLuts_->release();
  delete texLuts_; texLuts_ = nullptr;
  free(fsErr_); fsErr_ = nullptr;
  texOk_ = false;
  loadedBody_ = 255;
}

bool PlanetariumActivity::ensureBody() {
  if (texOk_ && loadedBody_ == body_) return true;
  if (!texBuf_) {
    texBuf_ = new (std::nothrow) tex::ChunkedBuf;
    if (texBuf_ && !texBuf_->alloc()) { delete texBuf_; texBuf_ = nullptr; }
    texLuts_ = new (std::nothrow) tex::Luts;
    if (texLuts_ && !texLuts_->alloc()) { delete texLuts_; texLuts_ = nullptr; }
    fsErr_ = (int16_t*)malloc(sizeof(int16_t) * 2 * (renderer.getScreenWidth() + 2));
    if (!texBuf_ || !texLuts_ || !fsErr_) { freePhoto(); return false; }
    texLuts_->build();
  }
  texOk_ = false;
  const Body& b = BODIES[body_];
  HalFile tf = Storage.open(b.file, O_RDONLY);
  if (tf) {
    if (b.blend) {
      HalFile cf = Storage.open(b.blend, O_RDONLY);
      if (cf) { texOk_ = tex::loadResident(tf, &cf, *texBuf_); cf.close(); }
    } else {
      texOk_ = tex::loadResident(tf, (HalFile*)nullptr, *texBuf_);
    }
    tf.close();
  }
  loadedBody_ = texOk_ ? body_ : 255;
  return texOk_;
}

void PlanetariumActivity::runMenuItem(int item) {
  if (item < N_BODIES) {
    body_ = (uint8_t)item;
    ensureBody();
    goHome();
  } else if (item == N_BODIES) {
    info_ = !info_;
  } else if (item == N_BODIES + 1) {
    cross_ = !cross_;
  }
  mode_ = VIEW;
  needFull_ = true;
}

void PlanetariumActivity::drawMenu() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Planetarium");
  const int count = N_BODIES + 3;
  const int lineH = renderer.getLineHeight(HEAD_FONT) + 10;
  const int top = m.topPadding + m.headerHeight + m.verticalSpacing + 8;
  const int pad = m.contentSidePadding;
  for (int i = 0; i < count; i++) {
    const char* label;
    char buf[40];
    if (i < N_BODIES) {
      snprintf(buf, sizeof(buf), "%s%s", BODIES[i].name, i == body_ ? "   <" : "");
      label = buf;
    } else if (i == N_BODIES) {
      label = info_ ? "Info text: on" : "Info text: off";
    } else if (i == N_BODIES + 1) {
      label = cross_ ? "Crosshair: on" : "Crosshair: off";
    } else {
      label = "Close";
    }
    const bool sel = (i == menuSel_);
    if (sel) renderer.fillRect(pad - 6, top + i * lineH - 3, pageW - (pad - 6) * 2, lineH - 3, true);
    renderer.drawText(HEAD_FONT, pad + 4, top + i * lineH, label, !sel);
  }
  const auto labels = mappedInput.mapLabels("Close", "Choose", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void PlanetariumActivity::loop() {
  // Hold anywhere to open the menu (no physical Back on this panel).
  if (mode_ == VIEW && wasLongPressGesture(mappedInput, LONG_PRESS_MS)) {
    mode_ = MENU_M;
    menuSel_ = 0;
    requestUpdate();
    return;
  }


  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { backHeld = true; backLong_ = false; }
  if (backHeld && !backLong_ && mode_ == VIEW &&
      mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    backLong_ = true;  // hold Back: menu
    mode_ = MENU_M;
    menuSel_ = body_;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!backHeld) return;  // leftover from the Almanac menu
    const bool wasLong = backLong_;
    backHeld = backLong_ = false;
    if (wasLong) return;
    if (mode_ == MENU_M) { mode_ = VIEW; needFull_ = true; requestUpdate(); return; }
    finish();
    return;
  }

  // Touch AFTER the Back handler: wasScreenTapped() consumes the tap, and the
  // Back hint is delivered as a tap, so reading it first swallows the only way
  // out of the activity on a panel with no physical Back.
  // Tap the body to return to its home view (the hold-Confirm that used to do it
  // needs a physical Confirm). Swipes spin, so tap is the free slot.
  if (mode_ == VIEW) {
    int tx = 0, ty = 0;
    if (wasContentTapped(mappedInput, renderer, tx, ty)) {
      confirmHeld = false;
      goHome();
      requestUpdate();
      return;
    }
  }

  // Drag the body under the reticle.
  if (mode_ == VIEW && spinDragToView(mappedInput, radius(), viewLat_, viewLon_)) {
    requestUpdate();
    return;
  }

  if (mode_ == MENU_M) {
    // Tap a menu row (these menus predate ListLayout; the hit test mirrors
    // drawMenu()'s fixed-pitch arithmetic).
    {
      const auto& mm = UITheme::getInstance().getMetrics();
      const int menuTop = mm.topPadding + mm.headerHeight + mm.verticalSpacing + 8;
      const int hit = simpleMenuTap(mappedInput, menuTop, renderer.getLineHeight(HEAD_FONT) + 10, N_BODIES + 3);
      if (hit >= 0) {
        menuSel_ = hit;
        runMenuItem(hit);
        requestUpdate();
        return;
      }
    }
    const int count = N_BODIES + 3;
    bool moved = false;
    nav_.onNext([&] { menuSel_ = ButtonNavigator::nextIndex(menuSel_, count); moved = true; });
    nav_.onPrevious([&] { menuSel_ = ButtonNavigator::previousIndex(menuSel_, count); moved = true; });
    if (moved) { requestUpdate(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHeld = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!confirmHeld) return;
      confirmHeld = false;
      runMenuItem(menuSel_);
      requestUpdate();
    }
    return;
  }

  const float step = SPIN_DEG;
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

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) { confirmHeld = true; confirmLong_ = false; }
  if (confirmHeld && !confirmLong_ && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    confirmLong_ = true;  // hold: home view
    goHome();
    needFull_ = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!confirmHeld) return;  // leftover from the Almanac menu
    const bool wasLong = confirmLong_;
    confirmHeld = confirmLong_ = false;
    if (wasLong) return;
    body_ = (uint8_t)((body_ + 1) % N_BODIES);  // tap: next body
    ensureBody();
    goHome();
    needFull_ = true;
    requestUpdate();
  }
}

void PlanetariumActivity::drawBody(int cx, int cy, int R) {
  if (strcmp(BODIES[body_].name, "Saturn") == 0) R = (int)(R / 2.32f);  // ball shrinks so the A ring (2.27 R) fits the panel
  const int pageW = renderer.getScreenWidth();
  const auto& m = UITheme::getInstance().getMetrics();
  const int lineH = renderer.getLineHeight(SMALL);
  const float xl = 0, xr = (float)pageW;
  const float yt = info_ ? (float)(m.topPadding + m.headerHeight + m.verticalSpacing - 6) : 0.0f;
  const float yb = info_ ? (float)(renderer.getScreenHeight() - m.buttonHintsHeight -
                                   m.verticalSpacing - lineH * 3 - 13)
                         : (float)(renderer.getScreenHeight() - 1);

  const globe::Basis B = globe::viewBasis(viewLat_, viewLon_);

  // ---- space, always: black band, still stars --------------------------------
  renderer.fillRect(0, (int)yt, pageW, (int)(yb - yt) + 1, BLACK);
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

  if (!texOk_) {
    renderer.drawCenteredText(HEAD_FONT, cy - 20, BODIES[body_].file + 1, false);
    renderer.drawCenteredText(SMALL, cy + 6, "not on SD card - run make_texture.py", false);
    return;
  }

  // ---- the photograph ---------------------------------------------------------
  {
    tex::FsRow fsr{fsErr_, renderer.getScreenWidth()};
    tex::renderPhotoGlobe(B, cx, cy, R, (int)xl, (int)yt, (int)xr, (int)yb,
                          *texBuf_, *texLuts_, fsr,
                          [&](int X, int Y, bool white) {
                            renderer.drawPixel(X, Y, !white);
                          });
  }

  // ---- Earth night: coasts + borders in white from /globe.bin, if present ----
  if (BODIES[body_].lines) {
    HalFile gf = Storage.open("/globe.bin", O_RDONLY);
    if (gf) {
      uint8_t h[6];
      if (gf.read(h, 6) == 6 && memcmp(h, "GLB1", 4) == 0) {
        const uint16_t nLines = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
        static int16_t buf[3 * 128];
        for (uint16_t L = 0; L < nLines; L++) {
          uint8_t lh[3];
          if (gf.read(lh, 3) != 3) break;
          const uint8_t kind = lh[0];  // 0 coast, 1 graticule, 2 border
          uint16_t np = (uint16_t)lh[1] | ((uint16_t)lh[2] << 8);
          if (kind == 1) { gf.seekCur((uint32_t)np * 6); continue; }  // no graticule
          float pxPrev = 0, pyPrev = 0;
          bool prevVis = false;
          int idx = 0;
          while (np > 0) {
            const int chunk = np < 128 ? np : 128;
            if (gf.read(buf, chunk * 6) != chunk * 6) { np = 0; break; }
            for (int i = 0; i < chunk; i++, idx++) {
              const float Px = buf[i * 3] / 32000.0f, Py = buf[i * 3 + 1] / 32000.0f,
                          Pz = buf[i * 3 + 2] / 32000.0f;
              const float sx = B.m[0] * Px + B.m[1] * Py + B.m[2] * Pz;
              const float sy = B.m[3] * Px + B.m[4] * Py + B.m[5] * Pz;
              const float sz = B.m[6] * Px + B.m[7] * Py + B.m[8] * Pz;
              const bool vis = sz > 0;
              const float X = cx + sx * R, Y = cy - sy * R;
              if (kind == 2 && (idx & 3) == 3) {
                // dashed borders, same rhythm as the Globe
              } else if (vis && prevVis) {
                if (X >= xl && X < xr && Y >= yt && Y < yb && pxPrev >= xl && pxPrev < xr &&
                    pyPrev >= yt && pyPrev < yb)
                  renderer.drawLine((int)pxPrev, (int)pyPrev, (int)X, (int)Y, false);
              }
              pxPrev = X; pyPrev = Y; prevVis = vis;
            }
            np -= chunk;
          }
        }
      }
      gf.close();
    }
  }

  // ---- Saturn's rings: procedural, real proportions -----------------------------
  // Rings live in the equatorial plane, so each is a circle of world points
  // through the SAME view basis as the texture: tilt the view and they open
  // and close honestly. Radii in Saturn radii: C 1.24-1.53, B 1.53-1.95,
  // Cassini gap, A 2.03-2.27. Dot density per band approximates opacity.
  // A dot shows if it is on the near side (sz > 0) or clears the silhouette.
  if (strcmp(BODIES[body_].name, "Saturn") == 0 && texOk_) {
    struct Band { float r0, r1; int step; };  // step: draw every Nth ring dot
    static const Band BANDS[3] = {{1.24f, 1.53f, 3}, {1.56f, 1.95f, 1}, {2.03f, 2.27f, 2}};
    for (const Band& bd : BANDS) {
      for (float r = bd.r0; r <= bd.r1; r += 0.055f) {
        const int n = (int)(r * R * 0.9f);  // dots around this ring
        for (int i = 0; i < n; i++) {
          if ((i % (bd.step + 1)) >= 1 && bd.step > 1) continue;
          const float a = 6.2831853f * i / n;
          const float Px = r * cosf(a), Py = r * sinf(a);  // Pz = 0: ring plane
          const float sx = B.m[0] * Px + B.m[1] * Py;
          const float sy = B.m[3] * Px + B.m[4] * Py;
          const float sz = B.m[6] * Px + B.m[7] * Py;
          const float X = cx + sx * R, Y = cy - sy * R;
          if (X < xl || X >= xr || Y < yt || Y >= yb) continue;
          const float dx = X - cx, dy = Y - cy;
          if (sz <= 0 && dx * dx + dy * dy <= (float)R * R) continue;  // behind the ball
          renderer.drawPixel((int)X, (int)Y, false);
        }
      }
    }
  }

  // ---- limb (white against space) --------------------------------------------
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

  // ---- crosshair (off by default: this module is a gallery) -------------------
  if (cross_) {
    renderer.drawLine(cx - 17, cy, cx - 5, cy, 4, true);
    renderer.drawLine(cx + 5, cy, cx + 17, cy, 4, true);
    renderer.drawLine(cx, cy - 17, cx, cy - 5, 4, true);
    renderer.drawLine(cx, cy + 5, cx, cy + 17, 4, true);
    renderer.drawLine(cx - 16, cy, cx - 6, cy, 2, false);
    renderer.drawLine(cx + 6, cy, cx + 16, cy, 2, false);
    renderer.drawLine(cx, cy - 16, cx, cy - 6, 2, false);
    renderer.drawLine(cx, cy + 6, cx, cy + 16, 2, false);
  }
}

void PlanetariumActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (mode_ == MENU_M) { drawMenu(); return; }
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(SMALL);

  if (info_)
    renderer.drawCenteredText(HEAD_FONT, m.topPadding + 6, BODIES[body_].name, false);

  const int R = radius();
  const int contentTop = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int barTop = pageH - m.buttonHintsHeight - m.verticalSpacing - lineH * 3 - 12;
  const int cx = pageW / 2, cy = contentTop + (barTop - contentTop) / 2;

  drawBody(cx, cy, R);

  if (info_) {
    char l1[64], l2[64], l3[64];
    snprintf(l1, sizeof(l1), "%s   %.1f%c %.1f%c", BODIES[body_].name, fabsf(viewLat_),
             viewLat_ >= 0 ? 'N' : 'S', fabsf(viewLon_), viewLon_ >= 0 ? 'E' : 'W');
    if (body_ == 0 && timeSet_)
      snprintf(l2, sizeof(l2), "Carrington face of today");
    else
      snprintf(l2, sizeof(l2), "%d of %d", body_ + 1, N_BODIES);
    snprintf(l3, sizeof(l3), "Confirm: next body   hold: home");
    renderer.drawText(SMALL, m.contentSidePadding, barTop + 6, l1, false);
    renderer.drawText(SMALL, m.contentSidePadding, barTop + 6 + lineH + 2, l2, false);
    renderer.drawText(SMALL, m.contentSidePadding, barTop + 6 + 2 * (lineH + 2), l3, false);
  }

  // On a panel with no physical Back or Confirm the hints ARE the buttons:
  // TouchRegistry only registers a hint that was drawn, so hiding them strands
  // the activity. Draw them regardless of the chrome setting when the only
  // way out is a tap.
  if (mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels("Back", "Next", "Spin", "Spin");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  // FS dither ghosts a little under FAST_REFRESH; force FULL every 24th
  // render (and on needFull_ events: entry, body change, menu).
  {
    static uint8_t partials = 0;
    bool full = needFull_;
    if (!full && ++partials >= 24) { full = true; }
    if (full) partials = 0;
    renderer.displayBuffer(full ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    needFull_ = false;
  }
}
