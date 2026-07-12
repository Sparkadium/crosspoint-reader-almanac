// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "AlmanacActivity.h"

#include <GfxRenderer.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "ChessActivity.h"
#include "ClockActivity.h"
#include "HalStorage.h"
#include "LocationActivity.h"
#include "MappedInputManager.h"
#include "SkyActivity.h"
#include "TsumegoActivity.h"
#include "activities/dictionary/DictionaryActivity.h"
#include "ListLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"
namespace {
constexpr int ROW_FONT = BITTER_16_FONT_ID;
constexpr int SUB_FONT = SMALL_FONT_ID;

const char* NAMES[] = {"Dictionary", "World Factbook", "Wikipedia", "Sky Chart",
                       "Tsumego",    "Chess",          "Clock",     "Location"};

// Group digits: 69123 -> "69,123"
std::string withCommas(uint32_t n) {
  char raw[16];
  snprintf(raw, sizeof(raw), "%u", (unsigned)n);
  std::string in(raw), out;
  int c = 0;
  for (int i = (int)in.size() - 1; i >= 0; i--) {
    out.insert(out.begin(), in[i]);
    if (++c % 3 == 0 && i > 0) out.insert(out.begin(), ',');
  }
  return out;
}
}  // namespace

// The count lives in the file, so it can never disagree with the data.
uint32_t AlmanacActivity::wcdbEntryCount(const char* path) {
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return 0;
  uint8_t h[12];
  if (f.read(h, sizeof(h)) != (int)sizeof(h)) return 0;
  if (memcmp(h, "WCDB", 4) != 0) return 0;
  return (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
}

uint32_t AlmanacActivity::chessPuzzleCount() {
  HalFile f = Storage.open("/chess.bin", O_RDONLY);
  if (!f) f = Storage.open("/chess_full.bin", O_RDONLY);
  if (!f) return 0;
  uint8_t h[6];
  if (f.read(h, sizeof(h)) != (int)sizeof(h)) return 0;
  if (memcmp(h, "CHP1", 4) != 0) return 0;
  return (uint32_t)h[4] | ((uint32_t)h[5] << 8);
}

void AlmanacActivity::refreshBlurbs() {
  const uint32_t words = wcdbEntryCount("/dictionary.cdb");
  const uint32_t places = wcdbEntryCount("/gazetteer.cdb");
  blurbs_[DICTIONARY] = words ? withCommas(words) + " words" : "dictionary.cdb not on SD card";
  blurbs_[FACTBOOK] = places ? withCommas(places) + " countries and territories"
                             : "gazetteer.cdb not on SD card";
  const uint32_t articles = wcdbEntryCount("/wikipedia.cdb");
  blurbs_[WIKIPEDIA] = articles ? withCommas(articles) + " Simple English articles"
                                : "wikipedia.cdb not on SD card";
  blurbs_[SKY] = "stars, moon phase, sun times";
  blurbs_[TSUMEGO] = "Go life-and-death problems";  // count lives in problems.bin
  const uint32_t puzzles = chessPuzzleCount();
  blurbs_[CHESS] = puzzles ? withCommas(puzzles) + " Lichess puzzles" : "chess.bin not on SD card";
  blurbs_[CLOCK] = "set the date and time";
  blurbs_[LOCATION] = "coordinates, UTC offset, DST rule";
}

void AlmanacActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);
  sawConfirmPress_ = sawBackPress_ = false;
  refreshBlurbs();
  requestUpdate();
}

void AlmanacActivity::open(Item item) {
  auto onReturn = [this](const ActivityResult&) {
    // No guard flags needed: the child consumed the press, so its trailing
    // release arrives here without a matching press and is ignored below.
    sawConfirmPress_ = sawBackPress_ = false;
    requestUpdate(true);
  };

  switch (item) {
    case DICTIONARY:
      status_.clear();
      startActivityForResult(
          std::make_unique<DictionaryActivity>(renderer, mappedInput, "/dictionary.cdb", "Dictionary"),
          onReturn);
      break;
    case FACTBOOK:
      status_.clear();
      startActivityForResult(
          std::make_unique<DictionaryActivity>(renderer, mappedInput, "/gazetteer.cdb", "World Factbook"),
          onReturn);
      break;
    case WIKIPEDIA:
      // Same WCDB engine, third data file. make_wikipedia.py reuses the writer
      // from prepare_dict_fat.py, which is why no new module is needed.
      status_.clear();
      startActivityForResult(
          std::make_unique<DictionaryActivity>(renderer, mappedInput, "/wikipedia.cdb", "Wikipedia"),
          onReturn);
      break;
    case SKY:
      status_.clear();
      startActivityForResult(std::make_unique<SkyActivity>(renderer, mappedInput), onReturn);
      break;
    case CHESS:
      status_.clear();
      startActivityForResult(std::make_unique<ChessActivity>(renderer, mappedInput), onReturn);
      break;
    case CLOCK:
      status_.clear();
      startActivityForResult(std::make_unique<ClockActivity>(renderer, mappedInput), onReturn);
      break;
    case LOCATION:
      status_.clear();
      startActivityForResult(std::make_unique<LocationActivity>(renderer, mappedInput), onReturn);
      break;
    case TSUMEGO:
      status_.clear();
      startActivityForResult(std::make_unique<TsumegoActivity>(renderer, mappedInput), onReturn);
      break;
    default:
      break;
  }
}

void AlmanacActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) sawBackPress_ = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!sawBackPress_) return;  // leftover from a child
    sawBackPress_ = false;
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) sawConfirmPress_ = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!sawConfirmPress_) return;  // leftover from a child
    sawConfirmPress_ = false;
    open(static_cast<Item>(selector_));
    return;
  }

  // onNext/onPrevious resolve to NavNext/NavPrevious, i.e. side Down + front
  // Right and side Up + front Left, with the orientation swap applied. Using
  // raw Up/Down here is why only the side buttons moved the selector.
  bool moved = false;
  nav_.onNext([&] {
    selector_ = ButtonNavigator::nextIndex(selector_, ITEM_COUNT);
    moved = true;
  });
  nav_.onPrevious([&] {
    selector_ = ButtonNavigator::previousIndex(selector_, ITEM_COUNT);
    moved = true;
  });

  if (moved) {
    status_.clear();
    requestUpdate();
  }
}

void AlmanacActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Almanac");

  // Shared with the chess and tsumego lists: the font is measured against the
  // real space, and anything that still does not fit scrolls rather than being
  // cropped off the bottom.
  const ListLayout L = computeListLayout(renderer, ITEM_COUNT, selector_, /*wantBlurb=*/true);
  const int pad = m.contentSidePadding;

  for (int k = 0; k < L.rowsPerPage; k++) {
    const int i = L.firstVisible + k;
    if (i >= ITEM_COUNT) break;
    const bool sel = (i == selector_);
    if (sel) renderer.fillRect(pad - 6, L.rowTop(k), pageW - (pad - 6) * 2, L.boxH, true);

    renderer.drawText(L.titleFont, pad + 4, L.nameTop(k), NAMES[i], !sel);
    if (L.withBlurb)
      renderer.drawText(L.subFont, pad + 4, L.blurbTop(k), blurbs_[i].c_str(), !sel);
  }

  if (!status_.empty())
    renderer.drawCenteredText(L.subFont, L.bottom + 4, status_.c_str());
  else if (L.scrolls(ITEM_COUNT)) {
    char pos[24];
    snprintf(pos, sizeof(pos), "%d of %d", selector_ + 1, ITEM_COUNT);
    renderer.drawCenteredText(L.subFont, L.bottom + 4, pos);
  }

  const auto labels = mappedInput.mapLabels("Home", "Open", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
