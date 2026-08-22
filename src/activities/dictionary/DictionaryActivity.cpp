#include <cstdlib>
// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>
#include <Esp.h>

#include "DictionaryActivity.h"

#include <GfxRenderer.h>
#include <InflateReader.h>
#include <Preferences.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <new>
#include <variant>
#include <memory>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/almanac/TouchGestures.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "Bitmap.h"
namespace {
// Body text sizes. CrossInk 1.5.0 fixed the built-in reading fonts at
// 10/12/14/16pt (lib/EpdFont/builtinFonts/all.h). Bitter 18/20 still have
// headers in the tree but are not compiled in -- they ship as SD-card fonts
// only. fontIds.h still defines every ID, so naming 18pt compiles fine and
// then draws nothing at runtime ("Font -1308817601 not found"). The old
// OMIT_*_FONT size flags this used to switch on no longer exist; only
// OMIT_EMOJI_FONTS survives.
constexpr int BODY_FONTS[] = {
    SMALL_FONT_ID,
    UI_12_FONT_ID,
    BITTER_16_FONT_ID,
};
constexpr int BODY_PTS[] = {10, 12, 16};
constexpr int FONT_STEPS = (int)(sizeof(BODY_FONTS) / sizeof(BODY_FONTS[0]));
static_assert(FONT_STEPS > 0, "every body font size was omitted from this build");
constexpr int HEAD_FONT = UI_12_FONT_ID;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

uint32_t rd32(const uint8_t* p) {  // little-endian, matches struct.pack("<I")
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Keep Wikipedia bodies from exhausting the heap when returned by value from
// lookup/next/random. DictionaryActivity also clamps; this is the first gate.
constexpr size_t kMaxEntryDef = 30000;
void clampDef(std::string& s) {
  if (s.size() <= kMaxEntryDef) return;
  s.resize(kMaxEntryDef);
  auto sp = s.find_last_of(" \t");
  if (sp != std::string::npos && sp > kMaxEntryDef * 2 / 3) s.resize(sp);
  s += "…";
}
}  // namespace

// ===========================================================================
//  WcdbReader
// ===========================================================================

static bool bodyUsable(const WcdbReader::Entry& e) {
  return e.found && e.definition && e.defLen > 0 && e.definition[0] != '>';
}

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

  // Do not reserve MAX_RAW/MAX_COMP here. 64k+68k at open() OOMs the C3
  // and reboots even for small CDBs. decompressBlock resizes to the
  // actual block (validated against MAX_RAW).

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

  // Only inflate what we will display. 60k articles were crashing; 54k is safe.
  const uint32_t want = b.rawSize > (uint32_t)kMaxEntryDef ? (uint32_t)kMaxEntryDef : b.rawSize;

  compBuf_.resize(b.compSize);
  decBuf_.resize(want);

  if (!file_.seek(b.offset)) return false;
  if (file_.read(compBuf_.data(), b.compSize) != (int)b.compSize) return false;

  // Raw DEFLATE (wbits -15): no zlib header, so do NOT call skipZlibHeader().
  InflateReader inf;
  inf.init();  // one-shot: the whole block is in memory
  inf.setSource(compBuf_.data(), b.compSize);
  // InflateReader returns false if the stream isn't fully consumed.
  // We stop at `want` on purpose for long articles — that is still success.
  const bool complete = inf.read(decBuf_.data(), (int)want);
  if (!complete && want >= b.rawSize) return false;

  decSize_ = want;
  cachedBlk_ = idx;
  compBuf_.clear();
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
  if (decSize_ == 0) return 0;
  int count = 0;
  for (uint32_t i = 0; i < decSize_; i++)
    if (decBuf_[i] == '\n') count++;
  // Truncated long articles often have no trailing newline — still one record.
  if (count == 0) return 1;
  if (decBuf_[decSize_ - 1] != '\n') count++;
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
        e.definition = reinterpret_cast<const char*>(&decBuf_[tab + 1]);
        e.defLen = (size_t)(end - tab - 1);
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
  if (!e.found || !e.definition || e.defLen == 0 || e.definition[0] != '>') return e;
  // Copy target title out of decBuf_ before lookup() overwrites the block.
  std::string target(e.definition + 1, e.defLen > 0 ? e.defLen - 1 : 0);
  while (!target.empty() && isspace((unsigned char)target.front())) target.erase(target.begin());
  while (!target.empty() && isspace((unsigned char)target.back())) target.pop_back();
  auto hash = target.find('#');
  if (hash != std::string::npos) target.resize(hash);

  const std::string original = e.word;
  Entry r = lookup(target);  // moves the cursor to the target — intended
  if (r.found) {
    r.word = original + " -> " + r.word;
    return r;
  }
  // Static buffer so we never heap-allocate a definition string.
  static char seeBuf[192];
  int n = snprintf(seeBuf, sizeof(seeBuf), "see: %s", target.c_str());
  if (n < 0) n = 0;
  e.word = original;
  e.definition = seeBuf;
  e.defLen = (size_t)std::min(n, (int)sizeof(seeBuf) - 1);
  e.found = true;
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
          e.definition = reinterpret_cast<const char*>(&decBuf_[tab + 1]);
          e.defLen = (size_t)(end - tab - 1);
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

bool WcdbReader::currentBody(const char** ptr, size_t* len) {
  if (!ptr || !len) return false;
  *ptr = nullptr;
  *len = 0;
  if (!ready_ || !decompressBlock(curBlk_)) return false;
  // Cursor already sits on the resolved target after next/prev/lookup/random.
  Entry e = entryInBlock(curLine_);
  if (!e.found || !e.definition || e.defLen == 0) return false;
  // Skip redirect stubs — caller should have resolve()'d first
  if (e.definition[0] == '>') return false;
  *ptr = e.definition;
  *len = e.defLen > kMaxEntryDef ? kMaxEntryDef : e.defLen;
  return true;
}

WcdbReader::Entry WcdbReader::next() {
  if (!ready_) return Entry{};
  for (int attempt = 0; attempt < 64; attempt++) {
    if (!decompressBlock(curBlk_)) {
      curBlk_ = (curBlk_ + 1) % (int)blocks_;
      curLine_ = 0;
      continue;
    }
    int nb = curBlk_, nl = curLine_ + 1;
    if (nl >= linesInBlock()) {
      nl = 0;
      if (++nb >= (int)blocks_) nb = 0;
    }
    if (!decompressBlock(nb)) {
      curBlk_ = nb;
      curLine_ = 0;
      continue;
    }
    Entry raw = entryInBlock(nl);
    if (!raw.found) {
      curBlk_ = nb;
      curLine_ = nl;
      continue;
    }
    curBlk_ = nb;
    curLine_ = nl;
    Entry e = resolve(raw);
    if (bodyUsable(e)) return e;
    if (e.found && e.definition && e.defLen > 0) return e;
  }
  return Entry{};
}

WcdbReader::Entry WcdbReader::prev() {
  if (!ready_) return Entry{};
  for (int attempt = 0; attempt < 64; attempt++) {
    int nb = curBlk_, nl = curLine_ - 1;
    if (nl < 0) {
      if (--nb < 0) nb = (int)blocks_ - 1;
      if (!decompressBlock(nb)) return Entry{};
      nl = linesInBlock() - 1;
    }
    if (!decompressBlock(nb)) return Entry{};
    Entry raw = entryInBlock(nl);
    if (!raw.found) return Entry{};
    curBlk_ = nb;
    curLine_ = nl;
    Entry e = resolve(raw);
    if (bodyUsable(e)) return e;
    if (e.found && e.definition && e.defLen > 0) return e;
  }
  return Entry{};
}

WcdbReader::Entry WcdbReader::randomEntry() {
  if (!ready_ || blocks_ == 0) return Entry{};
  for (int attempt = 0; attempt < 64; attempt++) {
    const int rb = (int)(esp_random() % blocks_);
    if (!decompressBlock(rb)) continue;
    const int lines = linesInBlock();
    if (lines <= 0) continue;
    const int rl = (int)(esp_random() % (uint32_t)lines);
    Entry raw = entryInBlock(rl);
    if (!raw.found) continue;
    curBlk_ = rb;
    curLine_ = rl;
    Entry e = resolve(raw);
    if (bodyUsable(e)) return e;
    if (e.found && e.definition && e.defLen > 0) return e;
  }
  return Entry{};
}

// ===========================================================================
//  DictionaryActivity
// ===========================================================================


// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Phase 3: 24-bit BMP lead images (/wiki_img/<slug>.bmp) — device-native format
// ---------------------------------------------------------------------------
namespace {

#pragma pack(push, 1)
struct BmpFileHeader {
  uint16_t magic;
  uint32_t fileSize;
  uint16_t res1;
  uint16_t res2;
  uint32_t pixelOffset;
};
struct BmpInfoHeader {
  uint32_t size;
  int32_t width;
  int32_t height;
  uint16_t planes;
  uint16_t bpp;
  uint32_t compression;
  uint32_t imageSize;
  int32_t ppmX;
  int32_t ppmY;
  uint32_t colorsUsed;
  uint32_t colorsImportant;
};
#pragma pack(pop)

bool parseImgToken(const char* s, size_t n, char* slugOut, size_t slugCap) {
  if (!s || n < 10 || slugCap < 2) return false;
  size_t st = 0;
  while (st + 6 < n) {
    if (s[st] == '@' && s[st + 1] == '@' && s[st + 2] == 'I' && s[st + 3] == 'M' &&
        s[st + 4] == 'G' && s[st + 5] == ':')
      break;
    st++;
  }
  if (st + 6 >= n) return false;
  size_t i = st + 6, j = 0;
  while (i < n && s[i] != '@' && j + 1 < slugCap) {
    char c = s[i++];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
      slugOut[j++] = c;
    else if (c == ' ' || c == '\t')
      break;
    else
      return false;
  }
  slugOut[j] = '\0';
  return j > 0 && i + 1 < n && s[i] == '@' && s[i + 1] == '@';
}




static bool findImgPath(const char* slug, char* pathOut, size_t pathCap) {
  const char* patterns[] = {
      "/wiki_img/%s.bmp", "wiki_img/%s.bmp", "/Wiki_img/%s.bmp",
      "/sd/wiki_img/%s.bmp", "/wikipedia/wiki_img/%s.bmp",
  };
  for (const char* pat : patterns) {
    snprintf(pathOut, pathCap, pat, slug);
    HalFile f = Storage.open(pathOut, O_RDONLY);
    if (f) { f.close(); return true; }
  }
  return false;
}

static void fitSize(int fullW, int fullH, int maxW, int maxH, int& w, int& h) {
  w = fullW;
  h = fullH;
  if (w > maxW) { h = h * maxW / w; w = maxW; }
  if (h > maxH) { w = w * maxH / h; h = maxH; }
  if (w < 1) w = 1;
  if (h < 1) h = 1;
}

int imgHeight(const char* slug, int maxW, int maxH) {
  char path[96];
  if (!findImgPath(slug, path, sizeof(path))) return 0;
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return 0;
  Bitmap bitmap(f, false);
  auto err = bitmap.parseHeaders();
  if (err != BmpReaderError::Ok) { f.close(); return 0; }
  int fullW = bitmap.getWidth();
  int fullH = bitmap.getHeight();
  f.close();
  if (fullW <= 0 || fullH <= 0) return 0;
  int w, h;
  fitSize(fullW, fullH, maxW, maxH, w, h);
  return h;
}

// imgMode: 0=dither 1=no-dither 2=t160 3=t128 4=t96 — hold Prev to cycle
bool drawImgAt(GfxRenderer& renderer, const char* slug, int x, int y, int maxW, int maxH,
               uint8_t imgMode) {
  char path[96];
  if (!findImgPath(slug, path, sizeof(path))) {
    renderer.fillRect(x, y, std::min(maxW, 200), 8, true);
    return false;
  }
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) {
    renderer.fillRect(x, y, std::min(maxW, 200), 8, true);
    return false;
  }

  const bool useBitmapApi = (imgMode <= 1);
  if (useBitmapApi) {
    const bool dither = (imgMode == 0);
    Bitmap bitmap(f, dither);
    auto err = bitmap.parseHeaders();
    if (err != BmpReaderError::Ok) {
      f.close();
      renderer.fillRect(x, y, std::min(maxW, 200), 8, true);
      return false;
    }
    int w, h;
    fitSize(bitmap.getWidth(), bitmap.getHeight(), maxW, maxH, w, h);
    const int x0 = x + (maxW > w ? (maxW - w) / 2 : 0);
    renderer.fillRect(x0, y, w, h, false);
    renderer.drawBitmap(bitmap, x0, y, w, h, 0.0f, 0.0f);
    f.close();
    return true;
  }

  // Manual threshold modes 2–4
  BmpFileHeader fh{};
  BmpInfoHeader ih{};
  if (f.read(reinterpret_cast<uint8_t*>(&fh), sizeof(fh)) != (int)sizeof(fh) ||
      f.read(reinterpret_cast<uint8_t*>(&ih), sizeof(ih)) != (int)sizeof(ih) ||
      fh.magic != 0x4D42 || ih.bpp != 24 || ih.compression != 0) {
    f.close();
    renderer.fillRect(x, y, std::min(maxW, 200), 8, true);
    return false;
  }
  const bool bottomUp = ih.height > 0;
  int fullW = ih.width;
  int fullH = ih.height < 0 ? -ih.height : ih.height;
  if (fullW <= 0 || fullH <= 0 || fullW > 1200 || fullH > 1600) {
    f.close();
    return false;
  }
  int w, h;
  fitSize(fullW, fullH, maxW, maxH, w, h);
  const int rowRaw = fullW * 3;
  const int rowSize = (rowRaw + 3) & ~3;
  if (rowSize > 4096) { f.close(); return false; }
  uint8_t row[4096];
  const int x0 = x + (maxW > w ? (maxW - w) / 2 : 0);
  renderer.fillRect(x0, y, w, h, false);

  int thresh = 128;
  if (imgMode == 2) thresh = 160;
  else if (imgMode == 3) thresh = 128;
  else if (imgMode == 4) thresh = 96;

  for (int screenRow = 0; screenRow < h; screenRow++) {
    int srcRow = (fullH <= 1) ? 0 : (screenRow * (fullH - 1) / (h > 1 ? h - 1 : 1));
    if (srcRow >= fullH) srcRow = fullH - 1;
    int fileRow = bottomUp ? (fullH - 1 - srcRow) : srcRow;
    uint32_t off = fh.pixelOffset + (uint32_t)fileRow * (uint32_t)rowSize;
    if (!f.seekSet(off)) break;
    if (f.read(row, rowSize) != rowSize) break;
    renderer.fillRect(x0, y + screenRow, w, 1, false);
    int runStart = -1;
    for (int col = 0; col < w; col++) {
      int srcCol = (fullW <= 1) ? 0 : (col * (fullW - 1) / (w > 1 ? w - 1 : 1));
      if (srcCol >= fullW) srcCol = fullW - 1;
      const uint8_t bb = row[srcCol * 3 + 0];
      const uint8_t gg = row[srcCol * 3 + 1];
      const uint8_t rr = row[srcCol * 3 + 2];
      const int luma = (rr * 3 + gg * 6 + bb) / 10;
      const bool black = luma < thresh;
      if (black) {
        if (runStart < 0) runStart = col;
      } else if (runStart >= 0) {
        renderer.fillRect(x0 + runStart, y + screenRow, col - runStart, 1, true);
        runStart = -1;
      }
    }
    if (runStart >= 0)
      renderer.fillRect(x0 + runStart, y + screenRow, w - runStart, 1, true);
  }
  f.close();
  return true;
}

}  // namespace



void DictionaryActivity::saveResume() const {
  if (!cdbPath_ || !cdbPath_[0]) return;
  Preferences p;
  if (p.begin("lowio_wiki", false)) {
    p.putString("resume_cdb", cdbPath_);
    p.putString("resume_word", entryWord_.c_str());
    p.putUInt("resume_page", (uint32_t)page_);
    p.end();
  }
  Storage.mkdir("/.lowio");
  HalFile f = Storage.open("/.lowio/resume.txt", O_CREAT | O_TRUNC | O_WRONLY);
  if (f) {
    // path\nword\npage\n
    f.write(reinterpret_cast<const uint8_t*>(cdbPath_), strlen(cdbPath_));
    f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    f.write(reinterpret_cast<const uint8_t*>(entryWord_.c_str()), entryWord_.size());
    f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    char pg[16];
    snprintf(pg, sizeof(pg), "%d\n", page_);
    f.write(reinterpret_cast<const uint8_t*>(pg), strlen(pg));
    f.close();
  }
}

void DictionaryActivity::tryRestoreResume() {
  std::string wantWord;
  int wantPage = 0;
  {
    Preferences p;
    if (p.begin("lowio_wiki", true)) {
      String w = p.getString("resume_word", "");
      String path = p.getString("resume_cdb", "");
      wantPage = (int)p.getUInt("resume_page", 0);
      p.end();
      if (path.length() > 0 && strcasecmp(path.c_str(), cdbPath_) == 0 && w.length() > 0) {
        wantWord = w.c_str();
      }
    }
  }
  if (wantWord.empty()) {
    HalFile f = Storage.open("/.lowio/resume.txt", O_RDONLY);
    if (f) {
      char buf[256];
      int n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
      f.close();
      if (n > 0) {
        buf[n] = 0;
        char* line1 = buf;
        char* line2 = strchr(buf, '\n');
        char* line3 = nullptr;
        if (line2) {
          *line2++ = 0;
          line3 = strchr(line2, '\n');
          if (line3) {
            *line3++ = 0;
            wantPage = atoi(line3);
          }
          if (strcasecmp(line1, cdbPath_) == 0 && line2[0]) wantWord = line2;
        }
      }
    }
  }
  if (wantWord.empty()) return;
  WcdbReader::Entry e = dict_.lookup(wantWord);
  if (e.found) {
    setEntry(e);
    // restore page after setEntry resetPaging
    page_ = wantPage;
    if (page_ < 0) page_ = 0;
  }
}


void DictionaryActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);

  {
    Preferences p;
    p.begin("almanac", true);
    fontStep_ = p.getUChar("dictfont", 0);
    imgMode_ = p.getUChar("dictimg", 1);  // default: Bitmap no-dither (lighter)
    p.end();
    if (fontStep_ >= FONT_STEPS) fontStep_ = 0;
    if (imgMode_ >= IMG_MODE_COUNT) imgMode_ = 1;
  }

  loadError_ = !dict_.begin(cdbPath_);
  confirmHeld_ = confirmLongHandled_ = sawBackPress_ = backLongHandled_ = leftHeld_ = leftLongHandled_ = false;
  if (!loadError_) {
    tryRestoreResume();
    if (entryWord_.empty()) {
      WcdbReader::Entry e = dict_.currentEntry();
      for (int i = 0; i < 32 && !bodyUsable(e); i++) {
        e = dict_.next();
      }
      setEntry(e);
    }
  }
  requestUpdate();
}

void DictionaryActivity::onExit() {
  if (!loadError_ && !entryWord_.empty()) {
    saveResume();
  }
  Activity::onExit();
  dict_.end();
  lineStarts_.clear();
}

int DictionaryActivity::bodyFont() const {
  const int f = BODY_FONTS[fontStep_ < FONT_STEPS ? fontStep_ : 0];
  // second net: if a variant strips fonts by some other mechanism, fall back
  // to the UI font rather than drawing nothing
  return renderer.getLineHeight(f) > 0 ? f : HEAD_FONT;
}

void DictionaryActivity::cycleFont() {
  fontStep_ = (uint8_t)((fontStep_ + 1) % FONT_STEPS);
  Preferences p;
  p.begin("almanac", false);
  p.putUChar("dictfont", fontStep_);
  p.end();
}

void DictionaryActivity::cycleImgMode() {
  imgMode_ = (uint8_t)((imgMode_ + 1) % IMG_MODE_COUNT);
  Preferences p;
  p.begin("almanac", false);
  p.putUChar("dictimg", imgMode_);
  p.end();
  resetPaging();
}

void DictionaryActivity::resetPaging() {
  page_ = 0;
  pagesSinceFull_ = 0;
  lineStarts_.clear();
  bodyW_ = 0;
  wrapFont_ = -1;
}

void DictionaryActivity::setEntry(const WcdbReader::Entry& e) {
  resetPaging();
  entryWord_ = e.word;
  entryFound_ = e.found;
  bodyLen_ = e.defLen;
  fallbackLen_ = 0;
  fallbackBuf_[0] = '\0';
  // Keep short bodies (e.g. "see: Foo") when currentBody() cannot read them
  // because the on-disk line is still a '>' redirect.
  if (e.definition && e.defLen > 0 && e.defLen < sizeof(fallbackBuf_)) {
    memcpy(fallbackBuf_, e.definition, e.defLen);
    fallbackLen_ = e.defLen;
    fallbackBuf_[fallbackLen_] = 0;
  }
  // Persist last article for boot resume (path + word; page updated on exit too)
  if (!entryWord_.empty()) {
    saveResume();
  }
}


void DictionaryActivity::rebuildLineStarts(int bodyW) {
  lineStarts_.clear();
  bodyW_ = bodyW;
  wrapFont_ = bodyFont();

  const char* s = nullptr;
  size_t sLen = 0;
  if (!entryFound_) {
    lineStarts_.push_back(0);
    bodyLen_ = 0;
    return;
  }
  if (!dict_.currentBody(&s, &sLen) || !s || sLen == 0) {
    if (fallbackLen_ > 0) {
      s = fallbackBuf_;
      sLen = fallbackLen_;
    } else {
      lineStarts_.push_back(0);
      bodyLen_ = 0;
      return;
    }
  }
  if (sLen > kMaxEntryDef) sLen = kMaxEntryDef;
  bodyLen_ = sLen;

  const int lh = std::max(1, renderer.getLineHeight(wrapFont_));
  const int charW = std::max(5, (lh * 2) / 5);
  const int maxChars = std::max(8, (bodyW * 9 / 10) / charW);

  auto startsWithHeading = [](const char* p, size_t n) -> bool {
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    return (n - i) >= 3 && p[i] == '#' && p[i + 1] == '#' && p[i + 2] == ' ';
  };
  auto isBulletAt = [&](size_t p) -> bool {
    return p + 2 < sLen && (unsigned char)s[p] == 0xE2 &&
           (unsigned char)s[p + 1] == 0x80 && (unsigned char)s[p + 2] == 0xA2;
  };

  size_t pos = 0;
  while (pos < sLen && lineStarts_.size() < 2500) {
    while (pos < sLen) {
      if (s[pos] == ' ' || s[pos] == '\t') { pos++; continue; }
      if (isBulletAt(pos)) { pos += 3; continue; }
      break;
    }
    if (pos >= sLen) break;

    // Image token: reserve vertical slots ≈ bitmap height / line height
    if (pos + 10 <= sLen && strncmp(s + pos, "@@IMG:", 6) == 0) {
      char slug[68];
      size_t tokEnd = pos + 6;
      while (tokEnd + 1 < sLen && !(s[tokEnd] == '@' && s[tokEnd + 1] == '@')) tokEnd++;
      if (tokEnd + 1 < sLen) tokEnd += 2;
      if (parseImgToken(s + pos, tokEnd - pos, slug, sizeof(slug))) {
        // Cap for layout: ~55% of typical content height (~400px)
        const int maxLayoutH = 400;
        int imgH = imgHeight(slug, bodyW_, maxLayoutH);
        if (imgH <= 0) imgH = lh * 3;
        const int slots = std::max(1, (imgH + lh - 1) / lh);
        for (int k = 0; k < slots && lineStarts_.size() < 2500; k++)
          lineStarts_.push_back((uint32_t)pos);
        pos = tokEnd;
        while (pos < sLen && (s[pos] == ' ' || s[pos] == '\t')) pos++;
        if (pos + 3 < sLen && s[pos] == ' ' && (unsigned char)s[pos + 1] == 0xE2) pos += 4;
        while (pos < sLen && s[pos] == ' ') pos++;
        continue;
      }
    }

    lineStarts_.push_back((uint32_t)pos);

    size_t end = std::min(sLen, pos + (size_t)maxChars);
    if (startsWithHeading(s + pos, sLen - pos)) {
      size_t hEnd = pos;
      while (hEnd < sLen && !isBulletAt(hEnd)) {
        if (s[hEnd] == '\n') break;
        hEnd++;
      }
      if (hEnd > end) end = hEnd;
    }

    // Hard break at " • " / bullet
    for (size_t hard = pos; hard + 3 <= end; hard++) {
      if (s[hard] == ' ' && isBulletAt(hard + 1)) {
        end = hard;
        break;
      }
      if (isBulletAt(hard)) {
        end = hard;
        break;
      }
    }

    if (end < sLen && end == std::min(sLen, pos + (size_t)maxChars)) {
      size_t brk = end;
      while (brk > pos && s[brk] != ' ' && s[brk] != '\t') brk--;
      if (brk > pos) end = brk;
    }
    if (end <= pos) end = std::min(sLen, pos + 1);

    pos = end;
    while (pos < sLen && (s[pos] == ' ' || s[pos] == '\t')) pos++;
  }
    // Drop leading line starts that are only whitespace/bullets (phantom gaps above images)
  auto isEmptySeg = [&](uint32_t a, uint32_t b) -> bool {
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    if (a + 2 < b && (unsigned char)s[a] == 0xE2 && (unsigned char)s[a+1] == 0x80 &&
        (unsigned char)s[a+2] == 0xA2) {
      a += 3;
      while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    }
    return a >= b;
  };
  while (lineStarts_.size() >= 2) {
    uint32_t a = lineStarts_[0];
    uint32_t b = lineStarts_[1];
    if (!isEmptySeg(a, b)) break;
    lineStarts_.erase(lineStarts_.begin());
  }

  if (lineStarts_.empty()) lineStarts_.push_back(0);
}



std::vector<WcdbReader::TitleRef> WcdbReader::listTitles(int startBlk, int startLine, int maxN) {
  std::vector<TitleRef> out;
  if (!ready_ || maxN <= 0 || blocks_ == 0) return out;
  int b = startBlk < 0 ? 0 : startBlk;
  int l = startLine;
  while (b < (int)blocks_ && (int)out.size() < maxN) {
    if (!decompressBlock(b)) {
      b++;
      l = 0;
      continue;
    }
    const int nlines = linesInBlock();
    if (l < 0) l = 0;
    while (l < nlines && (int)out.size() < maxN) {
      Entry e = entryInBlock(l);
      if (e.found && !e.word.empty() && !(e.definition && e.defLen > 0 && e.definition[0] == '>')) {
        TitleRef r;
        r.word = e.word;
        r.blk = b;
        r.line = l;
        out.push_back(std::move(r));
      }
      l++;
    }
    b++;
    l = 0;
  }
  return out;
}

bool WcdbReader::stepBackTitles(int& blk, int& line, int n) {
  if (!ready_ || n <= 0) return false;
  int b = blk;
  int l = line;
  int left = n;
  while (left > 0) {
    l--;
    if (l < 0) {
      b--;
      if (b < 0) {
        blk = 0;
        line = 0;
        return false;
      }
      if (!decompressBlock(b)) {
        blk = b;
        line = 0;
        return false;
      }
      l = linesInBlock() - 1;
      if (l < 0) continue;
    }
    left--;
  }
  blk = b;
  line = l < 0 ? 0 : l;
  return true;
}

int DictionaryActivity::bodyLinesPerPage() const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int top = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int footer = m.buttonHintsHeight + m.verticalSpacing +
                     renderer.getLineHeight(SMALL_FONT_ID) + 8;
  const int usable = renderer.getScreenHeight() - top - footer;
  return std::max(1, usable / renderer.getLineHeight(bodyFont()));
}


void DictionaryActivity::enterIndex(const std::string& prefix) {
  indexMode_ = true;
  indexSel_ = 0;
  indexHist_.clear();
  indexBlk_ = 0;
  indexLine_ = 0;
  if (!prefix.empty()) {
    indexBlk_ = std::max(0, dict_.findBlock(prefix));
    indexLine_ = 0;
  }
  fillIndexPage();
}

void DictionaryActivity::fillIndexPage() {
  indexPage_ = dict_.listTitles(indexBlk_, indexLine_, 10);
  if (!indexPage_.empty()) {
    indexEndBlk_ = indexPage_.back().blk;
    indexEndLine_ = indexPage_.back().line + 1;
  } else {
    indexEndBlk_ = indexBlk_;
    indexEndLine_ = indexLine_;
  }
  if (indexSel_ >= (int)indexPage_.size())
    indexSel_ = std::max(0, (int)indexPage_.size() - 1);
}

void DictionaryActivity::openIndexSelection() {
  if (indexPage_.empty() || indexSel_ < 0 || indexSel_ >= (int)indexPage_.size()) return;
  auto e = dict_.lookup(indexPage_[indexSel_].word);
  if (!e.found) return;
  indexMode_ = false;
  status_.clear();
  setEntry(e);
}

void DictionaryActivity::openSearch() {
  auto handler = [this](const ActivityResult& res) {
    confirmHeld_ = confirmLongHandled_ = sawBackPress_ = backLongHandled_ = false;
    if (res.isCancelled) {
      requestUpdate(true);
      return;
    }
    const auto* kr = std::get_if<KeyboardResult>(&res.data);
    if (!kr) {
      requestUpdate(true);
      return;
    }
    enterIndex(kr->text);
    requestUpdate(true);
  };
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, std::string("Search ") + title_),
      handler);
}

void DictionaryActivity::loop() {
  if (!loadError_ && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Down) {
    auto e = dict_.randomEntry();
    if (e.found) {
      status_.clear();
      setEntry(e);
      requestUpdate();
    }
    return;
  }

  if (!loadError_ && wasLongPressGesture(mappedInput, LONG_PRESS_MS)) {
    cycleFont();
    requestUpdate();
    return;
  }
  if (loadError_) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) sawBackPress_ = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && sawBackPress_) finish();
    return;
  }


  if (indexMode_) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      indexMode_ = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openIndexSelection();
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (indexSel_ + 1 < (int)indexPage_.size()) {
        indexSel_++;
      } else if (!indexPage_.empty()) {
        indexHist_.push_back({indexBlk_, indexLine_});
        if (indexHist_.size() > 80) indexHist_.erase(indexHist_.begin());
        indexBlk_ = indexEndBlk_;
        indexLine_ = indexEndLine_;
        indexSel_ = 0;
        fillIndexPage();
      }
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (indexSel_ > 0) {
        indexSel_--;
      } else if (!indexHist_.empty()) {
        auto p = indexHist_.back();
        indexHist_.pop_back();
        indexBlk_ = p.first;
        indexLine_ = p.second;
        indexSel_ = 0;
        fillIndexPage();
        if (!indexPage_.empty()) indexSel_ = (int)indexPage_.size() - 1;
      }
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (!indexPage_.empty()) {
        indexHist_.push_back({indexBlk_, indexLine_});
        if (indexHist_.size() > 80) indexHist_.erase(indexHist_.begin());
        indexBlk_ = indexEndBlk_;
        indexLine_ = indexEndLine_;
        indexSel_ = 0;
        fillIndexPage();
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (!indexHist_.empty()) {
        auto p = indexHist_.back();
        indexHist_.pop_back();
        indexBlk_ = p.first;
        indexLine_ = p.second;
        indexSel_ = 0;
        fillIndexPage();
        requestUpdate();
      }
      return;
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    sawBackPress_ = true;
    backLongHandled_ = false;
  }
  if (sawBackPress_ && !backLongHandled_ && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    backLongHandled_ = true;
    cycleFont();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!sawBackPress_) return;
    const bool wasLong = backLongHandled_;
    sawBackPress_ = backLongHandled_ = false;
    if (wasLong) return;
    finish();
    return;
  }

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
    if (!confirmHeld_) return;
    const bool wasLong = confirmLongHandled_;
    confirmHeld_ = false;
    confirmLongHandled_ = false;
    if (wasLong) return;
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

  // Left/Prev: short = previous article, hold = cycle image render mode
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    leftHeld_ = true;
    leftLongHandled_ = false;
  }
  if (leftHeld_ && !leftLongHandled_ && mappedInput.isPressed(MappedInputManager::Button::Left) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    leftLongHandled_ = true;
    cycleImgMode();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (leftHeld_) {
      const bool wasLong = leftLongHandled_;
      leftHeld_ = leftLongHandled_ = false;
      if (!wasLong) goPrev();
    }
  }

  nav_.onPress({MappedInputManager::Button::Right}, goNext);
  nav_.onContinuous({MappedInputManager::Button::Right}, goNext);

  const int perPage = bodyLinesPerPage();
  auto pageForward = [&] {
    if ((page_ + 1) * perPage < (int)lineStarts_.size()) {
      page_++; pagesSinceFull_++;
      moved = true;
    }
  };
  auto pageBack = [&] {
    if (page_ > 0) {
      page_--; pagesSinceFull_++;
      moved = true;
    }
  };

  if (mappedInput.hasTouch()) {
    nav_.onPress({MappedInputManager::Button::Down}, goNext);
    nav_.onPress({MappedInputManager::Button::Up}, goPrev);
    nav_.onContinuous({MappedInputManager::Button::Down}, goNext);
    nav_.onContinuous({MappedInputManager::Button::Up}, goPrev);
    switch (mappedInput.wasSwipe()) {
      case MappedInputManager::SwipeDir::Left: pageForward(); break;
      case MappedInputManager::SwipeDir::Right: pageBack(); break;
      default: break;
    }
  } else {
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) pageForward();
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) pageBack();
  }

  if (moved) requestUpdate();
}

void DictionaryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (indexMode_ && !loadError_) {
    const auto& m = UITheme::getInstance().getMetrics();
    const int pageW = renderer.getScreenWidth();
    const int pageH = renderer.getScreenHeight();
    GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Index");
    const int sidePad = m.contentSidePadding + 4;
    int y = m.topPadding + m.headerHeight + m.verticalSpacing;
    const int lh = std::max(1, renderer.getLineHeight(UI_12_FONT_ID));
    const int yMax = pageH - m.buttonHintsHeight - 8;
    if (indexPage_.empty()) {
      renderer.drawText(UI_12_FONT_ID, sidePad, y, "(no titles)");
    } else {
      for (int i = 0; i < (int)indexPage_.size(); i++) {
        if (y + lh > yMax) break;
        const bool sel = (i == indexSel_);
        const char* w = indexPage_[i].word.c_str();
        if (sel) {
          renderer.fillRect(sidePad / 2, y - 1, pageW - sidePad, lh + 2, true);
        }
        renderer.drawText(UI_12_FONT_ID, sidePad, y, w, sel);
        y += lh + 2;
      }
    }
    GUI.drawButtonHints(renderer, "Back", "Open", "Up", "Down");
    renderer.displayBuffer();
    return;
  }


  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  if (loadError_) {
    renderer.drawCenteredText(HEAD_FONT, pageH / 2 - 20, title_);
    std::string msg = "No wiki data";
    renderer.drawCenteredText(SMALL_FONT_ID, pageH / 2 + 10, msg.c_str());
    GUI.drawButtonHints(renderer, "Back", "", "", "");
    renderer.displayBuffer();
    return;
  }

  const char* title = !status_.empty() ? status_.c_str()
                                       : (entryFound_ ? entryWord_.c_str() : "(no entry)");
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, title);

  const int contentTop = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int sidePad = m.contentSidePadding + 4;
  const int bodyW = pageW - sidePad * 2;
  const int lh = std::max(1, renderer.getLineHeight(bodyFont()));
  const int perPage = bodyLinesPerPage();
  const int yMax = pageH - m.buttonHintsHeight - m.verticalSpacing -
                   renderer.getLineHeight(SMALL_FONT_ID) - 6;

  const char* body = nullptr;
  size_t bodyN = 0;
  bool haveBody = false;
  if (entryFound_ && dict_.currentBody(&body, &bodyN) && body && bodyN > 0) {
    haveBody = true;
  } else if (fallbackLen_ > 0) {
    body = fallbackBuf_;
    bodyN = fallbackLen_;
    haveBody = true;
  }

  if (haveBody && (lineStarts_.empty() || bodyW_ != bodyW || wrapFont_ != bodyFont()))
    rebuildLineStarts(bodyW);

  int y = contentTop;
  const int start = page_ * perPage;
  const int font = bodyFont();

  // Empty lineStarts before the first real content used to push the image down
  // (and move it when font size changed maxChars / phantom lines). Skip those.
  bool sawContent = false;
  for (int i = start; i < start + perPage && i < (int)lineStarts_.size(); i++) {
    if (!haveBody) break;
    if (y + lh > yMax) break;
    const uint32_t a = lineStarts_[i];
    const uint32_t b = (i + 1 < (int)lineStarts_.size()) ? lineStarts_[i + 1] : (uint32_t)bodyN;
    uint32_t from = a;
    while (from < b && (body[from] == ' ' || body[from] == '\t')) from++;

    if (from + 2 < b && (unsigned char)body[from] == 0xE2 &&
        (unsigned char)body[from + 1] == 0x80 && (unsigned char)body[from + 2] == 0xA2) {
      from += 3;
      while (from < b && (body[from] == ' ' || body[from] == '\t')) from++;
      if (from >= b) { if (sawContent) y += lh; continue; }
    }
    uint32_t to = b;
    while (to > from && (body[to - 1] == ' ' || body[to - 1] == '\t')) to--;
    while (to >= from + 3 &&
           (unsigned char)body[to - 3] == 0xE2 &&
           (unsigned char)body[to - 2] == 0x80 &&
           (unsigned char)body[to - 1] == 0xA2) {
      to -= 3;
      while (to > from && (body[to - 1] == ' ' || body[to - 1] == '\t')) to--;
    }
    if (to <= from) { if (sawContent) y += lh; continue; }

    const size_t n = (size_t)(to - from);
    char buf[512];
    const size_t copy = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
    memcpy(buf, body + from, copy);
    buf[copy] = '\0';

    char slug[68];
    if (parseImgToken(buf, copy, slug, sizeof(slug)) || strstr(buf, "@@IMG:") != nullptr) {
      if (!parseImgToken(buf, copy, slug, sizeof(slug))) { if (sawContent) y += lh; continue; }
      const int remain = yMax - y;
      // Prefer ~45% of the content column so text still fits on page 1
      const int budget = std::min(remain - lh, (yMax - contentTop) * 75 / 100);
      if (budget < lh * 3) {
        // Not enough room — skip image on this page (avoid clipped strip / huge hole)
        continue;
      }
      const int maxImgH = std::max(lh * 4, budget);
      const bool ok = drawImgAt(renderer, slug, sidePad, y, bodyW, maxImgH, imgMode_);
      int drawnH = ok ? imgHeight(slug, bodyW, maxImgH) : 0;
      if (ok && drawnH > 0) {
        y += drawnH + 4;
        sawContent = true;
      }
      continue;
    }

    if (copy >= 3 && buf[0] == '#' && buf[1] == '#' && buf[2] == ' ') {
      const char* draw = buf + 3;
      size_t dlen = strlen(draw);
      while (dlen > 0 && (draw[dlen - 1] == ' ' || draw[dlen - 1] == '\t')) dlen--;
      char hbuf[512];
      if (dlen >= sizeof(hbuf)) dlen = sizeof(hbuf) - 1;
      memcpy(hbuf, draw, dlen);
      hbuf[dlen] = '\0';
      const int hlh = std::max(1, renderer.getLineHeight(HEAD_FONT));
      const int charW = std::max(5, (hlh * 2) / 5);
      const int maxHChars = std::max(4, bodyW / charW);
      if (i > start) y += (lh * 2) / 3;
      size_t hpos = 0;
      const size_t hlen = strlen(hbuf);
      while (hpos < hlen) {
        if (y + hlh > yMax) break;
        size_t hend = std::min(hlen, hpos + (size_t)maxHChars);
        if (hend < hlen) {
          size_t brk = hend;
          while (brk > hpos && hbuf[brk] != ' ' && hbuf[brk] != '\t') brk--;
          if (brk > hpos) hend = brk;
        }
        while (hpos < hend && (hbuf[hpos] == ' ' || hbuf[hpos] == '\t')) hpos++;
        if (hpos >= hend) { hpos = std::min(hlen, hpos + 1); continue; }
        char linebuf[512];
        size_t ln = hend - hpos;
        if (ln >= sizeof(linebuf)) ln = sizeof(linebuf) - 1;
        memcpy(linebuf, hbuf + hpos, ln);
        linebuf[ln] = '\0';
        renderer.drawText(HEAD_FONT, sidePad, y, linebuf);
        y += hlh;
        sawContent = true;
        hpos = hend;
        while (hpos < hlen && (hbuf[hpos] == ' ' || hbuf[hpos] == '\t')) hpos++;
      }
      y += lh / 3;
      continue;
    }

    renderer.drawText(font, sidePad, y, buf);
    y += lh;
    sawContent = true;
  }

  const auto labels = mappedInput.mapLabels("Home", "Search", "Prev", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  char foot[64];
  snprintf(foot, sizeof(foot), "Search: index   hold Search: random %dpt",
           BODY_PTS[fontStep_ < FONT_STEPS ? fontStep_ : 0]);
  renderer.drawText(SMALL_FONT_ID, sidePad,
                    pageH - m.buttonHintsHeight - m.verticalSpacing - renderer.getLineHeight(SMALL_FONT_ID),
                    foot);

  const int totalPages = std::max(1, ((int)lineStarts_.size() + perPage - 1) / perPage);
  if (totalPages > 1) {
    char pbuf[24];
    snprintf(pbuf, sizeof(pbuf), "%d/%d", page_ + 1, totalPages);
    renderer.drawText(SMALL_FONT_ID, pageW - sidePad - 40,
                      pageH - m.buttonHintsHeight - m.verticalSpacing - renderer.getLineHeight(SMALL_FONT_ID),
                      pbuf);
  }

  renderer.displayBuffer();
}

