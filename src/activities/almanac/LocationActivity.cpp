// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "LocationActivity.h"

#include <GfxRenderer.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string>

#include "Location.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
namespace {
constexpr int BIG_FONT = NOTOSERIF_18_FONT_ID;
constexpr int SMALL = SMALL_FONT_ID;
const char* LABEL[] = {"latitude", "longitude", "utc offset", "daylight saving"};
}  // namespace

void LocationActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);
  Location::begin();
  lat_ = Location::get().lat;
  lon_ = Location::get().lon;
  offMin_ = Location::get().baseOffsetMin;
  dstRule_ = Location::get().dstRule;
  field_ = F_LAT;
  dirty_ = false;
  status_.clear();
  sawConfirmPress_ = sawBackPress_ = false;
  requestUpdate();
}

void LocationActivity::bump(int dir) {
  // Hold to accelerate, so crossing a continent does not take a thousand presses.
  const unsigned long held = mappedInput.getHeldTime();
  const double step = held > 3000 ? 1.0 : (held > 1200 ? 0.1 : 0.01);

  switch (field_) {
    case F_LAT:
      lat_ += dir * step;
      if (lat_ > 90.0) lat_ = 90.0;
      if (lat_ < -90.0) lat_ = -90.0;
      break;
    case F_LON:
      lon_ += dir * step;
      if (lon_ > 180.0) lon_ -= 360.0;   // wrap the antimeridian
      if (lon_ < -180.0) lon_ += 360.0;
      break;
    case F_OFFSET:
      offMin_ += dir * 15;               // quarter-hour zones exist (+05:45)
      if (offMin_ > 840) offMin_ = -720;
      if (offMin_ < -720) offMin_ = 840;
      break;
    case F_DST:
      dstRule_ = (uint8_t)((dstRule_ + (dir > 0 ? 1 : 2)) % 3);
      break;
    default:
      break;
  }
  dirty_ = true;
  status_.clear();
}

void LocationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) sawBackPress_ = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!sawBackPress_) return;  // leftover from the Almanac menu
    sawBackPress_ = false;
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) sawConfirmPress_ = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!sawConfirmPress_) return;
    sawConfirmPress_ = false;
    Location::get().lat = lat_;
    Location::get().lon = lon_;
    Location::get().baseOffsetMin = offMin_;
    Location::get().dstRule = dstRule_;
    Location::save();
    dirty_ = false;
    status_ = "Saved";
    requestUpdate();
    return;
  }

  bool moved = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) { field_ = (field_ + 1) % F_COUNT; moved = true; }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) { field_ = (field_ - 1 + F_COUNT) % F_COUNT; moved = true; }

  // onPressAndContinuous gives the hold-repeat; bump() reads getHeldTime() to
  // pick its own step size, so a long hold sweeps degrees rather than hundredths.
  nav_.onPressAndContinuous({MappedInputManager::Button::Up}, [&] { bump(+1); moved = true; });
  nav_.onPressAndContinuous({MappedInputManager::Button::Down}, [&] { bump(-1); moved = true; });

  if (moved) requestUpdate();
}

void LocationActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Location");

  char vals[F_COUNT][24];
  snprintf(vals[F_LAT], 24, "%.4f %c", fabs(lat_), lat_ >= 0 ? 'N' : 'S');
  snprintf(vals[F_LON], 24, "%.4f %c", fabs(lon_), lon_ >= 0 ? 'E' : 'W');
  Location::offsetLabel(vals[F_OFFSET], 24, offMin_);
  snprintf(vals[F_DST], 24, "%s", Location::dstRuleName(dstRule_));

  const int bigH = renderer.getLineHeight(BIG_FONT);
  const int smallH = renderer.getLineHeight(SMALL);
  const int rowH = smallH + 4 + bigH + 16;
  const int top = m.topPadding + m.headerHeight + m.verticalSpacing + 16;
  const int pad = m.contentSidePadding;

  for (int i = 0; i < F_COUNT; i++) {
    const int rowTop = top + i * rowH;
    renderer.drawText(SMALL, pad + 4, rowTop, LABEL[i]);
    renderer.drawText(BIG_FONT, pad + 4, rowTop + smallH + 4, vals[i]);
    if (i == field_) {
      const int w = renderer.getTextWidth(BIG_FONT, vals[i]);
      renderer.fillRect(pad + 4, rowTop + smallH + 4 + bigH + 2, w, 4, true);
    }
  }

  int y = top + F_COUNT * rowH + 8;
  renderer.drawText(SMALL, pad + 4, y, "Left / Right pick a field, Up / Down change it");
  y += smallH + 4;
  renderer.drawText(SMALL, pad + 4, y, "Hold Up / Down to move by 0.1 then 1 degree");
  y += smallH + 4;
  renderer.drawText(SMALL, pad + 4, y, "Longitude is EAST positive. Offset is standard time.");
  y += smallH + 4;
  if (dstRule_ != Location::RULE_NONE)
    renderer.drawText(SMALL, pad + 4, y, "Southern-hemisphere DST is not supported; use none.");

  if (!status_.empty())
    renderer.drawCenteredText(SMALL, pageH - m.buttonHintsHeight - m.verticalSpacing - smallH * 2 - 8,
                              status_.c_str());
  else if (dirty_)
    renderer.drawCenteredText(SMALL, pageH - m.buttonHintsHeight - m.verticalSpacing - smallH * 2 - 8,
                              "unsaved");

  const auto labels = mappedInput.mapLabels("Back", "Save", "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
