#pragma once
//
// LocationActivity.h — set latitude, longitude, UTC offset and DST rule.
//
// The Sky Chart projects the sky from these coordinates, and the Clock converts
// UTC to local civil time with the offset and rule. Both were hardcoded to
// Ottawa/Eastern in the original watch firmware.
//
// Fields are edited with a caret under the active one, never by inverting it —
// an inverted box can hide the digits you are trying to read while changing them.
//
//   Left / Right   pick a field
//   Up / Down      change it (hold to accelerate: 0.01 -> 0.1 -> 1 degree)
//   Confirm        save
//   Back           discard and leave
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

  void bump(int dir);

  ButtonNavigator nav_;
  int field_ = F_LAT;
  double lat_ = 0, lon_ = 0;
  int32_t offMin_ = 0;
  uint8_t dstRule_ = 0;
  bool dirty_ = false;
  std::string status_;

  bool sawConfirmPress_ = false;
  bool sawBackPress_ = false;
};
