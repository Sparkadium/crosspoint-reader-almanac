// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "LocationActivity.h"

#include <GfxRenderer.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <variant>

#include "Location.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Bitter 18 and 20 report a line-height of 0 on CrossInk (their font data
// lacks the metric), which stacked every value on top of its label. 16 is the
// largest Bitter size that measures correctly, and it is barely smaller.
constexpr int BIG_FONT = BITTER_16_FONT_ID;
constexpr int SMALL = SMALL_FONT_ID;
const char* LABEL[] = {"latitude", "longitude", "utc offset", "daylight saving"};
const char* PROMPT[] = {"Latitude (e.g. 45.3475)", "Longitude, east positive (e.g. -75.7566)",
                        "UTC offset (e.g. -5:00 or -300)", ""};
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
  dirty_ = confirmedExit_ = false;
  status_.clear();
  confirmHeld_ = confirmLong_ = sawBackPress_ = false;
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
      if (lon_ > 180.0) lon_ -= 360.0;  // wrap the antimeridian
      if (lon_ < -180.0) lon_ += 360.0;
      break;
    case F_OFFSET:
      offMin_ += dir * 15;  // quarter-hour zones exist (+05:45)
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
  confirmedExit_ = false;
  status_.clear();
}

// Accepts "-75.7566" for coordinates; "-5:00", "-5", or "-300" for the offset.
void LocationActivity::applyTyped(const std::string& t) {
  if (t.empty()) return;
  switch (field_) {
    case F_LAT: {
      const double v = atof(t.c_str());
      if (v < -90.0 || v > 90.0) { status_ = "Latitude must be -90 to 90"; return; }
      lat_ = v;
      break;
    }
    case F_LON: {
      const double v = atof(t.c_str());
      if (v < -180.0 || v > 180.0) { status_ = "Longitude must be -180 to 180"; return; }
      lon_ = v;
      break;
    }
    case F_OFFSET: {
      long mins;
      const size_t colon = t.find(':');
      if (colon != std::string::npos) {                    // "-5:30"
        const long h = strtol(t.substr(0, colon).c_str(), nullptr, 10);
        const long m = strtol(t.substr(colon + 1).c_str(), nullptr, 10);
        mins = h * 60 + (h < 0 || t[0] == '-' ? -m : m);
      } else {
        const long n = strtol(t.c_str(), nullptr, 10);
        mins = (labs(n) <= 14) ? n * 60 : n;               // hours, else minutes
      }
      if (mins < -720 || mins > 840) { status_ = "Offset must be -12:00 to +14:00"; return; }
      offMin_ = (int32_t)mins;
      break;
    }
    default:
      return;
  }
  dirty_ = true;
  confirmedExit_ = false;
  status_.clear();
}

void LocationActivity::openTypeEntry() {
  char cur[24];
  switch (field_) {
    case F_LAT: snprintf(cur, sizeof(cur), "%.4f", lat_); break;
    case F_LON: snprintf(cur, sizeof(cur), "%.4f", lon_); break;
    case F_OFFSET: Location::offsetLabel(cur, sizeof(cur), offMin_);
      memmove(cur, cur + 3, strlen(cur) - 2);  // drop the "UTC" prefix, keep -05:00
      break;
    default: return;
  }

  auto handler = [this](const ActivityResult& res) {
    // The keyboard consumed the press; its trailing release arrives without a
    // matching press and is discarded by the press/release matching in loop().
    confirmHeld_ = confirmLong_ = sawBackPress_ = false;
    if (!res.isCancelled) {
      const auto* kr = std::get_if<KeyboardResult>(&res.data);
      if (kr) applyTyped(kr->text);
    }
    requestUpdate(true);
  };
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, PROMPT[field_], cur, 12),
      handler);
}

void LocationActivity::commit() {
  Location::get().lat = lat_;
  Location::get().lon = lon_;
  Location::get().baseOffsetMin = offMin_;
  Location::get().dstRule = dstRule_;
  Location::save();
  dirty_ = false;
  confirmedExit_ = false;
  status_ = "Saved";
}

void LocationActivity::loop() {
  // ---- Back: leave, but warn once if there is unsaved work ----------------
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) sawBackPress_ = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!sawBackPress_) return;  // leftover from a child
    sawBackPress_ = false;
    if (dirty_ && !confirmedExit_) {
      confirmedExit_ = true;
      status_ = "Unsaved. Hold Confirm to save, Back again to discard.";
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  // ---- Confirm: tap types the value, hold saves ---------------------------
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHeld_ = true;
    confirmLong_ = false;
  }
  if (confirmHeld_ && !confirmLong_ && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    confirmLong_ = true;
    commit();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!confirmHeld_) return;  // leftover from a child
    const bool wasLong = confirmLong_;
    confirmHeld_ = confirmLong_ = false;
    if (wasLong) return;  // the hold already saved
    if (field_ == F_DST) {
      dstRule_ = (uint8_t)((dstRule_ + 1) % 3);
      dirty_ = true;
      confirmedExit_ = false;
      status_.clear();
      requestUpdate();
    } else {
      openTypeEntry();
    }
    return;
  }

  // ---- D-pad --------------------------------------------------------------
  bool moved = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) { field_ = (field_ + 1) % F_COUNT; moved = true; }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) { field_ = (field_ - 1 + F_COUNT) % F_COUNT; moved = true; }

  // bump() reads getHeldTime() to pick its own step, so a long hold sweeps
  // whole degrees rather than hundredths.
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
  const int pad = m.contentSidePadding;

  // Fit the four rows into the space between the header and the button hints,
  // whatever the theme's metrics turn out to be. Deriving the row pitch from the
  // real measured height (rather than a fixed guess) keeps the label and value
  // from stacking on top of each other if SMALL/BIG are taller under CrossInk.
  const int top = m.topPadding + m.headerHeight + m.verticalSpacing + 12;
  const int hintsTop = pageH - m.buttonHintsHeight - m.verticalSpacing - smallH * 4 - 12;
  const int minRow = smallH + 6 + bigH + 12;         // label + gap + value + air
  int rowH = (hintsTop - top) / F_COUNT;
  if (rowH < minRow) rowH = minRow;                  // never let rows collapse

  for (int i = 0; i < F_COUNT; i++) {
    const int rowTop = top + i * rowH;
    renderer.drawText(SMALL, pad + 4, rowTop, LABEL[i]);
    const int valTop = rowTop + smallH + 6;
    renderer.drawText(BIG_FONT, pad + 4, valTop, vals[i]);
    if (i == field_) {
      const int w = renderer.getTextWidth(BIG_FONT, vals[i]);
      renderer.fillRect(pad + 4, valTop + bigH + 2, w, 4, true);
    }
  }

  int y = top + F_COUNT * rowH + 6;
  renderer.drawText(SMALL, pad + 4, y,
                    field_ == F_DST ? "Confirm cycles the rule" : "Confirm to type the value");
  y += smallH + 4;
  renderer.drawText(SMALL, pad + 4, y, "Up / Down nudge it. Hold Confirm to save.");
  y += smallH + 4;
  renderer.drawText(SMALL, pad + 4, y, "Longitude is EAST positive. Offset is standard time.");
  y += smallH + 4;
  if (dstRule_ != Location::RULE_NONE)
    renderer.drawText(SMALL, pad + 4, y, "Southern-hemisphere DST is not supported; use none.");

  const int msgY = pageH - m.buttonHintsHeight - m.verticalSpacing - smallH * 2 - 8;
  if (!status_.empty())
    renderer.drawCenteredText(SMALL, msgY, status_.c_str());
  else if (dirty_)
    renderer.drawCenteredText(SMALL, msgY, "unsaved");

  const auto labels = mappedInput.mapLabels("Back", field_ == F_DST ? "Cycle" : "Type", "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
