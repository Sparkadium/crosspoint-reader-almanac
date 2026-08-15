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

#include "CalcActivity.h"
#include "GlobeActivity.h"
#include "MoonActivity.h"
#include "PlanetariumActivity.h"
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

const char* NAMES[] = {"Dictionary", "Thesaurus", "Wikipedia", "World Factbook", "Globe",  "Moon",
                       "Planetarium", "Sky Chart",  "Calculator", "Chess",  "Tsumego",
                       "Clock",  "Location"};

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
  const uint32_t senses = wcdbEntryCount("/thesaurus.cdb");
  blurbs_[THESAURUS] = senses ? withCommas(senses) + " headwords"
                              : "thesaurus.cdb not on SD card";
  blurbs_[SKY] = "stars, moon phase, sun times";
  blurbs_[TSUMEGO] = "Go life-and-death problems";  // count lives in problems.bin
  const uint32_t puzzles = chessPuzzleCount();
  blurbs_[CHESS] = puzzles ? withCommas(puzzles) + " Lichess puzzles" : "chess.bin not on SD card";
  blurbs_[CALCULATOR] = "plot f(x), evaluate expressions";
  blurbs_[GLOBE] = "spin the Earth, live day and night";
  blurbs_[MOON] = "the Moon as it faces you tonight";
  blurbs_[PLANETARIUM] = "the Sun, planets, and Earth in photographs";
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
    case THESAURUS:
      status_.clear();
      startActivityForResult(
          std::make_unique<DictionaryActivity>(renderer, mappedInput, "/thesaurus.cdb", "Thesaurus"),
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
    case CALCULATOR:
      status_.clear();
      startActivityForResult(std::make_unique<CalcActivity>(renderer, mappedInput), onReturn);
      break;
    case GLOBE:
      status_.clear();
      startActivityForResult(std::make_unique<GlobeActivity>(renderer, mappedInput), onReturn);
      break;
    case PLANETARIUM:
      startActivityForResult(std::make_unique<PlanetariumActivity>(renderer, mappedInput), onReturn);
      break;
    case MOON:
      status_.clear();
      startActivityForResult(std::make_unique<MoonActivity>(renderer, mappedInput), onReturn);
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

  // Touch: swipe up/down pages the list, tap opens a row. Activation is on
  // release; see listRowTouch() for why the selector is not moved on
  // finger-down.
  {
    const ListLayout L = computeListLayout(renderer, ITEM_COUNT, selector_, /*wantBlurb=*/true);
    if (listSwipePage(mappedInput, L, ITEM_COUNT, selector_)) {
      status_.clear();
      requestUpdate();
      return;
    }
    if (listRowTouch(mappedInput, L, ITEM_COUNT, selector_)) {
      // A row tap opens a child without a Confirm press/release pair, so clear
      // the flag or the child's trailing release looks like ours.
      sawConfirmPress_ = false;
      status_.clear();
      open(static_cast<Item>(selector_));
      return;
    }
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
