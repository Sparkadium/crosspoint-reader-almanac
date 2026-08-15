// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "SkyActivity.h"

#include <Preferences.h>

#include <GfxRenderer.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "Location.h"
#include "MappedInputManager.h"
#include "TimeSource.h"
#include "TouchGestures.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "FallbackMoment.h"
#include "sky_math.h"
#include "stars.h"
namespace {
constexpr int HEAD_FONT = UI_12_FONT_ID;
constexpr int SMALL = SMALL_FONT_ID;

// GfxRenderer has no circle primitives — hand-rolled midpoint circles.
void fillCircleR(const GfxRenderer& r, int cx, int cy, int rad, bool state) {
  for (int dy = -rad; dy <= rad; dy++) {
    const int dx = (int)lround(sqrt((double)rad * rad - (double)dy * dy));
    if (dx > 0) r.drawLine(cx - dx, cy + dy, cx + dx, cy + dy, state);
  }
}
void drawCircleR(const GfxRenderer& r, int cx, int cy, int rad, bool state) {
  int x = rad, y = 0, err = 1 - rad;
  while (x >= y) {
    r.drawPixel(cx + x, cy + y, state); r.drawPixel(cx + y, cy + x, state);
    r.drawPixel(cx - y, cy + x, state); r.drawPixel(cx - x, cy + y, state);
    r.drawPixel(cx - x, cy - y, state); r.drawPixel(cx - y, cy - x, state);
    r.drawPixel(cx + y, cy - x, state); r.drawPixel(cx + x, cy - y, state);
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}
constexpr bool BLACK = true;
constexpr bool WHITE = false;
constexpr uint16_t LONG_PRESS_MS = 700;
}  // namespace

void SkyActivity::onEnter() {
  Activity::onEnter();
  Location::begin();
  TimeSource::begin();  // seeds the system clock from the RTC, on boards that have one
  timeSet_ = TimeSource::isSet();
  if (timeSet_) {
    lat_ = Location::get().lat;
    lon_ = Location::get().lon;
    baseUtc_ = TimeSource::nowUtc();
  } else {
    // no clock: show the fallback moment's sky instead of a dead screen
    lat_ = fallback_moment::LAT;
    lon_ = fallback_moment::LON;
    baseUtc_ = fallback_moment::utc();
  }
  offsetMin_ = 0;
  sawBackPress_ = sawConfirmPress_ = false;
  confirmLong_ = false;
  {
    Preferences p;
    p.begin("almanac", true);
    infoUi_ = p.getUChar("skyui", 1) != 0;
    lines_ = p.getUChar("skylines", 1) != 0;
    p.end();
  }
  requestUpdate();
}


bool SkyActivity::project(double raDeg, double decDeg, double jd, int& px, int& py) const {
  double alt, az;
  sky_altaz(raDeg, decDeg, jd, lat_, lon_, &alt, &az);
  if (alt < 0) return false;
  const double r = (90.0 - alt) / 90.0 * radius_;
  px = cx_ - (int)lround(r * sin(az * D2R));  // E on the left: correct looking UP
  py = cy_ - (int)lround(r * cos(az * D2R));
  return true;
}

void SkyActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) sawBackPress_ = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!sawBackPress_) return;  // leftover from the Almanac menu
    sawBackPress_ = false;
    finish();
    return;
  }
  // ---- Touch ---------------------------------------------------------------
  // Hold toggles the chrome (the hold-Confirm branch below needs a physical
  // Confirm). Swipes mirror the D-pad: horizontal steps 30 minutes, vertical
  // steps a day. A tap anywhere returns to now, same as Confirm.
  if (wasLongPressGesture(mappedInput, LONG_PRESS_MS)) {
    infoUi_ = !infoUi_;
    Preferences p;
    p.begin("almanac", false);
    p.putUChar("skyui", infoUi_ ? 1 : 0);
    p.end();
    requestUpdate();
    return;
  }
  switch (mappedInput.wasSwipe()) {
    case MappedInputManager::SwipeDir::Left: offsetMin_ += 30; requestUpdate(); return;
    case MappedInputManager::SwipeDir::Right: offsetMin_ -= 30; requestUpdate(); return;
    case MappedInputManager::SwipeDir::Up: offsetMin_ += 1440; requestUpdate(); return;
    case MappedInputManager::SwipeDir::Down: offsetMin_ -= 1440; requestUpdate(); return;
    default: break;
  }
  // Tap the sky to toggle the constellation lines. This used to reset the time,
  // which the Confirm hint already does -- so the tap was redundant and the
  // lines had no control at all.
  {
    int tx = 0, ty = 0;
    if (wasContentTapped(mappedInput, renderer, tx, ty)) {
      lines_ = !lines_;
      Preferences p;
      p.begin("almanac", false);
      p.putUChar("skylines", lines_ ? 1 : 0);
      p.end();
      requestUpdate();
      return;
    }
  }

  bool moved = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) { offsetMin_ += 30; moved = true; }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left))  { offsetMin_ -= 30; moved = true; }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up))    { offsetMin_ += 1440; moved = true; }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down))  { offsetMin_ -= 1440; moved = true; }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    sawConfirmPress_ = true;
    confirmLong_ = false;
  }
  if (sawConfirmPress_ && !confirmLong_ && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    confirmLong_ = true;  // hold Confirm: toggle every piece of chrome at once
    infoUi_ = !infoUi_;
    Preferences p;
    p.begin("almanac", false);
    p.putUChar("skyui", infoUi_ ? 1 : 0);
    p.end();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && sawConfirmPress_) {
    const bool wasLong = confirmLong_;
    sawConfirmPress_ = false;
    confirmLong_ = false;
    if (!wasLong) {
      offsetMin_ = 0;
      moved = true;
    }
  }
  if (moved) requestUpdate();
}

void SkyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  if (infoUi_) GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Sky Chart");

  // ---- time under the current offset --------------------------------------
  const time_t viewUtc = baseUtc_ + (time_t)offsetMin_ * 60;
  int y, mo, d, hh, mm;
  const int offMin = TimeSource::localPartsMin(viewUtc, y, mo, d, hh, mm);
  const double offHours = offMin / 60.0;
  const double jd = sky_julian(y, mo, d, hh + mm / 60.0 - offHours);

  double riseU, setU;
  const int sunOk = sky_sunRiseSet(y, mo, d, lat_, lon_, &riseU, &setU);
  const double offNoonH = Location::utcOffsetMinutes(y, mo, d, 12) / 60.0;
  const double riseL = sunOk ? fmod(riseU + offNoonH + 48.0, 24.0) : 0;
  const double setL = sunOk ? fmod(setU + offNoonH + 48.0, 24.0) : 0;

  int wax;
  const double illum = sky_moonIllum(jd, &wax);
  const int phase = sky_moonPhaseIdx(jd);
  static const char* phaseName[8] = {"new moon",  "wax crescent", "first quarter", "wax gibbous",
                                     "full moon", "wan gibbous",  "last quarter",  "wan crescent"};
  static const char* monName[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};

  // ---- geometry ------------------------------------------------------------
  // drawText/drawCenteredText take the TOP of the text, not the baseline.
  const int lineH = renderer.getLineHeight(SMALL);
  const int contentTop = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int barTop = pageH - m.buttonHintsHeight - m.verticalSpacing - lineH * 3 - 20;
  const int headerLineY = contentTop;
  const int diskTop = headerLineY + lineH + 12 + 5;  // +5: sits better between
                                                     // the date line and bar

  radius_ = std::min(pageW / 2 - m.contentSidePadding, (barTop - diskTop) / 2);
  cx_ = pageW / 2;
  cy_ = diskTop + radius_;

  // ---- header line: date, time, dst marker ---------------------------------
  char off[16];
  Location::offsetLabel(off, sizeof(off), offMin);
  char hdr[80];
  if (timeSet_)
    snprintf(hdr, sizeof(hdr), "%s %d  %02d:%02d  %s%s", monName[mo - 1], d, hh, mm, off,
             offsetMin_ ? "  *" : "");
  else  // fallback moment: the year matters, since it is not this year
    snprintf(hdr, sizeof(hdr), "%s %d %d  %02d:%02d  %s%s", monName[mo - 1], d, y, hh, mm, off,
             offsetMin_ ? "  *" : "");
  if (infoUi_) {
    renderer.drawCenteredText(SMALL, headerLineY, hdr);
    if (!timeSet_)
      renderer.drawCenteredText(SMALL, headerLineY - lineH - 2, "clock not set");
  }

  // ---- sky disk ------------------------------------------------------------
  fillCircleR(renderer, cx_, cy_, radius_, BLACK);

  // dotted 45-degree altitude ring
  for (int a = 0; a < 360; a += 4)
    renderer.drawPixel(cx_ + (int)lround(radius_ * 0.5 * cos(a * D2R)),
                       cy_ + (int)lround(radius_ * 0.5 * sin(a * D2R)), WHITE);

  // constellation lines (both endpoints above the horizon)
  for (int i = 0; lines_ && i < N_LINES; i++) {
    SkyLine L;
    memcpy_P(&L, &SKY_LINES[i], sizeof(SkyLine));
    int x1, y1, x2, y2;
    if (!project(L.ra1 * 360.0 / 65535.0, L.dec1 * 90.0 / 32000.0, jd, x1, y1)) continue;
    if (!project(L.ra2 * 360.0 / 65535.0, L.dec2 * 90.0 / 32000.0, jd, x2, y2)) continue;
    renderer.drawLine(x1, y1, x2, y2, WHITE);
  }

  // stars, scaled up for the bigger disk
  for (int i = 0; i < N_STARS; i++) {
    SkyStar s;
    memcpy_P(&s, &SKY_STARS[i], sizeof(SkyStar));
    int px, py;
    if (!project(s.ra * 360.0 / 65535.0, s.dec * 90.0 / 32000.0, jd, px, py)) continue;
    switch (s.cls) {
      case 0: fillCircleR(renderer, px, py, 4, WHITE); break;
      case 1: fillCircleR(renderer, px, py, 3, WHITE); break;
      case 2: fillCircleR(renderer, px, py, 2, WHITE); break;
      default: fillCircleR(renderer, px, py, 1, WHITE); break;
    }
  }

  // Cardinal letters just inside the rim. East is on the LEFT and west on the
  // right: this is an all-sky chart drawn as if you are lying back and looking
  // UP, so the compass runs the opposite way to a map of the ground. Hold it
  // overhead with N pointing north and the stars line up with the real sky.
  if (infoUi_) {
    const int capW = renderer.getTextWidth(SMALL, "W");
    renderer.drawText(SMALL, cx_ - capW / 2, cy_ - radius_ + 6, "N", WHITE);
    renderer.drawText(SMALL, cx_ - capW / 2, cy_ + radius_ - lineH - 6, "S", WHITE);
    renderer.drawText(SMALL, cx_ - radius_ + 8, cy_ - lineH / 2, "E", WHITE);
    renderer.drawText(SMALL, cx_ + radius_ - capW - 8, cy_ - lineH / 2, "W", WHITE);

    // ---- bottom bar: moon + sun -------------------------------------------
    renderer.drawLine(m.contentSidePadding, barTop, pageW - m.contentSidePadding, barTop, BLACK);

    const int moonR = 14;
    const int moonY = barTop + 10 + moonR;
    drawMoonGlyph(m.contentSidePadding + moonR + 4, moonY, moonR, phase);

    char mtxt[40];
    snprintf(mtxt, sizeof(mtxt), "%s  %d%%", phaseName[phase], (int)lround(illum * 100));
    renderer.drawText(SMALL, m.contentSidePadding + moonR * 2 + 16, moonY - lineH / 2, mtxt);

    char stxt[40];
    if (sunOk)
      snprintf(stxt, sizeof(stxt), "rise %02d:%02d   set %02d:%02d", (int)riseL,
               (int)lround((riseL - (int)riseL) * 60) % 60, (int)setL,
               (int)lround((setL - (int)setL) * 60) % 60);
    else
      snprintf(stxt, sizeof(stxt), "sun: --");
    renderer.drawText(SMALL, m.contentSidePadding, moonY + moonR + 6, stxt);
    char loc[48];
    snprintf(loc, sizeof(loc), "%.4f%c %.4f%c   east is left (chart faces up)",
             fabs(lat_), lat_ >= 0 ? 'N' : 'S', fabs(lon_), lon_ >= 0 ? 'E' : 'W');
    renderer.drawText(SMALL, m.contentSidePadding, moonY + moonR + 6 + lineH + 2, loc);

    const auto labels = mappedInput.mapLabels("Back", "Now", "-30 min", "+30 min");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  // On a panel with no physical Back or Confirm the hints ARE the buttons:
  // TouchRegistry only registers a hint that was drawn, so hiding them strands
  // the activity. Draw them regardless of the chrome setting when the only
  // way out is a tap.
  if (!infoUi_ && mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels("Back", "Now", "-30 min", "+30 min");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

// 8-phase moon glyph, 1-bit: black disk + white lit region.
// Waxing lights the right side (northern hemisphere).
void SkyActivity::drawMoonGlyph(int cx, int cy, int r, int phase) const {
  fillCircleR(renderer, cx, cy, r, BLACK);
  switch (phase) {
    case 0: break;  // new: all dark
    case 4: fillCircleR(renderer, cx, cy, r - 1, WHITE); break;
    case 2: renderer.fillRect(cx, cy - r + 1, r, 2 * r - 1, WHITE); break;
    case 6: renderer.fillRect(cx - r + 1, cy - r + 1, r, 2 * r - 1, WHITE); break;
    case 1:
      renderer.fillRect(cx, cy - r + 1, r, 2 * r - 1, WHITE);
      fillCircleR(renderer, cx + r / 3, cy, r - r / 3, BLACK);
      break;
    case 3:
      renderer.fillRect(cx, cy - r + 1, r, 2 * r - 1, WHITE);
      fillCircleR(renderer, cx - r / 3, cy, r - r / 3, WHITE);
      break;
    case 5:
      renderer.fillRect(cx - r + 1, cy - r + 1, r, 2 * r - 1, WHITE);
      fillCircleR(renderer, cx + r / 3, cy, r - r / 3, WHITE);
      break;
    case 7:
      renderer.fillRect(cx - r + 1, cy - r + 1, r, 2 * r - 1, WHITE);
      fillCircleR(renderer, cx - r / 3, cy, r - r / 3, BLACK);
      break;
    default: break;
  }
  drawCircleR(renderer, cx, cy, r, BLACK);
}
