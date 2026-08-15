// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "GlobeActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "Location.h"
#include "MappedInputManager.h"
#include "ListLayout.h"
#include "FallbackMoment.h"
#include "cty_math.h"
#include "TimeSource.h"
#include "TouchGestures.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "sky_math.h"

namespace {
constexpr int HEAD_FONT = UI_12_FONT_ID;
constexpr int SMALL = SMALL_FONT_ID;

// The Factbook overlay offers three text sizes. The large one is whatever
// reader font this tree actually ships -- fontIds.h entries are #defines, so
// this resolves to Bitter on CrossInk, NotoSerif upstream, and falls back to
// the UI font on anything else rather than failing to build.
// NOTE: the IDs are ALWAYS #defined; it is the font DATA that decides whether
// anything draws. CrossInk 1.5.0 fixed the built-in reading fonts at
// 10/12/14/16pt (lib/EpdFont/builtinFonts/all.h) -- Bitter 18/20 keep their
// headers but are no longer compiled in, so naming 18pt here renders nothing
// ("Font -1308817601 not found"). The OMIT_*_FONT size flags this used to
// probe no longer exist; only OMIT_EMOJI_FONTS survives.
#if defined(BITTER_16_FONT_ID)
constexpr int FACT_LARGE = BITTER_16_FONT_ID;
#elif defined(NOTOSERIF_16_FONT_ID)
constexpr int FACT_LARGE = NOTOSERIF_16_FONT_ID;
#else
constexpr int FACT_LARGE = UI_12_FONT_ID;
#endif
constexpr int FACT_FONTS[3] = {SMALL_FONT_ID, UI_12_FONT_ID, FACT_LARGE};
constexpr bool BLACK = true;

constexpr int BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
constexpr int TWILIGHT_DENSITY = 3;  // of 16 — the mid-grey band (sun 0 to -6 deg)
constexpr int NIGHT_DENSITY = 6;     // of 16 — full night (sun below -6 deg)
constexpr float SIN_M6DEG = -0.104528f;  // sin(-6 deg): civil twilight edge

// Liang-Barsky segment clip to a rect; false if fully outside.
bool clipSeg(float& x0, float& y0, float& x1, float& y1, float xl, float yt, float xr, float yb) {
  float t0 = 0, t1 = 1;
  const float dx = x1 - x0, dy = y1 - y0;
  const float p[4] = {-dx, dx, -dy, dy};
  const float q[4] = {x0 - xl, xr - x0, y0 - yt, yb - y0};
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) { if (q[i] < 0) return false; continue; }
    const float r = q[i] / p[i];
    if (p[i] < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
    else { if (r < t0) return false; if (r < t1) t1 = r; }
  }
  const float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
  x1 = x0 + t1 * dx; y1 = y0 + t1 * dy;
  x0 = nx0; y0 = ny0;
  return true;
}
}  // namespace

// Where the globe SITS is fixed at the info-on layout, so toggling the info
// lines never shifts the planet vertically -- hiding them only lets the
// background and clipping extend further down (contentBottom).
int GlobeActivity::layoutBottom() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - m.buttonHintsHeight - m.verticalSpacing -
         renderer.getLineHeight(SMALL) * 3 - 12 - 1;
}

int GlobeActivity::contentBottom() const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageH = renderer.getScreenHeight();
  if (infoText_) return layoutBottom();
  // no info lines: space mode owns the whole panel; paper mode keeps hints
  return space_ ? pageH - 1 : pageH - m.buttonHintsHeight - m.verticalSpacing - 1;
}

int GlobeActivity::radius() const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int availH = layoutBottom() - (m.topPadding + m.headerHeight + m.verticalSpacing) - 10;
  const int base = std::min(pageW / 2 - m.contentSidePadding, availH / 2);
  static const float MUL[ZOOM_LEVELS] = {1.0f, 1.7f, 2.8f};
  return (int)(base * MUL[zoom_]);
}

void GlobeActivity::goHome() {
  viewLat_ = (float)Location::get().lat;
  viewLon_ = (float)Location::get().lon;
}

void GlobeActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);
  backHeld = confirmHeld = confirmLong = false;
  Location::begin();
  TimeSource::begin();
  timeSet_ = TimeSource::isSet();
  goHome();
  if (!timeSet_) {
    viewLat_ = (float)fallback_moment::LAT;
    viewLon_ = (float)fallback_moment::LON;
  }

  gFile = Storage.open("/globe.bin", O_RDONLY);
  loadError_ = true;
  if (gFile) {
    uint8_t h[6];
    if (gFile.read(h, 6) == 6 && memcmp(h, "GLB1", 4) == 0) {
      nLines_ = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
      loadError_ = false;
    }
  }
  ctyFile_ = Storage.open("/countries.bin", O_RDONLY);  // optional: no file,
  gaz_.begin("/gazetteer.cdb");                         // no Factbook link
  prefs_.begin("globe");
  borders_ = prefs_.getUChar("borders", 1) != 0;
  night_ = prefs_.getUChar("night", 1) != 0;
  space_ = prefs_.getUChar("space", 0) != 0;
  infoText_ = prefs_.getUChar("info", 1) != 0;
  crosshair_ = prefs_.getUChar("cross", 1) != 0;
  factFont_ = prefs_.getUChar("ffont", 1) % 3;
  mode_ = GLOBE;
  status_.clear();
  resolveCountry();

  // Subsolar point from the astropy-verified core: lat is the declination,
  // lon is RA minus GMST. With no clock, the fallback moment's sun is used
  // and the info line says so.
  {
    int y, mo, d, hh, mm;
    TimeSource::utcParts(timeSet_ ? TimeSource::nowUtc() : fallback_moment::utc(), y, mo, d, hh, mm);
    const double jd = sky_julian(y, mo, d, hh + mm / 60.0);
    double ra, dec;
    sky_sunRaDec(jd, &ra, &dec, 0);
    double lon = ra - sky_gmst(jd);
    while (lon < -180) lon += 360;
    while (lon > 180) lon -= 360;
    sunLat_ = (float)dec;
    sunLon_ = (float)lon;
  }
  requestUpdate();
}

void GlobeActivity::onExit() {
  Activity::onExit();
  prefs_.putUChar("borders", borders_ ? 1 : 0);
  prefs_.putUChar("night", night_ ? 1 : 0);
  prefs_.putUChar("space", space_ ? 1 : 0);
  prefs_.putUChar("info", infoText_ ? 1 : 0);
  prefs_.putUChar("cross", crosshair_ ? 1 : 0);
  prefs_.putUChar("ffont", factFont_);
  prefs_.end();
  if (gFile) gFile.close();
  if (ctyFile_) ctyFile_.close();
  gaz_.end();
}

void GlobeActivity::openFind() {
  if (!ctyFile_) { status_ = "countries.bin not on SD card"; mode_ = GLOBE; return; }
  if (findCount_ == 0) {
    const int n = cty::forEachCountry(ctyFile_, [&](int idx, const cty::CountryInfo& ci) {
      if (idx < MAX_FIND) findLetters_[idx] = ci.name[0];
    });
    findCount_ = n > MAX_FIND ? MAX_FIND : (n < 0 ? 0 : n);
  }
  if (findCount_ == 0) { status_ = "countries.bin unreadable"; mode_ = GLOBE; return; }
  findSel_ = 0;
  mode_ = FIND;
}

void GlobeActivity::drawFind() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int count = findCount_;
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Find a country");

  const ListLayout L = computeListLayout(renderer, count, findSel_, /*wantBlurb=*/false);
  // stream only the visible window's names off the card
  constexpr int MAX_ROWS = 24;
  char rows[MAX_ROWS][44];
  const int nRows = L.rowsPerPage < MAX_ROWS ? L.rowsPerPage : MAX_ROWS;
  cty::forEachCountry(ctyFile_, [&](int idx, const cty::CountryInfo& ci) {
    const int k = idx - L.firstVisible;
    if (k >= 0 && k < nRows) { strncpy(rows[k], ci.name, 43); rows[k][43] = 0; }
  });
  const int pad = m.contentSidePadding;
  for (int k = 0; k < nRows; k++) {
    const int i = L.firstVisible + k;
    if (i >= count) break;
    const bool sel = (i == findSel_);
    if (sel) renderer.fillRect(pad - 6, L.rowTop(k), pageW - (pad - 6) * 2, L.boxH, true);
    renderer.drawText(L.titleFont, pad + 4, L.nameTop(k), rows[k], !sel);
  }
  char pos[32];
  snprintf(pos, sizeof(pos), "%d of %d   Left/Right: letter", findSel_ + 1, count);
  renderer.drawCenteredText(L.subFont, L.bottom + 4, pos);

  const auto labels = mappedInput.mapLabels("Cancel", "Fly there", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void GlobeActivity::runMenuItem(int item) {
  switch (item) {
    case 0: openFind(); return;  // sets its own mode
    case 1:  // fly home
      goHome();
      zoom_ = 0;
      status_.clear();
      resolveCountry();
      break;
    case 2: borders_ = !borders_; break;
    case 3: night_ = !night_; break;
    case 4: space_ = !space_; break;
    case 5: infoText_ = !infoText_; break;
    case 6: crosshair_ = !crosshair_; break;
    default: break;
  }
  mode_ = GLOBE;
}

void GlobeActivity::drawMenu() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Globe menu");
  const char* items[8] = {"Find a country", "Fly home",
                          borders_ ? "Country borders: on" : "Country borders: off",
                          night_ ? "Night shading: on" : "Night shading: off",
                          space_ ? "Space background: on" : "Space background: off",
                          infoText_ ? "Bottom info: on" : "Bottom info: off",
                          crosshair_ ? "Crosshair: on" : "Crosshair: off", "Close"};
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID) + 14;
  const int top = m.topPadding + m.headerHeight + m.verticalSpacing + 10;
  const int pad = m.contentSidePadding;
  for (int i = 0; i < 8; i++) {
    const bool sel = (i == menuSel_);
    if (sel) renderer.fillRect(pad - 6, top + i * lineH - 4, pageW - (pad - 6) * 2, lineH - 4, true);
    renderer.drawText(UI_12_FONT_ID, pad + 4, top + i * lineH, items[i], !sel);
  }
  const auto labels = mappedInput.mapLabels("Close", "Choose", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void GlobeActivity::resolveCountry() {
  haveCountry_ = false;
  if (!ctyFile_) return;
  haveCountry_ = cty::findCountry(ctyFile_, viewLat_, viewLon_, countryKey_,
                                  sizeof(countryKey_), countryName_, sizeof(countryName_));
}

void GlobeActivity::openEntry() {
  if (!haveCountry_) { status_ = "open ocean - nothing under the reticle"; return; }
  if (!gaz_.ready()) { status_ = "gazetteer.cdb not on SD card"; return; }
  WcdbReader::Entry e = gaz_.lookup(countryKey_);
  if (!e.found) {
    status_ = std::string("no Factbook entry for ") + countryName_;
    return;
  }
  entryTitle_ = countryName_;
  entryText_ = e.definition;
  scroll_ = 0;
  mode_ = ENTRY;
}

void GlobeActivity::loop() {
  // Hold anywhere to open the menu: the hold-Back branch below needs a physical
  // Back button, which this panel does not have.
  if (mode_ == GLOBE && wasLongPressGesture(mappedInput, LONG_PRESS_MS)) {
    mode_ = MENU_M;
    menuSel_ = 0;
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { backHeld = true; backLong = false; }
  if (backHeld && !backLong && mode_ == GLOBE && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    backLong = true;  // hold Back: menu
    mode_ = MENU_M;
    menuSel_ = 0;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!backHeld) return;  // leftover from the Almanac menu
    const bool wasLong = backLong;
    backHeld = backLong = false;
    if (wasLong) return;
    if (mode_ == ENTRY || mode_ == MENU_M || mode_ == FIND) { mode_ = GLOBE; requestUpdate(); return; }
    finish();
    return;
  }

  // ---- find list -------------------------------------------------------------
  if (mode_ == FIND) {
    const int count = findCount_;
    // Touch: swipe pages the list, tap flies to a country. drawFind() uses
    // ListLayout, so the shared helpers apply directly.
    {
      const ListLayout L = computeListLayout(renderer, count, findSel_, /*wantBlurb=*/false);
      if (listSwipePage(mappedInput, L, count, findSel_)) { requestUpdate(); return; }
      if (listRowTouch(mappedInput, L, count, findSel_)) {
        confirmHeld = false;  // no Confirm press/release pair accompanies a tap
        cty::CountryInfo sel{};
        cty::forEachCountry(ctyFile_, [&](int idx, const cty::CountryInfo& ci) {
          if (idx == findSel_) sel = ci;
        });
        viewLat_ = sel.flyLat;
        viewLon_ = sel.flyLon;
        status_.clear();
        haveCountry_ = true;
        strncpy(countryKey_, sel.key, sizeof(countryKey_) - 1);
        strncpy(countryName_, sel.name, sizeof(countryName_) - 1);
        mode_ = GLOBE;
        requestUpdate();
        return;
      }
    }
    bool moved = false;
    nav_.onPressAndContinuous({MappedInputManager::Button::Down},
                              [&] { findSel_ = (findSel_ + 1) % count; moved = true; });
    nav_.onPressAndContinuous({MappedInputManager::Button::Up},
                              [&] { findSel_ = (findSel_ + count - 1) % count; moved = true; });
    // Left/Right jump by first letter -- 177 rows is a long walk otherwise.
    auto letterJump = [&](int dir) {
      const char cur = findLetters_[findSel_];
      int i = findSel_;
      for (int s = 0; s < count; s++) {
        i = (i + dir + count) % count;
        if (findLetters_[i] != cur) {
          if (dir < 0) {  // land on the FIRST entry of the previous letter
            const char c2 = findLetters_[i];
            while (i > 0 && findLetters_[i - 1] == c2) i--;
          }
          break;
        }
      }
      findSel_ = i;
      moved = true;
    };
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) letterJump(+1);
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) letterJump(-1);
    if (moved) { requestUpdate(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHeld = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!confirmHeld) return;
      confirmHeld = false;
      // one targeted pass fetches the selected entry; we flew here BY NAME,
      // so the selection IS the country -- no polygon test gets to veto it
      cty::CountryInfo sel{};
      cty::forEachCountry(ctyFile_, [&](int idx, const cty::CountryInfo& ci) {
        if (idx == findSel_) sel = ci;
      });
      viewLat_ = sel.flyLat;
      viewLon_ = sel.flyLon;
      status_.clear();
      haveCountry_ = true;
      strncpy(countryKey_, sel.key, sizeof(countryKey_) - 1);
      strncpy(countryName_, sel.name, sizeof(countryName_) - 1);
      mode_ = GLOBE;
      requestUpdate();
    }
    return;
  }

  // ---- menu ------------------------------------------------------------------
  if (mode_ == MENU_M) {
    // Tap a menu row (these menus predate ListLayout; the hit test mirrors
    // drawMenu()'s fixed-pitch arithmetic).
    {
      const auto& mm = UITheme::getInstance().getMetrics();
      const int menuTop = mm.topPadding + mm.headerHeight + mm.verticalSpacing + 10;
      const int hit = simpleMenuTap(mappedInput, menuTop, renderer.getLineHeight(UI_12_FONT_ID) + 14, 8);
      if (hit >= 0) {
        menuSel_ = hit;
        runMenuItem(hit);
        requestUpdate();
        return;
      }
    }
    bool moved = false;
    nav_.onNext([&] { menuSel_ = ButtonNavigator::nextIndex(menuSel_, 8); moved = true; });
    nav_.onPrevious([&] { menuSel_ = ButtonNavigator::previousIndex(menuSel_, 8); moved = true; });
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

  // ---- entry overlay: Up/Down scroll, Confirm cycles the text size ----------
  if (mode_ == ENTRY) {
    bool moved = false;
    nav_.onPressAndContinuous({MappedInputManager::Button::Down}, [&] { scroll_++; moved = true; });
    nav_.onPressAndContinuous({MappedInputManager::Button::Up}, [&] { scroll_ = std::max(0, scroll_ - 1); moved = true; });
    if (moved) { requestUpdate(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHeld = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!confirmHeld) return;
      confirmHeld = false;
      factFont_ = (factFont_ + 1) % 3;
      scroll_ = 0;  // line wrapping changed; page from the top
      requestUpdate();
    }
    return;
  }

  // Spin: pressing RIGHT flies you east, so the globe rolls west under the
  // reticle. Step shrinks with zoom so a press is a similar screen distance.
  // Drag the planet under the reticle.
  if (spinDragToView(mappedInput, radius(), viewLat_, viewLon_)) {
    status_.clear();
    resolveCountry();
    requestUpdate();
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
  if (moved) { status_.clear(); resolveCountry(); requestUpdate(); return; }

  // Tap the globe to open the Factbook entry under the reticle. That is the hold
  // below, which needs a physical Confirm this panel does not have -- and a tap
  // on the thing you want to read about is the more obvious gesture anyway.
  // Swipes are spent on spinning, so tap is the free slot here.
  {
    int tx = 0, ty = 0;
    if (wasContentTapped(mappedInput, renderer, tx, ty)) {
      confirmHeld = false;
      openEntry();
      requestUpdate();
      return;
    }
  }

  // Confirm: tap cycles zoom, hold opens the Factbook entry under the reticle.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) { confirmHeld = true; confirmLong = false; }
  if (confirmHeld && !confirmLong && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    confirmLong = true;
    openEntry();
    confirmHeld = false;  // the release lands in ENTRY mode; without this it
                          // reads as a fresh tap and cycles the font once
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!confirmHeld) return;  // leftover from the Almanac menu
    const bool wasLong = confirmLong;
    confirmHeld = confirmLong = false;
    if (wasLong) return;  // guard BEFORE zooming, or a hold over ocean zooms
    zoom_ = (zoom_ + 1) % ZOOM_LEVELS;
    requestUpdate();
  }
}

// Factbook entry overlay: the country's gazetteer text, word-wrapped, with
// Up/Down paging through lines. Same data the Factbook module shows.
void GlobeActivity::drawEntry() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int F = FACT_FONTS[factFont_];
  const int lineH = renderer.getLineHeight(F) + 2;
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, entryTitle_.c_str());

  const int top = m.topPadding + m.headerHeight + m.verticalSpacing + 4;
  const int bottom = pageH - m.buttonHintsHeight - m.verticalSpacing;
  const int maxW = pageW - 2 * m.contentSidePadding;
  const int rows = (bottom - top) / lineH;

  // greedy word wrap into lines, then show [scroll_, scroll_+rows)
  const std::string& t = entryText_;
  int line = 0, y = top;
  size_t i = 0;
  while (i < t.size() && line < scroll_ + rows) {
    size_t j = i, lastSp = std::string::npos;
    while (j < t.size() && t[j] != '\n') {
      if (t[j] == ' ') {
        if (renderer.getTextWidth(F, t.substr(i, j - i).c_str()) > maxW) break;
        lastSp = j;
      }
      j++;
    }
    if (j < t.size() && t[j] != '\n' && lastSp != std::string::npos &&
        renderer.getTextWidth(F, t.substr(i, j - i).c_str()) > maxW)
      j = lastSp;
    if (line >= scroll_) {
      renderer.drawText(F, m.contentSidePadding, y, t.substr(i, j - i).c_str());
      y += lineH;
    }
    line++;
    i = j + (j < t.size() && (t[j] == ' ' || t[j] == '\n') ? 1 : 0);
    if (j >= t.size()) break;
  }
  if (scroll_ > 0 && line <= scroll_) scroll_ = std::max(0, line - 1);  // past the end

  const auto labels = mappedInput.mapLabels("Globe", "Aa", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void GlobeActivity::drawGlobe(int cx, int cy, int R) {
  const auto& m = UITheme::getInstance().getMetrics();
  // In space mode the black runs edge to edge, so curves and stars may use
  // the full width; otherwise keep the standard side padding.
  const float xl = space_ ? 0.0f : (float)m.contentSidePadding;
  const float xr = space_ ? (float)renderer.getScreenWidth()
                          : (float)(renderer.getScreenWidth() - m.contentSidePadding);
  // Clip/fill top mirrors contentBottom(): with the chrome hidden, drawing
  // may run to the top edge (space mode) or just under the bezel padding
  // (paper mode). The globe's CENTER stays anchored either way -- only how
  // far curves, stars, and the black may extend changes.
  const float yt = infoText_
                       ? (float)(m.topPadding + m.headerHeight + m.verticalSpacing + 2)
                       : (space_ ? 0.0f : (float)m.topPadding);
  // Bottom of the drawing region is the info bar's divider line, computed the
  // same way render() places it -- NOT a band mirrored around the globe. A
  // symmetric band cropped the space background at the globe's bottom edge
  // and left a white strip above the info bar.
  const float yb = (float)contentBottom();

  const globe::Basis B = globe::viewBasis(viewLat_, viewLon_);

  // ---- space background: black void, white disc, a few fixed stars ---------
  if (space_) {
    renderer.fillRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), BLACK);
    // the visible part of the disc back to white, row by row
    const int rowTop = std::max(cy - R, (int)yt), rowBot = std::min(cy + R, (int)yb - 1);
    for (int Y = rowTop; Y <= rowBot; Y++) {
      const float dy = (float)(Y - cy) / R;
      const int dx = (int)(R * sqrtf(std::max(0.0f, 1.0f - dy * dy)));
      const int X0 = std::max(cx - dx, (int)xl), X1 = std::min(cx + dx, (int)xr - 1);
      if (X1 >= X0) renderer.drawLine(X0, Y, X1, Y, false);
    }
    // deterministic starfield: same seed every frame, so the sky holds still
    uint32_t s = 0x5EED5EED;
    for (int i = 0; i < 130; i++) {
      s = s * 1664525u + 1013904223u;
      const int X = (int)xl + (int)((s >> 16) % (uint32_t)std::max(1, (int)(xr - xl)));
      s = s * 1664525u + 1013904223u;
      const int Y = (int)yt + (int)((s >> 16) % (uint32_t)std::max(1, (int)(yb - yt)));
      const long ddx = X - cx, ddy = Y - cy;
      if (ddx * ddx + ddy * ddy <= (long)(R + 3) * (R + 3)) continue;  // not on the planet
      renderer.drawPixel(X, Y, false);
      if ((s & 7) == 0) {  // a few brighter ones
        renderer.drawPixel(X + 1, Y, false);
        renderer.drawPixel(X, Y + 1, false);
      }
    }
  }

  // ---- night side: one quadratic per screen row, Bayer dots ----------------
  if (night_) {
    float sw[3], sv[3];
    globe::unitVec(sunLat_, sunLon_, sw);
    for (int i = 0; i < 3; i++)
      sv[i] = B.m[i * 3] * sw[0] + B.m[i * 3 + 1] * sw[1] + B.m[i * 3 + 2] * sw[2];
    const int rowTop = std::max(cy - R, (int)yt), rowBot = std::min(cy + R, (int)yb);
    for (int Y = rowTop; Y <= rowBot; Y++) {
      const float py = (float)(cy - Y) / R;
      // pass 1: everything past the terminator gets the light grey; pass 2
      // re-covers everything past civil twilight at full night density. The
      // strip between the two contours keeps only the light dither — the
      // mid-grey terminator band.
      static const float THRESH[2] = {0.0f, SIN_M6DEG};
      static const int DENS[2] = {TWILIGHT_DENSITY, NIGHT_DENSITY};
      for (int pass = 0; pass < 2; pass++) {
        float spans[4];
        const int nSpan = globe::nightSpans(sv, py, THRESH[pass], spans);
        for (int s = 0; s < nSpan; s++) {
          int X0 = cx + (int)ceilf(spans[s * 2] * R), X1 = cx + (int)floorf(spans[s * 2 + 1] * R);
          X0 = std::max(X0, (int)xl);
          X1 = std::min(X1, (int)xr - 1);
          for (int X = X0; X <= X1; X++)
            if (BAYER[Y & 3][X & 3] < DENS[pass]) renderer.drawPixel(X, Y, BLACK);
        }
      }
    }
  }

  // ---- polylines, streamed from the SD card --------------------------------
  if (!loadError_) {
    gFile.seek(6);
    static int16_t buf[3 * 128];
    for (uint16_t L = 0; L < nLines_; L++) {
      uint8_t lh[3];
      if (gFile.read(lh, 3) != 3) break;
      const uint8_t kind = lh[0];  // 0 coast, 1 graticule, 2 border
      const bool dotted = kind == 1;
      uint16_t np = (uint16_t)lh[1] | ((uint16_t)lh[2] << 8);
      if (kind == 2 && !borders_) { gFile.seekCur((uint32_t)np * 6); continue; }
      float pxPrev = 0, pyPrev = 0;
      bool prevVis = false;
      int idx = 0;
      while (np > 0) {
        const int chunk = np < 128 ? np : 128;
        if (gFile.read(buf, chunk * 6) != chunk * 6) { np = 0; break; }
        for (int i = 0; i < chunk; i++, idx++) {
          const float Px = buf[i * 3] / 32000.0f, Py = buf[i * 3 + 1] / 32000.0f,
                      Pz = buf[i * 3 + 2] / 32000.0f;
          const float sx = B.m[0] * Px + B.m[1] * Py + B.m[2] * Pz;
          const float sy = B.m[3] * Px + B.m[4] * Py + B.m[5] * Pz;
          const float sz = B.m[6] * Px + B.m[7] * Py + B.m[8] * Pz;
          const bool vis = sz > 0;
          const float X = cx + sx * R, Y = cy - sy * R;
          if (dotted) {
            if (vis && idx % 3 == 0 && X >= xl && X < xr && Y >= yt && Y < yb)
              renderer.drawPixel((int)X, (int)Y, BLACK);
          } else if (kind == 2 && (idx & 3) == 3) {
            // borders are dashed: skip every fourth segment so they read as
            // political lines, not more coastline
          } else if (vis && prevVis) {
            float ax = pxPrev, ay = pyPrev, bx = X, by = Y;
            if (clipSeg(ax, ay, bx, by, xl, yt, xr - 1, yb - 1))
              renderer.drawLine((int)ax, (int)ay, (int)bx, (int)by, BLACK);
          }
          pxPrev = X; pyPrev = Y; prevVis = vis;
        }
        np -= chunk;
      }
    }
  }

  // ---- limb (midpoint circle, clipped) --------------------------------------
  int x = R, y0 = 0, err = 1 - R;
  // against space the planet's edge must be light, not black-on-black
  const bool limbColor = !space_;
  auto put = [&](int X, int Y) {
    if (X >= (int)xl && X < (int)xr && Y >= (int)yt && Y < (int)yb)
      renderer.drawPixel(X, Y, limbColor);
  };
  while (x >= y0) {
    put(cx + x, cy + y0); put(cx + y0, cy + x); put(cx - y0, cy + x); put(cx - x, cy + y0);
    put(cx - x, cy - y0); put(cx - y0, cy - x); put(cx + y0, cy - x); put(cx + x, cy - y0);
    y0++;
    if (err < 0) err += 2 * y0 + 1;
    else { x--; err += 2 * (y0 - x) + 1; }
  }

  // ---- fixed reticle (toggleable: hidden, the reticle still works -- the
  // country readout and Facts key off the invisible screen center) ----------
  if (crosshair_) {
    renderer.drawLine(cx - 16, cy, cx - 6, cy, 2, BLACK);
    renderer.drawLine(cx + 6, cy, cx + 16, cy, 2, BLACK);
    renderer.drawLine(cx, cy - 16, cx, cy - 6, 2, BLACK);
    renderer.drawLine(cx, cy + 6, cx, cy + 16, 2, BLACK);
  }
}

void GlobeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (mode_ == ENTRY) { drawEntry(); return; }
  if (mode_ == MENU_M) { drawMenu(); return; }
  if (mode_ == FIND) { drawFind(); return; }
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(SMALL);

  // The bottom-info toggle governs the top bar too: info off means NO chrome
  // at either end, just the planet.
  if (infoText_) {
    if (space_) {
      renderer.drawCenteredText(HEAD_FONT, m.topPadding + 6, "Globe", false);
    } else {
      GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Globe");
    }
  }

  if (loadError_) {
    renderer.drawCenteredText(HEAD_FONT, pageH / 2 - 20, "globe.bin not on SD card");
    renderer.drawCenteredText(SMALL, pageH / 2 + 10, "Run tools/almanac/make_globe.py and copy it over");
    GUI.drawButtonHints(renderer, "Back", "", "", "");
    renderer.displayBuffer();
    return;
  }

  const int R = radius();
  const int contentTop = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int barTop = contentBottom() + 1;  // text/divider position (varies)
  const int anchorBot = layoutBottom() + 1;  // globe position (fixed)
  const int cx = pageW / 2, cy = contentTop + (anchorBot - contentTop) / 2;

  drawGlobe(cx, cy, R);

  // ---- info: what is under the reticle ---------------------------------------
  const bool ink = !space_;  // black text on paper, white text in space
  if (infoText_ && !space_)
    renderer.drawLine(m.contentSidePadding, barTop, pageW - m.contentSidePadding, barTop, BLACK);
  char l1[80], l2[64], l3[64];
  snprintf(l1, sizeof(l1), "%s   %.1f%c %.1f%c   %dx",
           haveCountry_ ? countryName_ : "open ocean", fabsf(viewLat_),
           viewLat_ >= 0 ? 'N' : 'S', fabsf(viewLon_), viewLon_ >= 0 ? 'E' : 'W', zoom_ + 1);
  {
    float cv[3], sv[3];
    globe::unitVec(viewLat_, viewLon_, cv);
    globe::unitVec(sunLat_, sunLon_, sv);
    const float d = cv[0] * sv[0] + cv[1] * sv[1] + cv[2] * sv[2];
    const float alt = asinf(std::max(-1.0f, std::min(1.0f, d))) * 57.29578f;
    snprintf(l2, sizeof(l2), "sun %+.0f deg  (%s)", alt, alt > 0 ? "daylight" : "night");
    if (timeSet_) {
      int y, mo, dd, hh, mm;
      const int off = TimeSource::localPartsMin(TimeSource::nowUtc(), y, mo, dd, hh, mm);
      char offs[16];
      Location::offsetLabel(offs, sizeof(offs), off);
      snprintf(l3, sizeof(l3), "%02d:%02d %s   hold: facts / menu", hh, mm, offs);
    } else {
      snprintf(l3, sizeof(l3), "clock not set - sep 23 2023 shown");
    }
  }
  if (infoText_) {
    renderer.drawText(SMALL, m.contentSidePadding, barTop + 6, l1, ink);
    renderer.drawText(SMALL, m.contentSidePadding, barTop + 6 + lineH + 2, l2, ink);
    renderer.drawText(SMALL, m.contentSidePadding, barTop + 6 + 2 * (lineH + 2),
                      status_.empty() ? l3 : status_.c_str(), ink);
  } else if (!status_.empty()) {
    // errors still deserve a voice even with the info lines hidden
    renderer.drawCenteredText(SMALL, barTop - lineH - 4, status_.c_str(), ink);
  }

  // On a panel with no physical Back or Confirm the hints ARE the buttons:
  // TouchRegistry only registers a hint that was drawn, so hiding them strands
  // the activity. Draw them regardless of the chrome setting when the only
  // way out is a tap.
  if (!space_ || mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels("Back", "Facts", "Spin", "Spin");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
