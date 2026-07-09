#pragma once
//
// DictionaryActivity.h — offline dictionary/gazetteer for CrossPoint (Xteink X4)
// Ported from WatchyAlmanac/WatchyDict. Reads the WCDB format produced by
// prepare_dict_fat.py / make_gazetteer.py, unchanged.
//
// Place under: src/activities/dictionary/DictionaryActivity.{h,cpp}
//
// WCDB on-disk format (authoritative, from prepare_dict_fat.py::write_cdb):
//   Header  12 bytes : "WCDB" | uint32 blockCount | uint32 totalEntries   (LE)
//   Index   44 bytes x blockCount:
//              char[32] firstWord (NUL-padded)
//              uint32   offset        (absolute file offset of block data)
//              uint32   compSize      (bytes of raw-DEFLATE payload)
//              uint32   rawSize       (uncompressed size, <= 32768)
//   Blocks  : raw DEFLATE (zlib wbits -15, no header/checksum) of newline-
//             terminated "word\tdefinition\n" records, globally sorted.
//             A definition beginning with '>' is a redirect to another word.
//
// Navigation is a (block, line) cursor — the format stores no global word
// ordinals, so prev/next walk lines and roll across block boundaries exactly
// as WatchyDict's getNextEntry()/getPrevEntry() did.
//
#include <cstdint>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// ---------------------------------------------------------------------------
// WcdbReader — ported WatchyDict block engine. Display-agnostic pure logic.
// ---------------------------------------------------------------------------
class WcdbReader {
 public:
  struct Entry {
    std::string word;        // display word ("orig -> target" when redirected)
    std::string definition;  // redirect already resolved
    bool found = false;
  };

  bool begin(const char* path);
  void end();

  bool ready() const { return ready_; }
  uint32_t entryCount() const { return totalEntries_; }
  int blockCount() const { return (int)index_.size(); }

  // Exact, case-insensitive lookup. Moves the cursor on success.
  Entry lookup(const std::string& query);
  // Up to maxResults words beginning with prefix. Does not move the cursor.
  std::vector<std::string> prefixSearch(const std::string& prefix, int maxResults = 10);

  Entry currentEntry();  // entry at the cursor
  Entry next();          // advance cursor one word (wraps)
  Entry prev();          // retreat cursor one word (wraps)
  Entry randomEntry();   // random block + random line; moves the cursor

 private:
  struct BlockIdx {
    char firstWord[32];
    uint32_t offset, compSize, rawSize;
  };

  int findBlock(const std::string& query) const;  // last block whose firstWord <= query
  bool decompressBlock(int idx);                  // into decBuf_, cached
  int linesInBlock() const;                       // lines in the cached block
  Entry entryInBlock(int lineIdx) const;          // raw (redirect NOT resolved)
  Entry resolve(Entry e);                         // follow a leading '>' redirect

  HalFile file_;
  std::vector<BlockIdx> index_;
  std::vector<uint8_t> decBuf_, compBuf_;
  uint32_t decSize_ = 0;  // valid bytes in decBuf_ for cachedBlk_
  int cachedBlk_ = -1;
  int curBlk_ = 0, curLine_ = 0;
  uint32_t totalEntries_ = 0;
  bool ready_ = false;
};

// ---------------------------------------------------------------------------
// DictionaryActivity — CrossPoint UI shell
// ---------------------------------------------------------------------------
class DictionaryActivity final : public Activity {
 public:
  // One class, two modules: the gazetteer uses the identical WCDB engine,
  // exactly as the watch's curRef did. See HomeActivity::onGazetteerOpen().
  DictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                     const char* cdbPath = "/dictionary.cdb", const char* title = "Dictionary")
      : Activity(title, renderer, mappedInput), cdbPath_(cdbPath), title_(title) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr uint16_t LONG_PRESS_MS = 500;  // hold Confirm = random word

  void setEntry(const WcdbReader::Entry& e);
  void openSearch();
  int bodyLinesPerPage() const;

  const char* cdbPath_;
  const char* title_;

  WcdbReader dict_;
  ButtonNavigator nav_;

  WcdbReader::Entry entry_;
  std::vector<std::string> wrapped_;
  std::string status_;  // transient message ("Not found: xyz")
  int page_ = 0;
  bool loadError_ = false;

  bool confirmHeld_ = false;
  bool confirmLongHandled_ = false;
  bool sawBackPress_ = false;
};
