// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "TsumegoActivity.h"

#include <GfxRenderer.h>
#include <esp_random.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <variant>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "ListLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"
namespace {
constexpr int TITLE_FONT = UI_12_FONT_ID;
constexpr int SMALL = SMALL_FONT_ID;
constexpr int ROW_FONT = BITTER_16_FONT_ID;
constexpr bool BLACK = true;
constexpr bool WHITE = false;

void fillCircleR(const GfxRenderer& r, int cx, int cy, int rad, bool state) {
  for (int dy = -rad; dy <= rad; dy++) {
    const int dx = (int)lround(sqrt((double)rad * rad - (double)dy * dy));
    if (dx > 0) r.drawLine(cx - dx, cy + dy, cx + dx, cy + dy, state);
  }
}
void drawCircleR(const GfxRenderer& r, int cx, int cy, int rad, bool state) {
  int x = rad, y = 0, err = 1 - rad;
  while (x >= y) {
    r.drawPixel(cx + x, cy + y, state); r.drawPixel(cx + y, cy + x, state);
    r.drawPixel(cx - y, cy + x, state); r.drawPixel(cx - x, cy + y, state);
    r.drawPixel(cx - x, cy - y, state); r.drawPixel(cx - y, cy - x, state);
    r.drawPixel(cx + y, cy - x, state); r.drawPixel(cx + x, cy - y, state);
    y++;
    if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
  }
}

const char* MENU_LABELS[] = {"Next unsolved",  "Random problem",  "Browse to problem",
                             "Type problem number", "Problem sets", "Show solution",
                             "Restart problem"};
constexpr int MENU_COUNT = 7;
}  // namespace

// ===========================================================================
//  Lifecycle
// ===========================================================================

void TsumegoActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);
  confirmHeld = confirmLong = backHeld = backLong = false;

  binFile = Storage.open("/problems.bin", O_RDONLY);
  if (!binFile || binFile.size() < 6) { loadError = true; requestUpdate(); return; }

  uint8_t hdr[7];
  binFile.seek(0);
  binFile.read(hdr, 7);
  const bool v2 = memcmp(hdr, "TSU2", 4) == 0;
  if (!v2 && memcmp(hdr, "TSU1", 4) != 0) { loadError = true; requestUpdate(); return; }
  problemCount = hdr[4] | (hdr[5] << 8);
  if (problemCount == 0) { loadError = true; requestUpdate(); return; }

  offsetsBase = 6;
  nSets = 0;
  if (v2) {
    const uint8_t total = hdr[6];
    uint32_t p = 7;
    for (uint8_t i = 0; i < total; i++) {
      binFile.seek(p);
      uint8_t ln;
      binFile.read(&ln, 1);
      if (nSets < 16) {
        const uint8_t rd = ln > 24 ? 24 : ln;
        binFile.read((uint8_t*)setNames[nSets], rd);
        setNames[nSets][rd] = 0;
        binFile.seek(p + 1 + ln);
        uint8_t se[2];
        binFile.read(se, 2);
        setStarts[nSets] = se[0] | (se[1] << 8);
        nSets++;
      }
      p += 1 + (uint32_t)ln + 2;
    }
    offsetsBase = p;
  }

  prefs.begin("tsumego");
  solvedCount = prefs.getUShort("solved", 0);
  memset(solvedMap, 0, sizeof(solvedMap));
  prefs.getBytes("map", solvedMap, sizeof(solvedMap));

  uint16_t cur = prefs.getUShort("cur", 0);
  if (cur >= problemCount) cur = 0;
  if (!loadGo(cur)) { loadError = true; }
  requestUpdate();
}

void TsumegoActivity::onExit() {
  Activity::onExit();
  if (binFile) binFile.close();
  prefs.end();
}

// ===========================================================================
//  Problem loading (TSU2 record parse, unchanged)
// ===========================================================================

bool TsumegoActivity::loadGo(uint16_t idx) {
  if (idx >= problemCount) return false;
  binFile.seek(offsetsBase + 4UL * idx);
  uint32_t off = 0, next = 0;
  binFile.read(&off, 4);
  if (idx + 1 < problemCount) binFile.read(&next, 4);
  else next = binFile.size();
  blobLen = (uint16_t)std::min<uint32_t>(MAX_BLOB, next - off);
  binFile.seek(off);
  binFile.read(blob, blobLen);

  uint16_t p = 0;
  flags = blob[p++]; W = blob[p++]; H = blob[p++];
  memset(board, 0, sizeof(board));
  uint8_t nb = blob[p++];
  for (uint8_t i = 0; i < nb; i++) board[blob[p++]] = 1;
  uint8_t nw = blob[p++];
  for (uint8_t i = 0; i < nw; i++) board[blob[p++]] = 2;
  treeStart = p;

  toPlay = (flags & F_WHITE_TO_PLAY) ? 2 : 1;
  curList = treeStart;
  histLen = 0;
  mode = PLAYING;
  koPos = -1;
  lastMove = -1;
  probIdx = idx;
  cursor = (H / 2) * W + W / 2;
  return true;
}

// ===========================================================================
//  Go rules — transplanted verbatim from tsumego.h
// ===========================================================================

bool TsumegoActivity::openSide(int c, int r) const {
  if (c < 0) return !(flags & F_WALL_L);
  if (r < 0) return !(flags & F_WALL_T);
  if (c >= W) return !(flags & F_WALL_R);
  if (r >= H) return !(flags & F_WALL_B);
  return false;
}

int TsumegoActivity::floodLibs(uint8_t pos) {
  const uint8_t color = board[pos];
  memset(seen, 0, sizeof(seen));
  grpN = 0;
  int libs = 0;
  uint8_t stack[252];
  int sp = 0;
  stack[sp++] = pos;
  seen[pos] = true;
  while (sp) {
    const uint8_t q = stack[--sp];
    grp[grpN++] = q;
    const int c = q % W, r = q / W;
    const int dc[] = {1, -1, 0, 0}, dr[] = {0, 0, 1, -1};
    for (int k = 0; k < 4; k++) {
      const int nc = c + dc[k], nr = r + dr[k];
      if (nc < 0 || nr < 0 || nc >= W || nr >= H) {
        if (openSide(nc, nr)) libs += 100;  // open board = ample liberties
        continue;
      }
      const uint8_t np = nr * W + nc;
      if (board[np] == 0) libs++;
      else if (board[np] == color && !seen[np]) { seen[np] = true; stack[sp++] = np; }
    }
  }
  return libs;
}

bool TsumegoActivity::applyMove(uint8_t pos, uint8_t color, bool fromTree) {
  if (pos == PASS_POS) { koPos = -1; lastMove = -1; return true; }
  if (board[pos] != 0) return false;
  if (!fromTree && (int16_t)pos == koPos) return false;
  board[pos] = color;
  const uint8_t enemy = 3 - color;
  int captured = 0;
  uint8_t capPos = 0;
  const int c = pos % W, r = pos / W;
  const int dc[] = {1, -1, 0, 0}, dr[] = {0, 0, 1, -1};
  for (int k = 0; k < 4; k++) {
    const int nc = c + dc[k], nr = r + dr[k];
    if (nc < 0 || nr < 0 || nc >= W || nr >= H) continue;
    const uint8_t np = nr * W + nc;
    if (board[np] == enemy && floodLibs(np) == 0) {
      for (uint8_t i = 0; i < grpN; i++) board[grp[i]] = 0;
      captured += grpN;
      capPos = grp[0];
    }
  }
  if (floodLibs(pos) == 0) { board[pos] = 0; return false; }  // suicide
  koPos = (captured == 1 && grpN == 1 && floodLibs(pos) == 1) ? capPos : -1;
  lastMove = pos;
  return true;
}

uint16_t TsumegoActivity::skipSubtree(uint16_t nodeOff) {
  uint16_t p = nodeOff + 2;
  while (blob[p] != POP) p = skipSubtree(p);
  return p + 1;
}

uint16_t TsumegoActivity::findChild(uint16_t list, uint8_t pos) {
  uint16_t p = list;
  while (blob[p] != POP && blob[p] != END_TREE) {
    if (blob[p] == pos) return p;
    p = skipSubtree(p);
  }
  return 0;
}

void TsumegoActivity::replay() {
  memset(board, 0, sizeof(board));
  uint16_t q = 3;
  uint8_t nb = blob[q++];
  for (uint8_t i = 0; i < nb; i++) board[blob[q++]] = 1;
  uint8_t nw = blob[q++];
  for (uint8_t i = 0; i < nw; i++) board[blob[q++]] = 2;
  curList = treeStart;
  koPos = -1;
  lastMove = -1;
  uint8_t color = toPlay;
  for (uint8_t i = 0; i < histLen; i++) {
    applyMove(hist[i], color, true);
    const uint16_t n = findChild(curList, hist[i]);
    if (n) curList = n + 2;
    color = 3 - color;
  }
  mode = PLAYING;
}

void TsumegoActivity::userMove(uint8_t pos) {
  const uint16_t node = findChild(curList, pos);
  if (!node) {  // off-tree: show it, mark wrong
    if (!applyMove(pos, toPlay, false)) return;  // illegal: ignore
    offtreePos = pos;
    mode = OFFTREE;
    return;
  }
  if (!applyMove(pos, toPlay, true)) return;
  pushHist(pos);
  curList = node + 2;
  if (blob[node + 1] == 1) { solve(); return; }
  if (terminal(node)) { mode = FAILED; return; }

  const uint16_t reply = node + 2;  // engine reply: mainline child
  applyMove(blob[reply], 3 - toPlay, true);
  pushHist(blob[reply]);
  curList = reply + 2;
  if (blob[reply + 1] == 1) { solve(); return; }
  if (terminal(reply)) { mode = FAILED; return; }
}

void TsumegoActivity::undoTurn() {
  if (mode == OFFTREE) { replay(); return; }  // off-tree was never in hist
  if (histLen == 0) return;
  if (histLen & 1) histLen -= 1;   // lone user move
  else histLen -= 2;               // user move + engine reply
  replay();
}

uint16_t TsumegoActivity::countSolved(uint16_t a, uint16_t b) const {
  uint16_t n = 0;
  for (uint16_t i = a; i < b; i++)
    if (isSolved(i)) n++;
  return n;
}

uint8_t TsumegoActivity::setOf(uint16_t idx) const {
  uint8_t s = 0;
  for (uint8_t i = 0; i < nSets; i++)
    if (setStarts[i] <= idx) s = i;
  return s;
}

uint16_t TsumegoActivity::setEnd(uint8_t s) const {
  return (s + 1 < nSets) ? setStarts[s + 1] : problemCount;
}

void TsumegoActivity::solve() {
  mode = SOLVED;
  if (!isSolved(probIdx)) {
    solvedMap[probIdx >> 3] |= 1 << (probIdx & 7);
    prefs.putBytes("map", solvedMap, (problemCount + 7) / 8);
    solvedCount++;
    prefs.putUShort("solved", solvedCount);
  }
}

int8_t TsumegoActivity::findRight(uint16_t list, uint8_t depth) {
  if (depth >= 32) return -1;
  uint16_t p = list;
  while (blob[p] != POP && blob[p] != END_TREE) {
    solPath[depth] = blob[p];
    if (blob[p + 1] == 1) return depth + 1;
    const int8_t r = findRight(p + 2, depth + 1);
    if (r > 0) return r;
    p = skipSubtree(p);
  }
  return -1;
}

// Non-blocking replacement for the watch's delay(800) playback loop.
void TsumegoActivity::startReveal() {
  loadGo(probIdx);
  revealLen = findRight(treeStart, 0);
  if (revealLen <= 0) { requestUpdate(); return; }
  mode = REVEALED;
  revealAt = 0;
  revealColor = toPlay;
  revealNext = millis() + REVEAL_STEP_MS;
  requestUpdate();
}

void TsumegoActivity::jumpTo(uint16_t idx) {
  loadGo(idx);
  prefs.putUShort("cur", probIdx);
}

void TsumegoActivity::jumpToSet(uint8_t s) {
  const uint16_t a = setStarts[s], b = setEnd(s);
  uint16_t t = a;
  for (uint16_t i = a; i < b; i++)
    if (!isSolved(i)) { t = i; break; }
  jumpTo(t);
}

void TsumegoActivity::jumpNextUnsolved() {
  for (uint16_t k = 1; k <= problemCount; k++) {
    const uint16_t i = (probIdx + k) % problemCount;
    if (!isSolved(i)) { jumpTo(i); return; }
  }
}

void TsumegoActivity::jumpRandom() { jumpTo((uint16_t)(esp_random() % problemCount)); }

// ===========================================================================
//  Input
// ===========================================================================

// The watch had four buttons, so its cursor could only step to the "next empty
// point" or drop a row. With a D-pad we move freely; illegal placements are
// simply rejected by applyMove(), which is the same rule either way.
void TsumegoActivity::moveCursor(int dc, int dr) {
  int c = cursor % W, r = cursor / W;
  c = (c + dc + W) % W;
  r = (r + dr + H) % H;
  cursor = (uint8_t)(r * W + c);
}

void TsumegoActivity::openMenu() {
  resumeMode = (mode < MENU) ? mode : PLAYING;
  mode = MENU;
  menuSel = 0;
}

void TsumegoActivity::runMenuItem(int item) {
  switch (item) {
    case 0: jumpNextUnsolved(); break;
    case 1: jumpRandom(); break;
    case 2: scrubVal = probIdx; mode = SCRUB; return;
    case 3: openNumberEntry(); return;
    case 4: setSel = setOf(probIdx); mode = SETMENU; return;
    case 5: startReveal(); return;
    case 6: loadGo(probIdx); break;
    default: break;
  }
  mode = PLAYING;
}

// With 11,814 problems, nudging a scrubber is no way to reach P9042. The
// keyboard has digits, so let the number be typed outright.
void TsumegoActivity::openNumberEntry() {
  auto handler = [this](const ActivityResult& res) {
    // The keyboard consumed the press; its trailing release arrives without a
    // matching press and is discarded by the press/release matching in loop().
    confirmHeld = confirmLong = backHeld = backLong = false;
    if (!res.isCancelled) {
      const auto* kr = std::get_if<KeyboardResult>(&res.data);
      if (kr && !kr->text.empty()) {
        long n = strtol(kr->text.c_str(), nullptr, 10);
        if (n >= 1 && n <= (long)problemCount) {
          jumpTo((uint16_t)(n - 1));  // shown 1-based, stored 0-based
          mode = PLAYING;
          requestUpdate(true);
          return;
        }
      }
    }
    mode = MENU;
    requestUpdate(true);
  };
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Problem number", "", 5),
      handler);
}

void TsumegoActivity::loop() {
  if (loadError) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backHeld = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backHeld) finish();
    return;
  }

  // ---- timed solution playback -------------------------------------------
  if (mode == REVEALED && revealAt < revealLen) {
    if (millis() >= revealNext) {
      applyMove(solPath[revealAt], revealColor, true);
      revealColor = 3 - revealColor;
      revealAt++;
      revealNext = millis() + REVEAL_STEP_MS;
      requestUpdate();
    }
    return;  // ignore input mid-playback
  }

  // ---- Back: hold = menu, tap = undo / exit -------------------------------
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { backHeld = true; backLong = false; }
  if (backHeld && !backLong && mode < MENU && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() > 700) {
    backLong = true;
    openMenu();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!backHeld) return;  // leftover from a child
    const bool wasLong = backLong;
    backHeld = backLong = false;
    if (wasLong) return;
    switch (mode) {
      case MENU: mode = resumeMode; requestUpdate(); return;
      case SETMENU:
      case SCRUB: mode = MENU; requestUpdate(); return;
      case OFFTREE:
      case FAILED: undoTurn(); requestUpdate(); return;
      case REVEALED: loadGo(probIdx); requestUpdate(); return;
      default:
        if (histLen > 0) { undoTurn(); requestUpdate(); return; }
        finish();
        return;
    }
  }

  // ---- menus ---------------------------------------------------------------
  if (mode == SETMENU && nSets == 0) {  // TSU1 file: nothing to choose from
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      mode = MENU;
      requestUpdate();
    }
    return;
  }

  if (mode == MENU || mode == SETMENU) {
    const int count = (mode == MENU) ? MENU_COUNT : nSets;
    int& sel = (mode == MENU) ? menuSel : setSel;
    bool moved = false;
    nav_.onNext([&] { sel = ButtonNavigator::nextIndex(sel, count); moved = true; });
    nav_.onPrevious([&] { sel = ButtonNavigator::previousIndex(sel, count); moved = true; });
    if (moved) { requestUpdate(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHeld = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!confirmHeld) return;  // leftover from a child
      confirmHeld = false;
      if (mode == MENU) runMenuItem(menuSel);
      else { jumpToSet((uint8_t)setSel); mode = PLAYING; }
      requestUpdate();
    }
    return;
  }

  if (mode == SCRUB) {
    bool moved = false;
    auto bump = [&](long delta) {
      long v = (long)scrubVal + delta;
      v %= (long)problemCount;
      if (v < 0) v += problemCount;
      scrubVal = (uint16_t)v;
      moved = true;
    };
    // Hold to accelerate: 1 -> 10 -> 100 -> 1000. Up/Down jump a hundred at a
    // time so a five-figure library is actually traversable.
    const unsigned long held = mappedInput.getHeldTime();
    const long step = held > 5000 ? 1000 : (held > 3000 ? 100 : (held > 1200 ? 10 : 1));
    nav_.onPressAndContinuous({MappedInputManager::Button::Right}, [&] { bump(step); });
    nav_.onPressAndContinuous({MappedInputManager::Button::Left}, [&] { bump(-step); });
    nav_.onPressAndContinuous({MappedInputManager::Button::Down}, [&] { bump(100); });
    nav_.onPressAndContinuous({MappedInputManager::Button::Up}, [&] { bump(-100); });
    if (moved) { requestUpdate(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHeld = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!confirmHeld) return;  // leftover from a child
      confirmHeld = false;
      jumpTo(scrubVal);
      mode = PLAYING;
      requestUpdate();
    }
    return;
  }

  // ---- playing -------------------------------------------------------------
  bool moved = false;
  nav_.onPressAndContinuous({MappedInputManager::Button::Right}, [&] { moveCursor(1, 0); moved = true; });
  nav_.onPressAndContinuous({MappedInputManager::Button::Left}, [&] { moveCursor(-1, 0); moved = true; });
  nav_.onPressAndContinuous({MappedInputManager::Button::Down}, [&] { moveCursor(0, 1); moved = true; });
  nav_.onPressAndContinuous({MappedInputManager::Button::Up}, [&] { moveCursor(0, -1); moved = true; });
  if (moved && mode == PLAYING) { requestUpdate(); return; }

  // Confirm: tap = place, hold = pass. In SOLVED, tap advances.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) { confirmHeld = true; confirmLong = false; }
  if (confirmHeld && !confirmLong && mode == PLAYING &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    confirmLong = true;
    userMove(PASS_POS);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!confirmHeld) return;  // leftover from a child
    const bool wasLong = confirmLong;
    confirmHeld = confirmLong = false;
    if (wasLong) return;
    switch (mode) {
      case SOLVED: jumpNextUnsolved(); break;
      case REVEALED: loadGo(probIdx); break;
      case OFFTREE:
      case FAILED: undoTurn(); break;
      default: userMove(cursor); break;
    }
    requestUpdate();
  }
}

// ===========================================================================
//  Rendering
// ===========================================================================

void TsumegoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  if (loadError) {
    renderer.drawCenteredText(TITLE_FONT, pageH / 2 - 20, "Tsumego problems not found");
    renderer.drawCenteredText(SMALL, pageH / 2 + 10, "Copy problems.bin to the SD card root");
    GUI.drawButtonHints(renderer, "Back", "", "", "");
    renderer.displayBuffer();
    return;
  }

  if (mode == MENU) { drawMenu(); return; }
  if (mode == SETMENU) { drawSetMenu(); return; }
  if (mode == SCRUB) { drawScrub(); return; }

  char title[64];
  snprintf(title, sizeof(title), "P%u%s   %s to play", (unsigned)(probIdx + 1),
           isSolved(probIdx) ? " *" : "", toPlay == 1 ? "black" : "white");
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, title);

  drawBoard();

  const int statusY = pageH - m.buttonHintsHeight - m.verticalSpacing - renderer.getLineHeight(SMALL) - 4;
  char status[64];
  switch (mode) {
    case PLAYING:
      snprintf(status, sizeof(status), "%u moves    %u of %u solved", (unsigned)histLen,
               (unsigned)solvedCount, (unsigned)problemCount);
      break;
    case OFFTREE:
      snprintf(status, sizeof(status), "%s", (lastMove < 0 && histLen == 0) ? "Pass is not it"
                                                                            : "Not the move");
      break;
    case FAILED: snprintf(status, sizeof(status), "Refuted"); break;
    case SOLVED: snprintf(status, sizeof(status), "Solved"); break;
    case REVEALED: snprintf(status, sizeof(status), "Solution"); break;
    default: status[0] = 0; break;
  }
  renderer.drawCenteredText(SMALL, statusY, status);

  // The menu was reachable but invisible: nothing on screen said so.
  renderer.drawCenteredText(SMALL, statusY - renderer.getLineHeight(SMALL) - 2,
                            "hold Back for menu, sets and problem jump");

  const char* confirmLabel = (mode == SOLVED) ? "Next" : (mode == PLAYING ? "Place" : "Undo");
  const char* backLabel = (mode == PLAYING && histLen == 0) ? "Back" : "Undo";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "Move", "Move");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Ghosting control, as on the watch: a full waveform every 20 partials.
  if (++partialCount >= 20) {
    partialCount = 0;
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  } else {
    renderer.displayBuffer();
  }
}

void TsumegoActivity::drawBoard() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  const int top = m.topPadding + m.headerHeight + m.verticalSpacing + 10;
  const int bottom = pageH - m.buttonHintsHeight - m.verticalSpacing - 40;
  const int availW = pageW - m.contentSidePadding * 2;
  const int availH = bottom - top;

  const int pitch = std::min(56, std::min(availW / std::max<int>(W, 1), availH / std::max<int>(H, 1)));
  const int x0 = (pageW - (W - 1) * pitch) / 2;
  const int y0 = top + (availH - (H - 1) * pitch) / 2;
  const int ext = pitch / 2;  // continuation stub on open sides

  for (uint8_t c = 0; c < W; c++) {
    const int x = x0 + c * pitch;
    const int yA = (flags & F_WALL_T) ? y0 : y0 - ext;
    const int yB = (flags & F_WALL_B) ? y0 + (H - 1) * pitch : y0 + (H - 1) * pitch + ext;
    renderer.drawLine(x, yA, x, yB, BLACK);
  }
  for (uint8_t r = 0; r < H; r++) {
    const int y = y0 + r * pitch;
    const int xA = (flags & F_WALL_L) ? x0 : x0 - ext;
    const int xB = (flags & F_WALL_R) ? x0 + (W - 1) * pitch : x0 + (W - 1) * pitch + ext;
    renderer.drawLine(xA, y, xB, y, BLACK);
  }
  // thick board edges on wall sides
  if (flags & F_WALL_L) renderer.drawLine(x0 - 2, y0 - 2, x0 - 2, y0 + (H - 1) * pitch + 2, 2, BLACK);
  if (flags & F_WALL_T) renderer.drawLine(x0 - 2, y0 - 2, x0 + (W - 1) * pitch + 2, y0 - 2, 2, BLACK);
  if (flags & F_WALL_R)
    renderer.drawLine(x0 + (W - 1) * pitch + 2, y0 - 2, x0 + (W - 1) * pitch + 2, y0 + (H - 1) * pitch + 2, 2, BLACK);
  if (flags & F_WALL_B)
    renderer.drawLine(x0 - 2, y0 + (H - 1) * pitch + 2, x0 + (W - 1) * pitch + 2, y0 + (H - 1) * pitch + 2, 2, BLACK);

  const int rad = std::max(4, pitch / 2 - 1);
  for (uint8_t r = 0; r < H; r++)
    for (uint8_t c = 0; c < W; c++) {
      const uint8_t v = board[r * W + c];
      if (!v) continue;
      const int x = x0 + c * pitch, y = y0 + r * pitch;
      if (v == 1) {
        fillCircleR(renderer, x, y, rad, BLACK);
      } else {
        fillCircleR(renderer, x, y, rad, WHITE);
        drawCircleR(renderer, x, y, rad, BLACK);
      }
    }

  if (lastMove >= 0 && board[lastMove]) {
    const int x = x0 + (lastMove % W) * pitch, y = y0 + (lastMove / W) * pitch;
    const bool inv = board[lastMove] == 1 ? WHITE : BLACK;
    renderer.drawRect(x - 4, y - 4, 9, 9, inv);
  }

  if (mode == PLAYING) {  // cursor: four corner ticks
    const int x = x0 + (cursor % W) * pitch, y = y0 + (cursor / W) * pitch;
    const int t = rad + 2, k = 6;
    renderer.drawLine(x - t, y - t, x - t + k, y - t, BLACK);
    renderer.drawLine(x - t, y - t, x - t, y - t + k, BLACK);
    renderer.drawLine(x + t, y - t, x + t - k, y - t, BLACK);
    renderer.drawLine(x + t, y - t, x + t, y - t + k, BLACK);
    renderer.drawLine(x - t, y + t, x - t + k, y + t, BLACK);
    renderer.drawLine(x - t, y + t, x - t, y + t - k, BLACK);
    renderer.drawLine(x + t, y + t, x + t - k, y + t, BLACK);
    renderer.drawLine(x + t, y + t, x + t, y + t - k, BLACK);
  }
}

void TsumegoActivity::drawMenu() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Tsumego menu");

  const ListLayout L = computeListLayout(renderer, MENU_COUNT, menuSel, /*wantBlurb=*/false);
  const int pad = m.contentSidePadding;

  for (int k = 0; k < L.rowsPerPage; k++) {
    const int i = L.firstVisible + k;
    const bool sel = (i == menuSel);
    if (sel) renderer.fillRect(pad - 6, L.rowTop(k), pageW - (pad - 6) * 2, L.boxH, true);
    renderer.drawText(L.titleFont, pad + 4, L.nameTop(k), MENU_LABELS[i], !sel);
  }

  const auto labels = mappedInput.mapLabels("Close", "Choose", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void TsumegoActivity::drawSetMenu() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Problem sets");

  // A TSU1 file carries no set table, so there is genuinely nothing to list.
  // Say so, rather than drawing an empty screen.
  if (nSets == 0) {
    const int pageH = renderer.getScreenHeight();
    renderer.drawCenteredText(ROW_FONT, pageH / 2 - 20, "No problem sets");
    renderer.drawCenteredText(SMALL, pageH / 2 + 12,
                              "This problems.bin is in the older TSU1 format,");
    renderer.drawCenteredText(SMALL, pageH / 2 + 12 + renderer.getLineHeight(SMALL) + 4,
                              "which stores no set names. Problems still work.");
    const auto lbl = mappedInput.mapLabels("Back", "Back", "", "");
    GUI.drawButtonHints(renderer, lbl.btn1, lbl.btn2, lbl.btn3, lbl.btn4);
    renderer.displayBuffer();
    return;
  }

  // A TSU2 file may carry up to 16 sets; scroll rather than crop.
  const ListLayout L = computeListLayout(renderer, nSets, setSel, /*wantBlurb=*/true);
  const int pad = m.contentSidePadding;

  for (int k = 0; k < L.rowsPerPage; k++) {
    const int i = L.firstVisible + k;
    if (i >= nSets) break;
    const bool sel = (i == setSel);
    if (sel) renderer.fillRect(pad - 6, L.rowTop(k), pageW - (pad - 6) * 2, L.boxH, true);
    renderer.drawText(L.titleFont, pad + 4, L.nameTop(k), setNames[i], !sel);

    if (L.withBlurb) {
      const uint16_t a = setStarts[i], b = setEnd((uint8_t)i);
      char sub[48];
      snprintf(sub, sizeof(sub), "%u of %u solved", (unsigned)countSolved(a, b), (unsigned)(b - a));
      renderer.drawText(L.subFont, pad + 4, L.blurbTop(k), sub, !sel);
    }
  }

  if (L.scrolls(nSets)) {
    char pos[24];
    snprintf(pos, sizeof(pos), "%d of %d", setSel + 1, nSets);
    renderer.drawCenteredText(L.subFont, L.bottom + 4, pos);
  }

  const auto labels = mappedInput.mapLabels("Back", "Open", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void TsumegoActivity::drawScrub() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Go to problem");

  char buf[48];
  snprintf(buf, sizeof(buf), "P%u of %u", (unsigned)(scrubVal + 1), (unsigned)problemCount);
  renderer.drawCenteredText(BITTER_18_FONT_ID, pageH / 2 - 30, buf);

  if (nSets) {
    snprintf(buf, sizeof(buf), "%s%s", setNames[setOf(scrubVal)], isSolved(scrubVal) ? "   solved" : "");
    renderer.drawCenteredText(SMALL, pageH / 2 + 6, buf);
  }

  // progress bar
  const int barW = pageW - m.contentSidePadding * 2;
  const int barX = m.contentSidePadding, barY = pageH / 2 + 40;
  renderer.drawRect(barX, barY, barW, 12, BLACK);
  const int fill = problemCount ? (int)((uint32_t)barW * scrubVal / problemCount) : 0;
  renderer.fillRect(barX, barY, fill, 12, BLACK);

  renderer.drawCenteredText(SMALL, barY + 34, "Hold Left / Right to accelerate");
  renderer.drawCenteredText(SMALL, barY + 34 + renderer.getLineHeight(SMALL) + 4,
                            "Up / Down jump 100. Menu has direct entry.");

  const auto labels = mappedInput.mapLabels("Back", "Go", "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
