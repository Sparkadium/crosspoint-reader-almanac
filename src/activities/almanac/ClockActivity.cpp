// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "ClockActivity.h"

#include <GfxRenderer.h>

#include <cstdio>
#include <new>
#include <string>
#include <variant>

#include "Location.h"
#include "MappedInputManager.h"
#include "TimeSource.h"
#include "components/UITheme.h"
#include "fontIds.h"
namespace {
// Bitter 18/20 report line-height 0 on CrossInk; 16 is the largest that works.
constexpr int BIG_FONT = BITTER_16_FONT_ID;
constexpr int MID_FONT = BITTER_16_FONT_ID;
constexpr int SMALL = SMALL_FONT_ID;
const char* MON[12] = {"January", "February", "March",     "April",   "May",      "June",
                       "July",    "August",   "September", "October", "November", "December"};
}  // namespace

int ClockActivity::daysInMonth(int y, int m) {
  static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return dm[m - 1];
}

void ClockActivity::onEnter() {
  Activity::onEnter();
  TimeSource::begin();  // seeds the system clock from the RTC, on boards that have one
  sawConfirmPress_ = sawBackPress_ = false;
  lastTick_ = millis();
  requestUpdate();
}


void ClockActivity::beginEdit() {
  if (TimeSource::isSet()) {
    TimeSource::localPartsMin(TimeSource::nowUtc(), y_, mo_, d_, hh_, mm_);
  } else if (TimeSource::lastKnownUtc() >= TimeSource::MIN_VALID) {
    // Not the current time -- elapsed time is unrecoverable without a clock --
    // but starting from what was last entered beats starting from 2026-01-01.
    TimeSource::localPartsMin(TimeSource::lastKnownUtc(), y_, mo_, d_, hh_, mm_);
  } else {
    y_ = 2026; mo_ = 1; d_ = 1; hh_ = 0; mm_ = 0;
  }
  field_ = F_YEAR;
  editing_ = true;
  status_.clear();
}

void ClockActivity::commitEdit() {
  TimeSource::setUtc(TimeSource::epochFromLocalParts(y_, mo_, d_, hh_, mm_));
  editing_ = false;
  status_ = "Time set";
  lastTick_ = millis();
}

void ClockActivity::bumpField(int delta) {
  switch (field_) {
    case F_YEAR: y_ += delta; if (y_ < 2025) y_ = 2025; if (y_ > 2099) y_ = 2099; break;
    case F_MONTH: mo_ += delta; if (mo_ < 1) mo_ = 12; if (mo_ > 12) mo_ = 1; break;
    case F_DAY: d_ += delta; break;
    case F_HOUR: hh_ += delta; if (hh_ < 0) hh_ = 23; if (hh_ > 23) hh_ = 0; break;
    case F_MIN: mm_ += delta; if (mm_ < 0) mm_ = 59; if (mm_ > 59) mm_ = 0; break;
    default: break;
  }
  const int dim = daysInMonth(y_, mo_);  // keep the day legal after any change
  if (d_ < 1) d_ = dim;
  if (d_ > dim) d_ = 1;
}

void ClockActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) sawBackPress_ = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!sawBackPress_) return;  // leftover from the Almanac menu
    sawBackPress_ = false;
    if (editing_) {  // cancel the edit, stay in the clock
      editing_ = false;
      status_.clear();
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) sawConfirmPress_ = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!sawConfirmPress_) return;  // leftover from the Almanac menu
    sawConfirmPress_ = false;
    if (editing_) commitEdit(); else beginEdit();
    requestUpdate();
    return;
  }

  if (editing_) {
    bool moved = false;
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      field_ = (field_ + 1) % F_COUNT; moved = true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      field_ = (field_ - 1 + F_COUNT) % F_COUNT; moved = true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up))   { bumpField(+1); moved = true; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) { bumpField(-1); moved = true; }
    if (moved) requestUpdate();
    return;
  }

  // Repaint once a minute so the shown time stays honest. E-ink: deliberately
  // lazy rather than a per-second tick.
  if (TimeSource::isSet() && millis() - lastTick_ > 60000UL) {
    lastTick_ = millis();
    status_.clear();
    requestUpdate();
  }
}

void ClockActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Clock");

  if (editing_) {
    // The active field is marked with a caret UNDER it, never by inverting it:
    // an inverted box can hide the very digit you are trying to read while you
    // change it. Each field also carries a label, so nothing is guessed at.
    char parts[F_COUNT][8];
    snprintf(parts[F_YEAR], 8, "%04d", y_);
    snprintf(parts[F_MONTH], 8, "%02d", mo_);
    snprintf(parts[F_DAY], 8, "%02d", d_);
    snprintf(parts[F_HOUR], 8, "%02d", hh_);
    snprintf(parts[F_MIN], 8, "%02d", mm_);
    static const char* SEP[F_COUNT] = {"-", "-", "   ", ":", ""};
    static const char* LABEL[F_COUNT] = {"year", "month", "day", "hour", "min"};

    const int bigH = renderer.getLineHeight(BIG_FONT);
    const int smallH = renderer.getLineHeight(SMALL);

    // Each field's column must be as wide as the WIDER of its value and its
    // label -- the labels ("month", "year") are wider than the two-digit values,
    // so sizing columns to the value alone let a label spill into its neighbour.
    // That was the "jumble of overlapping letters".
    int fieldW[F_COUNT];
    int totalW = 0;
    for (int i = 0; i < F_COUNT; i++) {
      const int vw = renderer.getTextWidth(BIG_FONT, parts[i]);
      const int lw = renderer.getTextWidth(SMALL, LABEL[i]);
      fieldW[i] = (lw > vw ? lw : vw) + 10;  // a little breathing room
      totalW += fieldW[i] + renderer.getTextWidth(BIG_FONT, SEP[i]);
    }

    const int labelTop = pageH / 2 - 60;
    const int valueTop = labelTop + smallH + 6;

    int x = (pageW - totalW) / 2;
    for (int i = 0; i < F_COUNT; i++) {
      const int w = fieldW[i];

      // label, centred in the column
      const int lw = renderer.getTextWidth(SMALL, LABEL[i]);
      renderer.drawText(SMALL, x + (w - lw) / 2, labelTop, LABEL[i]);

      // value, centred in the column, always plain black
      const int vw = renderer.getTextWidth(BIG_FONT, parts[i]);
      renderer.drawText(BIG_FONT, x + (w - vw) / 2, valueTop, parts[i]);

      // caret under the active field
      if (i == field_) {
        const int cy = valueTop + bigH + 4;
        renderer.fillRect(x, cy, w, 4, true);
        renderer.drawText(SMALL, x + (w - renderer.getTextWidth(SMALL, "+")) / 2, cy + 8, "+");
      }

      x += w;
      const int sepTop = valueTop;
      renderer.drawText(BIG_FONT, x, sepTop, SEP[i]);
      x += renderer.getTextWidth(BIG_FONT, SEP[i]);
    }

    const int hintTop = valueTop + bigH + 40;
    renderer.drawCenteredText(SMALL, hintTop, "Left / Right pick a field");
    renderer.drawCenteredText(SMALL, hintTop + smallH + 4, "Up / Down change it");
    renderer.drawCenteredText(SMALL, hintTop + (smallH + 4) * 2,
                              "Local time, 24-hour. Set the zone in Location.");

    const auto labels = mappedInput.mapLabels("Cancel", "Save", "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (!TimeSource::isSet()) {
    const int lh = renderer.getLineHeight(SMALL);
    renderer.drawCenteredText(MID_FONT, pageH / 2 - 30, "Time not set");
    const char* l1; const char* l2;
    switch (TimeSource::rtcStatus()) {
      case TimeSource::Clock::Ready:
        l1 = "The clock chip has never been set."; l2 = ""; break;
      case TimeSource::Clock::NoChip:
        l1 = "This device has no clock chip."; l2 = "Sleep clears the time."; break;
      default:  // NoLibrary: an X3 lands here if the build lacks the RTC flag
        l1 = "Built without RTC support."; l2 = "On X3, add the Rtc build flag."; break;
    }
    renderer.drawCenteredText(SMALL, pageH / 2 + 4, l1);
    renderer.drawCenteredText(SMALL, pageH / 2 + 4 + lh + 4, "Press Set to enter the date and time.");
    if (l2[0])
      renderer.drawCenteredText(SMALL, pageH / 2 + 4 + (lh + 4) * 2, l2);
  } else {
    int y, mo, d, hh, mm;
    const int offMin = TimeSource::localPartsMin(TimeSource::nowUtc(), y, mo, d, hh, mm);

    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", hh, mm);
    renderer.drawCenteredText(BIG_FONT, pageH / 2 - 40, timeBuf);

    char dateBuf[48];
    snprintf(dateBuf, sizeof(dateBuf), "%s %d, %d", MON[mo - 1], d, y);
    renderer.drawCenteredText(MID_FONT, pageH / 2 + 10, dateBuf);

    // No hardcoded timezone: report the offset actually in use, and whether the
    // configured DST rule is currently adding an hour.
    char tz[40], offBuf[16];
    Location::offsetLabel(offBuf, sizeof(offBuf), offMin);
    snprintf(tz, sizeof(tz), "%s%s", offBuf,
             Location::dstActive(y, mo, d, hh) ? "  (DST)" : "");
    renderer.drawCenteredText(SMALL, pageH / 2 + 10 + renderer.getLineHeight(MID_FONT) + 6, tz);
    // Say only what is true of THIS board. Sleep cuts power to the MCU, so a
    // device with no clock chip genuinely loses the time; one with an RTC keeps it.
    const char* foot;
    switch (TimeSource::rtcStatus()) {
      case TimeSource::Clock::Ready: foot = "Kept by the clock chip, even when off."; break;
      case TimeSource::Clock::NoChip: foot = "Cleared when the device sleeps."; break;
      default: foot = "No RTC support in this build."; break;
    }
    renderer.drawCenteredText(
        SMALL, pageH - m.buttonHintsHeight - m.verticalSpacing - renderer.getLineHeight(SMALL) - 4, foot);
  }

  if (!status_.empty()) renderer.drawCenteredText(SMALL, pageH / 2 + 90, status_.c_str());

  const auto labels = mappedInput.mapLabels("Back", "Set", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
