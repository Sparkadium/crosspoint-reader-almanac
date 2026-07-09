#pragma once
//
// LocationActivity.h — set latitude, longitude, UTC offset and DST rule.
//
// The Sky Chart projects the sky from these coordinates, and the Clock converts
// UTC to local civil time with the offset and rule.
//
// Typing beats scrolling for a number like -75.7566, so Confirm opens the
// keyboard on the selected field. The D-pad remains for nudging a value you can
// already see. Nothing is inverted: the active field carries a caret beneath it,
// because an inverted box hides the digits you are trying to read.
//
//   Left / Right    pick a field
//   Up / Down       nudge it (hold to accelerate: 0.01 -> 0.1 -> 1 degree)
//   Confirm         type the value (DST rule cycles instead)
//   Hold Confirm    save
//   Back            leave (warns once if there are unsaved changes)
//
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class LocationActivity final : public Activity {
 public:
  LocationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Location", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Field { F_LAT = 0, F_LON, F_OFFSET, F_DST, F_COUNT };
  static constexpr uint16_t LONG_PRESS_MS = 600;

  void bump(int dir);
  void openTypeEntry();
  void applyTyped(const std::string& text);
  void commit();

  ButtonNavigator nav_;
  int field_ = F_LAT;
  double lat_ = 0, lon_ = 0;
  int32_t offMin_ = 0;
  uint8_t dstRule_ = 0;
  bool dirty_ = false;
  bool confirmedExit_ = false;  // second Back discards unsaved changes
  std::string status_;

  bool confirmHeld_ = false, confirmLong_ = false;
  bool sawBackPress_ = false;
};
