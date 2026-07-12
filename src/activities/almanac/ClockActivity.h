#pragma once
//
// ClockActivity.h — clock + time setter for the Xteink X4.
//
// The X4 has no hardware RTC (see TimeSource.h), and stock CrossPoint hides all
// clock settings on this device. This module is therefore the only way to set
// the wall-clock time, which the Sky Chart also depends on.
//
// Setting is a FIELD EDITOR, not the keyboard: there is no ':' key on the
// on-screen keyboard, and typing a timestamp with a D-pad is miserable.
//   Confirm      enter edit mode / save
//   Left, Right  move between YYYY MM DD HH MM
//   Up, Down     change the selected field (month-aware day clamping)
//   Back         cancel
//
// The time survives deep sleep but is lost on a full power-off.
//
#include "activities/Activity.h"

class ClockActivity final : public Activity {
 public:
  ClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Clock", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Field { F_YEAR = 0, F_MONTH, F_DAY, F_HOUR, F_MIN, F_COUNT };

  void beginEdit();
  void commitEdit();
  void bumpField(int delta);
  static int daysInMonth(int y, int m);

  bool editing_ = false;
  int field_ = F_DAY;
  int y_ = 2026, mo_ = 1, d_ = 1, hh_ = 0, mm_ = 0;

  std::string status_;
  // Act on a release only if this activity saw the press.
  bool sawConfirmPress_ = false;
  bool sawBackPress_ = false;
  unsigned long lastTick_ = 0;
};
