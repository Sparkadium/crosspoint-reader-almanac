#pragma once
//
// AlmanacActivity.h — the Almanac hub. One entry on the CrossPoint home screen
// that opens a submenu of the ported WatchyAlmanac modules:
//
//   Dictionary      — WCDB engine, /dictionary.cdb
//   World Factbook  — same engine, /gazetteer.cdb
//   Wikipedia       — same engine again, /wikipedia.cdb (Simple English leads)
//   Sky Chart       — 904 stars, moon phase, sunrise/sunset
//   Tsumego         — Go life-and-death problems (port pending)
//   Clock           — displays and sets the wall clock (the X4 has no RTC)
//   Location        - latitude, longitude, UTC offset and DST rule
//
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class AlmanacActivity final : public Activity {
 public:
  AlmanacActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Almanac", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Item { DICTIONARY = 0, FACTBOOK, WIKIPEDIA, SKY, TSUMEGO, CHESS, CLOCK, LOCATION, ITEM_COUNT };

  void open(Item item);
  // Read totalEntries from a WCDB header (bytes 8..11) rather than hardcoding
  // a count that goes stale whenever the data file is rebuilt. 0 = unavailable.
  static uint32_t wcdbEntryCount(const char* path);
  // "CHP1" | u16 count — same trick: read the count, never assert one.
  static uint32_t chessPuzzleCount();
  void refreshBlurbs();

  ButtonNavigator nav_;
  std::string blurbs_[ITEM_COUNT];
  int selector_ = 0;
  std::string status_;
  // Act on a release only if this activity also saw the press. A release with
  // no matching press is left over from a child activity that just closed.
  bool sawConfirmPress_ = false;
  bool sawBackPress_ = false;
};
