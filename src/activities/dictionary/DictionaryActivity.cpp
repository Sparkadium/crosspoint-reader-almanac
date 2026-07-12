// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "DictionaryActivity.h"

#include <GfxRenderer.h>
#include <InflateReader.h>
#include <Preferences.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>
#include <variant>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
namespace {
// Body text can be 12, 14 or 16pt. Wikipedia leads and Factbook entries are long
// enough that 16pt cost real paging; 12pt is the default now. All three are
// registered in main.cpp -- an unregistered font id draws nothing at all.
constexpr int BODY_FONTS[] = {NOTOSERIF_12_FONT_ID, NOTOSERIF_14_FONT_ID, NOTOSERIF_16_FONT_ID};
constexpr int FONT_STEPS = 3;
constexpr int HEAD_FONT = UI_12_FONT_ID;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}
uint32_t rd32(const uint8_t* p) {  // little-endian, matches struct.pack('<I')
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
}  // namespace

// ===========================================================================
//  WcdbReader
// ===========================================================================

bool WcdbReader::begin(const char* path) {
  end();
  file_ = Storage.open(path, O_RDONLY);
  if (!file_) return false;

  uint8_t header[12];
  if (file_.read(header, sizeof(header)) != (int)sizeof(header)) return false;
  if (memcmp(header, "WCDB", 4) != 0) return false;
  blocks_ = rd32(header + 4);
  totalEntries_ = rd32(header + 8);
  if (blocks_ == 0) return false;

  // Sanity: the index must actually fit inside the file.
  const size_t need = INDEX_OFFSET + (size_t)blocks_ * RECORD_SIZE;
  if (file_.size() < need) return false;

  // The index stays on the card. Reserve the working buffers once, to their
  // fixed ceilings: resize() inside an existing capacity never reallocates, so
  // jumping between blocks of different sizes cannot fragment the heap.
  decBuf_.reserve(MAX_RAW);
  compBuf_.reserve(MAX_COMP);
  if (decBuf_.capacity() < MAX_RAW || compBuf_.capacity() < MAX_COMP) {
    end();  // not enough contiguous heap; fail cleanly rather than abort() later
    return false;
  }

  curBlk_ = curLine_ = 0;
  ready_ = true;
  return true;
}

// One 44-byte record, read in place. Called ~12 times per binary search.
bool WcdbReader::readRecord(uint32_t idx, BlockIdx& out) {
  if (idx >= blocks_) return false;
  if (!file_.seek(INDEX_OFFSET + (size_t)idx * RECORD_SIZE)) return false;
  uint8_t rec[RECORD_SIZE];
  if (file_.read(rec, RECORD_SIZE) != (int)RECORD_SIZE) return false;
  memcpy(out.firstWord, rec, 32);
  out.firstWord[31] = '\0';  // the writer caps the word at 31 bytes + NUL padding
  out.offset = rd32(rec + 32);
  out.compSize = rd32(rec + 36);
  out.rawSize = rd32(rec + 40);
  return true;
}

void WcdbReader::end() {
  if (file_) file_.close();
  decBuf_.clear();
  compBuf_.clear();
  decBuf_.shrink_to_fit();
  compBuf_.shrink_to_fit();
  decSize_ = 0;
  cachedBlk_ = -1;
  curBlk_ = curLine_ = 0;
  blocks_ = 0;
  totalEntries_ = 0;
  ready_ = false;
}

bool WcdbReader::decompressBlock(int idx) {
  if (idx == cachedBlk_) return true;
  if (idx < 0 || (uint32_t)idx >= blocks_) return false;

  BlockIdx b;
  if (!readRecord((uint32_t)idx, b)) return false;

  // These three numbers came off an SD card. Validate them before they are used
  // to size an allocation or a read: a corrupt record must fail this block, not
  // take the firmware down with it.
  if (b.rawSize == 0 || b.rawSize > MAX_RAW) return false;
  if (b.compSize == 0 || b.compSize > MAX_COMP) return false;
  if ((uint64_t)b.offset + b.compSize > (uint64_t)file_.size()) return false;

  compBuf_.resize(b.compSize);  // within the reserved capacity: no reallocation
  decBuf_.resize(b.rawSize);

  if (!file_.seek(b.offset)) return false;
  if (file_.read(compBuf_.data(), b.compSize) != (int)b.compSize) return false;

  // Raw DEFLATE (wbits -15): no zlib header, so do NOT call skipZlibHeader().
  InflateReader inf;
  if (!inf.init(false)) return false;  // one-shot: the whole block is in memory
  inf.setSource(compBuf_.data(), b.compSize);
  if (!inf.read(decBuf_.data(), b.rawSize)) return false;

  decSize_ = b.rawSize;
  cachedBlk_ = idx;
  return true;
}

// Binary search the on-card index. Same result as the RAM version; ~12 seeks.
int WcdbReader::findBlock(const std::string& query) {
  const std::string q = lower(query);
  int lo = 0, hi = (int)blocks_ - 1, result = 0;
  BlockIdx b;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (!readRecord((uint32_t)mid, b)) break;
    if (lower(b.firstWord) <= q) {
      result = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return result;
}

int WcdbReader::linesInBlock() const {
  int count = 0;
  for (uint32_t i = 0; i < decSize_; i++)
    if (decBuf_[i] == '\n') count++;
  return count;
}

WcdbReader::Entry WcdbReader::entryInBlock(int lineIdx) const {
  Entry e;
  uint32_t pos = 0;
  int line = 0;
  while (pos < decSize_) {
    uint32_t end = pos;
    while (end < decSize_ && decBuf_[end] != '\n') end++;
    if (line == lineIdx) {
      uint32_t tab = pos;
      while (tab < end && decBuf_[tab] != '\t') tab++;
      if (tab < end) {
        e.word.assign(reinterpret_cast<const char*>(&decBuf_[pos]), tab - pos);
        e.definition.assign(reinterpret_cast<const char*>(&decBuf_[tab + 1]), end - tab - 1);
        e.found = true;
      }
      return e;
    }
    pos = end + 1;
    line++;
  }
  return e;
}

// A definition of ">target" redirects; mirrors WatchyDict::showEntry().
WcdbReader::Entry WcdbReader::resolve(Entry e) {
  if (!e.found || e.definition.empty() || e.definition[0] != '>') return e;
  std::string target = e.definition.substr(1);
  while (!target.empty() && isspace((unsigned char)target.front())) target.erase(target.begin());
  while (!target.empty() && isspace((unsigned char)target.back())) target.pop_back();

  const std::string original = e.word;
  Entry r = lookup(target);  // moves the cursor to the target — intended
  if (r.found) {
    r.word = original + " -> " + r.word;
    return r;
  }
  e.definition = "see: " + target;
  return e;
}

WcdbReader::Entry WcdbReader::lookup(const std::string& query) {
  Entry miss;
  if (!ready_) return miss;
  const std::string q = lower(query);
  const int blk = findBlock(q);

  // The target may sit just past a block boundary; check neighbours, as on watch.
  for (int b = std::max(0, blk - 1); b <= std::min(blk + 1, (int)blocks_ - 1); b++) {
    if (!decompressBlock(b)) continue;
    uint32_t pos = 0;
    int line = 0;
    while (pos < decSize_) {
      uint32_t end = pos;
      while (end < decSize_ && decBuf_[end] != '\n') end++;
      uint32_t tab = pos;
      while (tab < end && decBuf_[tab] != '\t') tab++;
      if (tab < end) {
        std::string w(reinterpret_cast<const char*>(&decBuf_[pos]), tab - pos);
        const std::string wl = lower(w);
        if (wl == q) {
          Entry e;
          e.word = std::move(w);
          e.definition.assign(reinterpret_cast<const char*>(&decBuf_[tab + 1]), end - tab - 1);
          e.found = true;
          curBlk_ = b;
          curLine_ = line;
          return e;  // caller resolves redirects
        }
        if (wl > q) return miss;  // sorted: we've passed it
      }
      pos = end + 1;
      line++;
    }
  }
  return miss;
}

std::vector<std::string> WcdbReader::prefixSearch(const std::string& prefix, int maxResults) {
  std::vector<std::string> out;
  if (!ready_ || prefix.empty()) return out;
  const std::string p = lower(prefix);
  for (int b = findBlock(p); b < (int)blocks_ && (int)out.size() < maxResults; b++) {
    if (!decompressBlock(b)) break;
    uint32_t pos = 0;
    while (pos < decSize_ && (int)out.size() < maxResults) {
      uint32_t end = pos;
      while (end < decSize_ && decBuf_[end] != '\n') end++;
      uint32_t tab = pos;
      while (tab < end && decBuf_[tab] != '\t') tab++;
      if (tab < end) {
        std::string w(reinterpret_cast<const char*>(&decBuf_[pos]), tab - pos);
        const std::string wl = lower(w);
        if (wl.compare(0, p.size(), p) == 0)
          out.push_back(std::move(w));
        else if (wl > p)
          return out;
      }
      pos = end + 1;
    }
  }
  return out;
}

WcdbReader::Entry WcdbReader::currentEntry() {
  if (!ready_ || !decompressBlock(curBlk_)) return Entry{};
  return resolve(entryInBlock(curLine_));
}

WcdbReader::Entry WcdbReader::next() {
  if (!ready_ || !decompressBlock(curBlk_)) return Entry{};
  int nb = curBlk_, nl = curLine_ + 1;
  if (nl >= linesInBlock()) {
    nl = 0;
    if (++nb >= (int)blocks_) nb = 0;
  }
  if (!decompressBlock(nb)) return Entry{};
  Entry e = entryInBlock(nl);
  if (e.found) {
    curBlk_ = nb;
    curLine_ = nl;
  }
  return resolve(e);
}

WcdbReader::Entry WcdbReader::prev() {
  if (!ready_) return Entry{};
  int nb = curBlk_, nl = curLine_ - 1;
  if (nl < 0) {
    if (--nb < 0) nb = (int)blocks_ - 1;
    if (!decompressBlock(nb)) return Entry{};
    nl = linesInBlock() - 1;
  }
  if (!decompressBlock(nb)) return Entry{};
  Entry e = entryInBlock(nl);
  if (e.found) {
    curBlk_ = nb;
    curLine_ = nl;
  }
  return resolve(e);
}

WcdbReader::Entry WcdbReader::randomEntry() {
  if (!ready_ || blocks_ == 0) return Entry{};
  const int rb = (int)(esp_random() % blocks_);
  if (!decompressBlock(rb)) return Entry{};
  const int lines = linesInBlock();
  if (lines <= 0) return Entry{};
  const int rl = (int)(esp_random() % (uint32_t)lines);
  Entry e = entryInBlock(rl);
  if (e.found) {
    curBlk_ = rb;
    curLine_ = rl;
  }
  return resolve(e);
}

// ===========================================================================
//  DictionaryActivity
// ===========================================================================

void DictionaryActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);

  {
    Preferences p;
    p.begin("almanac", true);
    fontStep_ = p.getUChar("dictfont", 0);  // 12pt by default
    p.end();
    if (fontStep_ >= FONT_STEPS) fontStep_ = 0;
  }

  loadError_ = !dict_.begin(cdbPath_);
  confirmHeld_ = confirmLongHandled_ = sawBackPress_ = backLongHandled_ = false;
  if (!loadError_) setEntry(dict_.currentEntry());
  requestUpdate();
}

void DictionaryActivity::onExit() {
  Activity::onExit();
  dict_.end();
  wrapped_.clear();
}

int DictionaryActivity::bodyFont() const {
  return BODY_FONTS[fontStep_ < FONT_STEPS ? fontStep_ : 0];
}

void DictionaryActivity::cycleFont() {
  fontStep_ = (uint8_t)((fontStep_ + 1) % FONT_STEPS);
  Preferences p;
  p.begin("almanac", false);
  p.putUChar("dictfont", fontStep_);
  p.end();
  page_ = 0;
  wrapped_.clear();  // re-wrap at the new size
}

void DictionaryActivity::setEntry(const WcdbReader::Entry& e) {
  entry_ = e;
  page_ = 0;
  wrapped_.clear();  // re-wrapped in render(), where the width is known
}

int DictionaryActivity::bodyLinesPerPage() const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int top = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int usable = renderer.getScreenHeight() - top - m.buttonHintsHeight - m.verticalSpacing;
  return std::max(1, usable / renderer.getLineHeight(bodyFont()));
}

void DictionaryActivity::openSearch() {
  auto handler = [this](const ActivityResult& res) {
    // The keyboard consumed the press; its trailing release reaches us without
    // a matching press and is discarded by the press/release matching in loop().
    confirmHeld_ = confirmLongHandled_ = sawBackPress_ = backLongHandled_ = false;
    if (res.isCancelled) {
      requestUpdate(true);
      return;
    }
    const auto* kr = std::get_if<KeyboardResult>(&res.data);
    if (!kr || kr->text.empty()) {
      requestUpdate(true);
      return;
    }
    const std::string typed = kr->text;

    WcdbReader::Entry e = dict_.lookup(typed);
    if (e.found) {
      status_.clear();
      setEntry(e);
    } else {
      // Fall back to the first prefix match, as the watch's match-select did.
      auto matches = dict_.prefixSearch(typed, 1);
      if (!matches.empty()) {
        status_.clear();
        setEntry(dict_.lookup(matches[0]));
      } else {
        status_ = "Not found: " + typed;
      }
    }
    requestUpdate(true);
  };
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, std::string("Search ") + title_),
      handler);
}

void DictionaryActivity::loop() {
  if (loadError_) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) sawBackPress_ = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && sawBackPress_) finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    sawBackPress_ = true;
    backLongHandled_ = false;
  }
  if (sawBackPress_ && !backLongHandled_ && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    backLongHandled_ = true;  // hold Back = smaller/larger text
    cycleFont();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!sawBackPress_) return;  // leftover from the keyboard
    const bool wasLong = backLongHandled_;
    sawBackPress_ = backLongHandled_ = false;
    if (wasLong) return;  // the hold already changed the size
    finish();
    return;
  }

  // Hold Confirm = random word. Short press = search.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHeld_ = true;
    confirmLongHandled_ = false;
  }
  if (confirmHeld_ && !confirmLongHandled_ && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    confirmLongHandled_ = true;
    auto e = dict_.randomEntry();
    if (e.found) {
      status_.clear();
      setEntry(e);
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!confirmHeld_) return;  // leftover from the keyboard
    const bool wasLong = confirmLongHandled_;
    confirmHeld_ = false;
    confirmLongHandled_ = false;
    if (wasLong) return;  // the long-press already served a random word
    openSearch();
    return;
  }

  bool moved = false;
  auto goNext = [&] {
    auto e = dict_.next();
    if (e.found) {
      status_.clear();
      setEntry(e);
      moved = true;
    }
  };
  auto goPrev = [&] {
    auto e = dict_.prev();
    if (e.found) {
      status_.clear();
      setEntry(e);
      moved = true;
    }
  };

  nav_.onPress({MappedInputManager::Button::Right}, goNext);
  nav_.onPress({MappedInputManager::Button::Left}, goPrev);
  nav_.onContinuous({MappedInputManager::Button::Right}, goNext);
  nav_.onContinuous({MappedInputManager::Button::Left}, goPrev);

  const int perPage = bodyLinesPerPage();
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if ((page_ + 1) * perPage < (int)wrapped_.size()) {
      page_++;
      moved = true;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (page_ > 0) {
      page_--;
      moved = true;
    }
  }

  if (moved) requestUpdate();
}

void DictionaryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  if (loadError_) {
    renderer.drawCenteredText(HEAD_FONT, pageH / 2 - 20, title_);
    std::string msg = std::string("Copy ") + (cdbPath_ + 1) + " to the SD card root";
    renderer.drawCenteredText(SMALL_FONT_ID, pageH / 2 + 10, msg.c_str());
    GUI.drawButtonHints(renderer, "Back", "", "", "");
    renderer.displayBuffer();
    return;
  }

  const char* title = !status_.empty() ? status_.c_str()
                                       : (entry_.found ? entry_.word.c_str() : "(no entry)");
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, title);

  const int contentTop = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int sidePad = m.contentSidePadding;
  const int bodyW = pageW - sidePad * 2;
  const int lh = renderer.getLineHeight(bodyFont());
  const int perPage = bodyLinesPerPage();

  if (wrapped_.empty() && entry_.found)
    wrapped_ = renderer.wrappedText(bodyFont(), entry_.definition.c_str(), bodyW, 4096);

  // drawText's y is the TOP of the line (it adds the ascender internally).
  int y = contentTop;
  const int start = page_ * perPage;
  for (int i = start; i < start + perPage && i < (int)wrapped_.size(); i++) {
    renderer.drawText(bodyFont(), sidePad, y, wrapped_[i].c_str());
    y += lh;
  }

  const auto labels = mappedInput.mapLabels("Home", "Search", "Prev", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // The long-press affordance has nowhere to live in the hint bar without
  // colliding with its neighbours, so state it once, quietly, on the left.
  char foot[64];
  snprintf(foot, sizeof(foot), "hold Search: random   hold Home: text size %dpt",
           fontStep_ == 0 ? 12 : (fontStep_ == 1 ? 14 : 16));
  renderer.drawText(SMALL_FONT_ID, sidePad,
                    pageH - m.buttonHintsHeight - m.verticalSpacing - renderer.getLineHeight(SMALL_FONT_ID),
                    foot);

  const int totalPages = std::max(1, ((int)wrapped_.size() + perPage - 1) / perPage);
  if (totalPages > 1) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d/%d", page_ + 1, totalPages);
    renderer.drawText(SMALL_FONT_ID, pageW - sidePad - 40,
                      pageH - m.buttonHintsHeight - m.verticalSpacing - renderer.getLineHeight(SMALL_FONT_ID),
                      buf);
  }

  renderer.displayBuffer();
}
