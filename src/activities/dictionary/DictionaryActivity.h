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
// The block index is NOT held in RAM. It is sorted by firstWord and sits on the
// SD card, so findBlock() binary-searches it in place: ~12 seeks of 44 bytes
// instead of a resident vector. On the watch that vector was ~10KB and fine; a
// Wikipedia corpus would need ~170KB of the X4's 327KB just to hold the index.
// Reading it from the card removes the corpus-size ceiling altogether, and the
// dictionary and gazetteer open faster as a side effect.
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
    std::string word;  // display title (short; stays in SSO for typical titles)
    // Body is NOT a std::string — that abort()s on the X4 when next/random
    // allocate a second heap block beside WCDB's ~70KB buffers.
    // Points into decBuf_ (valid until next decompress) or a short static.
    const char* definition = nullptr;
    size_t defLen = 0;
    bool found = false;
  };

  bool begin(const char* path);
  void end();

  bool ready() const { return ready_; }
  uint32_t entryCount() const { return totalEntries_; }
  int blockCount() const { return (int)blocks_; }

  // Exact, case-insensitive lookup. Moves the cursor on success.
  Entry lookup(const std::string& query);
  // Up to maxResults words beginning with prefix. Does not move the cursor.
  std::vector<std::string> prefixSearch(const std::string& prefix, int maxResults = 10);

  Entry currentEntry();  // entry at the cursor
  Entry next();          // advance cursor one word (wraps)
  Entry prev();          // retreat cursor one word (wraps)
  Entry randomEntry();   // random block + random line; moves the cursor

  // Phase 1b: pointer into the decompressed block for the cursor entry's body.
  // Valid until the next decompressBlock (any lookup/next/prev/random).
  // No heap copy — body can be the full WCDB record (up to ~32KB).
  bool currentBody(const char** ptr, size_t* len);

 private:
  struct BlockIdx {
    char firstWord[32];
    uint32_t offset, compSize, rawSize;
  };

  static constexpr uint32_t INDEX_OFFSET = 12;  // straight after the header
  static constexpr uint32_t RECORD_SIZE = 44;

  // The writers cap a raw block at BLOCK_SIZE. Deflate's worst case on
  // incompressible input is raw + raw/16 + 64, so a compressed block can be
  // slightly larger than a raw one. Both buffers are reserved ONCE to these
  // ceilings: with -fno-exceptions a failed allocation calls abort(), and a
  // vector that grows to a new size on every block would fragment the heap
  // until one did. Sizes read off the card are validated against them too --
  // never allocate a number that came from a file.
  static constexpr uint32_t MAX_RAW = 32768;
  static constexpr uint32_t MAX_COMP = MAX_RAW + MAX_RAW / 16 + 64;

  bool readRecord(uint32_t idx, BlockIdx& out);   // 44 bytes, straight off the card
  int findBlock(const std::string& query);        // last block whose firstWord <= query
  bool decompressBlock(int idx);                  // into decBuf_, cached
  int linesInBlock() const;                       // lines in the cached block
  Entry entryInBlock(int lineIdx) const;          // raw (redirect NOT resolved)
  Entry resolve(Entry e);                         // follow a leading '>' redirect

  HalFile file_;
  // Buffers grow to the largest block actually seen, so a corpus of small blocks
  // never pays for a corpus of large ones.
  std::vector<uint8_t> decBuf_, compBuf_;
  uint32_t decSize_ = 0;  // valid bytes in decBuf_ for cachedBlk_
  int cachedBlk_ = -1;
  int curBlk_ = 0, curLine_ = 0;
  uint32_t blocks_ = 0;
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
      : Activity(title, renderer, mappedInput),
        cdbPathOwned_(cdbPath ? cdbPath : "/dictionary.cdb"),
        titleOwned_(title ? title : "Dictionary"),
        cdbPath_(cdbPathOwned_.c_str()),
        title_(titleOwned_.c_str()) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr uint16_t LONG_PRESS_MS = 500;  // hold Confirm = random, hold Back = text size

  void setEntry(const WcdbReader::Entry& e);
  void openSearch();
  int bodyLinesPerPage() const;
  void resetPaging();
  void rebuildLineStarts(int bodyW);
  void saveResume() const;
  void tryRestoreResume();

  std::string cdbPathOwned_;
  std::string titleOwned_;
  const char* cdbPath_;
  const char* title_;

  WcdbReader dict_;
  ButtonNavigator nav_;

  // Title only; body is read live from WcdbReader::currentBody() (decBuf_).
  // Short fallback for "see: target" when the block still holds a '>' redirect.
  std::string entryWord_;
  bool entryFound_ = false;
  size_t bodyLen_ = 0;
  char fallbackBuf_[192];
  size_t fallbackLen_ = 0;

  // Byte offsets of visual lines into the body (rebuilt when width/font/entry change).
  std::vector<uint32_t> lineStarts_;
  std::string status_;  // transient message ("Not found: xyz")
  int page_ = 0;
  int pagesSinceFull_ = 0;
  int bodyW_ = 0;
  int wrapFont_ = -1;
  bool loadError_ = false;

  bool confirmHeld_ = false;
  bool confirmLongHandled_ = false;
  bool sawBackPress_ = false;
  bool backLongHandled_ = false;

  // Body text size, cycled with a long press on Back and kept in NVS. The
  // available steps depend on the build variant: only font sizes whose data
  // survives the OMIT_*_FONT flags are offered (see BODY_FONTS in the .cpp).
  uint8_t fontStep_ = 0;
  // 0=dither 1=no-dither 2=t160 3=t128 4=t96 — hold Prev to cycle
  static constexpr uint8_t IMG_MODE_COUNT = 5;
  uint8_t imgMode_ = 1;
  bool leftHeld_ = false;
  bool leftLongHandled_ = false;
  int bodyFont() const;
  void cycleFont();
  void cycleImgMode();
};
