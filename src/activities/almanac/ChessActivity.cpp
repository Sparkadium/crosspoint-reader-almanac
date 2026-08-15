// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "ChessActivity.h"

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
#include "sprites.h"    // SPR (=20), SPR_FILL[6], SPR_LINE[6]      -- small squares
#include "sprites40.h"  // SPR40 (=40), SPR40_FILL/LINE/HALO[6]   -- native, no scaling
namespace {
// Board geometry, shared by drawBoard() and loop(). It lived inline in
// drawBoard(); a tap has to resolve to the square that was actually drawn, and
// two copies would drift.
struct BoardGeom {
  int x0, y0, sq;
};

BoardGeom computeBoardGeom(const GfxRenderer& r) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = r.getScreenWidth();
  const int top = m.topPadding + m.headerHeight + m.verticalSpacing + 8;
  const int bottom = r.getScreenHeight() - m.buttonHintsHeight - m.verticalSpacing - 44;
  const int avail = std::min(pageW - m.contentSidePadding * 2, bottom - top);
  BoardGeom g;
  g.sq = avail / 8;
  g.x0 = (pageW - g.sq * 8) / 2;
  g.y0 = top + ((bottom - top) - g.sq * 8) / 2;
  return g;
}
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

const char* MENU_LABELS[] = {"Next unsolved",  "Random puzzle", "Browse to puzzle",
                             "Type puzzle number", "Rating bands", "Show solution",
                             "Restart puzzle"};
constexpr int MENU_COUNT = 7;
}  // namespace

// ===========================================================================
//  Engine — transplanted from chess_module.h
// ===========================================================================

uint8_t ChessActivity::toSq(uint8_t scr) const {
  const uint8_t r = scr / 8, c = scr % 8;
  return playerBlack ? (r * 8 + (7 - c)) : ((7 - r) * 8 + c);
}

bool ChessActivity::ownPiece(uint8_t sq) const {
  const uint8_t n = cboard[sq];
  return n && (playerBlack ? n >= 7 : n <= 6);
}

void ChessActivity::chessApply(uint8_t frm, uint8_t to, uint8_t promo) {
  const uint8_t p = cboard[frm];
  if ((p == 6 || p == 12) && abs(frm % 8 - to % 8) == 2) {  // castling
    const uint8_t rank = frm - frm % 8;
    if (to > frm) { cboard[rank + 5] = cboard[rank + 7]; cboard[rank + 7] = 0; }
    else { cboard[rank + 3] = cboard[rank + 0]; cboard[rank + 0] = 0; }
  }
  if ((p == 1 || p == 7) && frm % 8 != to % 8 && cboard[to] == 0)
    cboard[to + (p == 1 ? -8 : 8)] = 0;  // en passant
  cboard[to] = promo ? (p <= 6 ? promo : promo + 6) : p;
  cboard[frm] = 0;
  dispFrom = frm;
  dispTo = to;
}

bool ChessActivity::loadChess(uint16_t idx) {
  if (idx >= chCount) return false;
  chFile.seek(chBase + 4UL * idx);
  uint32_t off = 0;
  chFile.read(&off, 4);
  chFile.seek(off);

  uint8_t hd[5];
  chFile.read(hd, 5);
  playerBlack = hd[0] & 1;
  dispFrom = hd[1];
  dispTo = hd[2];
  crating = hd[3] | (hd[4] << 8);

  uint8_t nib[32];
  chFile.read(nib, 32);
  for (uint8_t i = 0; i < 32; i++) {
    cboard[2 * i] = nib[i] & 0xF;
    cboard[2 * i + 1] = nib[i] >> 4;
  }
  chFile.read(&lineN, 1);
  if (lineN > 16) lineN = 16;
  chFile.read((uint8_t*)cline, 3 * lineN);

  linePos = 0;
  selSq = 0xFF;
  mode = PLAYING;
  probIdx = idx;
  ccursor = 63;
  for (int8_t i = 63; i >= 0; i--)  // start on the nearest own piece
    if (ownPiece(toSq(i))) { ccursor = i; break; }
  return true;
}

void ChessActivity::chessReplay() {
  const uint8_t keep = linePos;
  loadChess(probIdx);
  while (linePos < keep) {
    chessApply(cline[linePos][0], cline[linePos][1], cline[linePos][2]);
    linePos++;
  }
}

void ChessActivity::chessAttempt(uint8_t toBoardSq) {
  const uint8_t f = cline[linePos][0], t = cline[linePos][1];
  if (selSq == f && toBoardSq == t) {  // correct
    chessApply(f, t, cline[linePos][2]);
    linePos++;
    selSq = 0xFF;
    if (linePos >= lineN) { solve(); return; }
    chessApply(cline[linePos][0], cline[linePos][1], cline[linePos][2]);  // scripted reply
    linePos++;
    if (linePos >= lineN) solve();
    return;
  }
  chessApply(selSq, toBoardSq, 0);  // wrong: show it, retry via chessReplay()
  selSq = 0xFF;
  mode = WRONG;
}

void ChessActivity::chessConfirm() {
  const uint8_t sq = toSq(ccursor);
  if (selSq == 0xFF || ownPiece(sq)) {
    if (ownPiece(sq)) selSq = sq;
    return;
  }
  chessAttempt(sq);
}

void ChessActivity::undoTurn() {
  linePos = linePos >= 2 ? linePos - 2 : 0;
  chessReplay();
}

void ChessActivity::solve() {
  mode = SOLVED;
  if (!isSolved(probIdx)) {
    solvedMap[probIdx >> 3] |= 1 << (probIdx & 7);
    prefs.putBytes("map", solvedMap, (chCount + 7) / 8);
    solvedCount++;
    prefs.putUShort("solved", solvedCount);
  }
}

// ===========================================================================
//  Progress / navigation
// ===========================================================================

uint16_t ChessActivity::countSolved(uint16_t a, uint16_t b) const {
  uint16_t n = 0;
  for (uint16_t i = a; i < b; i++)
    if (isSolved(i)) n++;
  return n;
}

uint8_t ChessActivity::setOf(uint16_t idx) const {
  uint8_t s = 0;
  for (uint8_t i = 0; i < nSets; i++)
    if (setStarts[i] <= idx) s = i;
  return s;
}

uint16_t ChessActivity::setEnd(uint8_t s) const {
  return (s + 1 < nSets) ? setStarts[s + 1] : chCount;
}

void ChessActivity::jumpTo(uint16_t idx) {
  loadChess(idx);
  prefs.putUShort("cur", probIdx);
}

void ChessActivity::jumpToSet(uint8_t s) {
  const uint16_t a = setStarts[s], b = setEnd(s);
  uint16_t t = a;
  for (uint16_t i = a; i < b; i++)
    if (!isSolved(i)) { t = i; break; }
  jumpTo(t);
}

void ChessActivity::jumpNextUnsolved() {
  for (uint16_t k = 1; k <= chCount; k++) {
    const uint16_t i = (probIdx + k) % chCount;
    if (!isSolved(i)) { jumpTo(i); return; }
  }
}

void ChessActivity::jumpRandom() { jumpTo((uint16_t)(esp_random() % chCount)); }

// Non-blocking playback of the remaining stored line.
void ChessActivity::startReveal() {
  loadChess(probIdx);
  if (lineN == 0) { requestUpdate(); return; }
  mode = REVEALED;
  revealAt = 0;
  revealNext = millis() + REVEAL_STEP_MS;
  requestUpdate();
}

// ===========================================================================
//  Lifecycle
// ===========================================================================

void ChessActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);
  backHeld = backLong = confirmHeld = false;

  chFile = Storage.open("/chess.bin", O_RDONLY);
  if (!chFile) chFile = Storage.open("/chess_full.bin", O_RDONLY);  // as shipped
  if (!chFile || chFile.size() < 7) { loadError = true; requestUpdate(); return; }

  uint8_t hdr[7];
  chFile.seek(0);
  chFile.read(hdr, 7);
  if (memcmp(hdr, "CHP1", 4) != 0) { loadError = true; requestUpdate(); return; }
  chCount = hdr[4] | (hdr[5] << 8);
  if (chCount == 0) { loadError = true; requestUpdate(); return; }

  const uint8_t nsets = hdr[6];
  uint32_t p = 7;
  nSets = 0;
  for (uint8_t i = 0; i < nsets; i++) {
    chFile.seek(p);
    uint8_t ln;
    chFile.read(&ln, 1);
    if (nSets < 32) {
      const uint8_t rd = ln > 24 ? 24 : ln;
      chFile.read((uint8_t*)setNames[nSets], rd);
      setNames[nSets][rd] = 0;
      chFile.seek(p + 1 + ln);
      uint8_t se[2];
      chFile.read(se, 2);
      setStarts[nSets] = se[0] | (se[1] << 8);
      nSets++;
    }
    p += 1 + (uint32_t)ln + 2;
  }
  chBase = p;  // offsets table starts here

  prefs.begin("chess");
  solvedCount = prefs.getUShort("solved", 0);
  memset(solvedMap, 0, sizeof(solvedMap));
  prefs.getBytes("map", solvedMap, sizeof(solvedMap));

  uint16_t cur = prefs.getUShort("cur", 0);
  if (cur >= chCount) cur = 0;
  if (!loadChess(cur)) loadError = true;
  requestUpdate();
}

void ChessActivity::onExit() {
  Activity::onExit();
  if (chFile) chFile.close();
  prefs.end();
}

// ===========================================================================
//  Input
// ===========================================================================

// The watch stepped the cursor through own pieces only, because four buttons.
// With a D-pad we roam freely in screen space; chessConfirm() still enforces
// the two-phase rule, so the interaction contract is unchanged.
void ChessActivity::moveCursor(int dc, int dr) {
  int c = ccursor % 8, r = ccursor / 8;
  c = (c + dc + 8) % 8;
  r = (r + dr + 8) % 8;
  ccursor = (uint8_t)(r * 8 + c);
}

void ChessActivity::openMenu() {
  resumeMode = (mode < MENU) ? mode : PLAYING;
  mode = MENU;
  menuSel = 0;
}

void ChessActivity::openNumberEntry() {
  auto handler = [this](const ActivityResult& res) {
    // The keyboard consumed the press; its trailing release arrives without a
    // matching press and is discarded by the press/release matching in loop().
    backHeld = backLong = confirmHeld = false;
    if (!res.isCancelled) {
      const auto* kr = std::get_if<KeyboardResult>(&res.data);
      if (kr && !kr->text.empty()) {
        const long n = strtol(kr->text.c_str(), nullptr, 10);
        if (n >= 1 && n <= (long)chCount) {
          jumpTo((uint16_t)(n - 1));  // shown 1-based
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
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Puzzle number", "", 5), handler);
}

void ChessActivity::runMenuItem(int item) {
  switch (item) {
    case 0: jumpNextUnsolved(); break;
    case 1: jumpRandom(); break;
    case 2: scrubVal = probIdx; mode = SCRUB; return;
    case 3: openNumberEntry(); return;
    case 4: setSel = setOf(probIdx); mode = SETMENU; return;
    case 5: startReveal(); return;
    case 6: loadChess(probIdx); break;
    default: break;
  }
  mode = PLAYING;
}

void ChessActivity::loop() {
  if (loadError) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backHeld = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backHeld) finish();
    return;
  }

  // ---- timed solution playback -------------------------------------------
  if (mode == REVEALED && revealAt < lineN) {
    if (millis() >= revealNext) {
      chessApply(cline[revealAt][0], cline[revealAt][1], cline[revealAt][2]);
      revealAt++;
      revealNext = millis() + REVEAL_STEP_MS;
      requestUpdate();
    }
    return;
  }

  // ---- Touch: hold anywhere to open the menu -------------------------------
  // A hint tap gives press+release with no held state between, so isPressed()
  // never sees a hold and the hold-Back branch below cannot fire on a device
  // with no physical Back. This uses the SDK long-press classifier instead;
  // suppressNextTouchTap() keeps the release from also landing on the board.
  if (mode < MENU) {
    int lx = 0, ly = 0;
    if (mappedInput.isScreenTouchLongPress(lx, ly, LONG_PRESS_MS)) {
      mappedInput.suppressNextTouchTap();
      openMenu();
      requestUpdate();
      return;
    }
  }

  // ---- Back: hold = menu; tap = deselect / undo / exit ---------------------
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { backHeld = true; backLong = false; }
  if (backHeld && !backLong && mode < MENU && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
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
      case WRONG: chessReplay(); requestUpdate(); return;
      case REVEALED: loadChess(probIdx); requestUpdate(); return;
      default:
        if (selSq != 0xFF) { selSq = 0xFF; requestUpdate(); return; }
        if (linePos > 0) { undoTurn(); requestUpdate(); return; }
        finish();
        return;
    }
  }

  // ---- menus ---------------------------------------------------------------
  if (mode == MENU || mode == SETMENU) {
    const int count = (mode == MENU) ? MENU_COUNT : nSets;
    int& sel = (mode == MENU) ? menuSel : setSel;

    // Touch: same gestures as every other ListLayout screen. sel is a
    // reference, so the helpers update menuSel / setSel directly.
    {
      const ListLayout L = computeListLayout(renderer, count, sel, /*wantBlurb=*/mode == SETMENU);
      if (listSwipePage(mappedInput, L, count, sel)) { requestUpdate(); return; }
      if (listRowTouch(mappedInput, L, count, sel)) {
        confirmHeld = false;  // no Confirm press/release pair accompanies a tap
        if (mode == MENU) runMenuItem(menuSel);
        else { jumpToSet((uint8_t)setSel); mode = PLAYING; }
        requestUpdate();
        return;
      }
    }
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
      v %= (long)chCount;
      if (v < 0) v += chCount;
      scrubVal = (uint16_t)v;
      moved = true;
    };
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

  // Swipe between problems once this one is finished. Left is "next", the way
  // a page turns; right steps back one. Restricted to the done states on
  // purpose -- a swipe mid-solve would throw the attempt away.
  if (mode == SOLVED || mode == REVEALED) {
    switch (mappedInput.wasSwipe()) {
      case MappedInputManager::SwipeDir::Left:
        jumpNextUnsolved();
        requestUpdate();
        return;
      case MappedInputManager::SwipeDir::Right:
        jumpTo(probIdx > 0 ? static_cast<uint16_t>(probIdx - 1)
                           : static_cast<uint16_t>(chCount - 1));
        requestUpdate();
        return;
      default:
        break;
    }
  }

  // Tap a square. chessConfirm() already implements the two-phase rule against
  // ccursor -- tapping an own piece selects it, tapping again elsewhere plays --
  // so moving the cursor to the tap and reusing it keeps touch and buttons
  // behaving identically.
  if (mode == PLAYING) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const BoardGeom g = computeBoardGeom(renderer);
      const int col = (tx >= g.x0) ? (tx - g.x0) / g.sq : -1;
      const int row = (ty >= g.y0) ? (ty - g.y0) / g.sq : -1;
      if (col >= 0 && col < 8 && row >= 0 && row < 8) {
        ccursor = static_cast<uint8_t>(row * 8 + col);
        chessConfirm();
        requestUpdate();
        return;
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHeld = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!confirmHeld) return;  // leftover from a child
    confirmHeld = false;
    switch (mode) {
      case SOLVED: jumpNextUnsolved(); break;
      case WRONG: chessReplay(); break;
      case REVEALED: loadChess(probIdx); break;
      default: chessConfirm(); break;
    }
    requestUpdate();
  }
}

// ===========================================================================
//  Rendering
// ===========================================================================

// Legacy 20px masks, integer-scaled. Doubling pixels cannot add detail -- it
// just turns every edge into a staircase -- so this is only used on boards whose
// squares are too small for the native 40px set.
void ChessActivity::blitSprite(int x, int y, const uint32_t* mask, int scale) const {
  for (uint8_t yy = 0; yy < SPR; yy++) {
    const uint32_t row = mask[yy];
    for (uint8_t xx = 0; xx < SPR; xx++)
      if (row >> xx & 1)
        renderer.fillRect(x + xx * scale, y + yy * scale, scale, scale, BLACK);
  }
}

// Native 40px masks: one source pixel, one screen pixel. Rows are 40 bits wide,
// hence uint64_t.
void ChessActivity::blitMask40(int x, int y, const uint64_t* mask, bool state) const {
  for (int yy = 0; yy < SPR40; yy++) {
    const uint64_t row = mask[yy];
    if (!row) continue;
    for (int xx = 0; xx < SPR40; xx++)
      if ((row >> xx) & 1ULL) renderer.drawPixel(x + xx, y + yy, state);
  }
}

void ChessActivity::drawBoard() {
  const BoardGeom g = computeBoardGeom(renderer);
  const int SQ = g.sq;
  const int X0 = g.x0;
  const int Y0 = g.y0;

  // Use the native 40px art whenever a square can hold it; fall back to the
  // scaled 20px set on smaller panels.
  const bool native = SQ >= SPR40 + 6;
  const int scale = std::max(1, (SQ - 6) / SPR);
  const int sprPx = native ? SPR40 : SPR * scale;
  const int haloR = SQ * 11 / 25;  // only used by the legacy path
  const int dither = std::max(3, native ? 4 : 3 * scale);

  for (uint8_t scr = 0; scr < 64; scr++) {
    const uint8_t sq = toSq(scr);
    const int x = X0 + (scr % 8) * SQ, y = Y0 + (scr / 8) * SQ;
    const bool dark = ((sq % 8) + (sq / 8)) % 2 == 0;  // a1 is dark

    if (dark)
      for (int yy = 1; yy < SQ; yy += dither)
        for (int xx = 1; xx < SQ; xx += dither) renderer.drawPixel(x + xx, y + yy, BLACK);

    const uint8_t n = cboard[sq];
    if (n) {
      const bool white = n <= 6;
      const uint8_t t = white ? n - 1 : n - 7;
      const int sx = x + (SQ - sprPx) / 2, sy = y + (SQ - sprPx) / 2;

      // A piece must never merge with the dark-square stipple. The 40px art is
      // nearly as wide as the square, so a halo DISC no longer covers its
      // crenellations, beads and base corners -- use the piece's own dilated
      // silhouette instead. The legacy path keeps the disc, which fit its 20px art.
      if (dark) {
        if (native) blitMask40(sx, sy, SPR40_HALO[t], WHITE);
        else fillCircleR(renderer, x + SQ / 2, y + SQ / 2, haloR, WHITE);
      }

      if (native) blitMask40(sx, sy, white ? SPR40_LINE[t] : SPR40_FILL[t], BLACK);
      else blitSprite(sx, sy, white ? SPR_LINE[t] : SPR_FILL[t], scale);
    }

    if (sq == dispFrom || sq == dispTo) renderer.drawRect(x, y, SQ, SQ, BLACK);
    if (sq == selSq) {
      renderer.drawRect(x, y, SQ, SQ, 3, BLACK);  // selection: thick border
    }
  }

  if (mode == PLAYING) {  // cursor: four corner ticks
    const int x = X0 + (ccursor % 8) * SQ, y = Y0 + (ccursor / 8) * SQ;
    const int k = SQ / 4;
    renderer.drawLine(x, y, x + k, y, 2, BLACK);
    renderer.drawLine(x, y, x, y + k, 2, BLACK);
    renderer.drawLine(x + SQ - 1, y, x + SQ - 1 - k, y, 2, BLACK);
    renderer.drawLine(x + SQ - 1, y, x + SQ - 1, y + k, 2, BLACK);
    renderer.drawLine(x, y + SQ - 1, x + k, y + SQ - 1, 2, BLACK);
    renderer.drawLine(x, y + SQ - 1, x, y + SQ - 1 - k, 2, BLACK);
    renderer.drawLine(x + SQ - 1, y + SQ - 1, x + SQ - 1 - k, y + SQ - 1, 2, BLACK);
    renderer.drawLine(x + SQ - 1, y + SQ - 1, x + SQ - 1, y + SQ - 1 - k, 2, BLACK);
  }

  renderer.drawRect(X0 - 2, Y0 - 2, SQ * 8 + 4, SQ * 8 + 4, 2, BLACK);
}

void ChessActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  if (loadError) {
    renderer.drawCenteredText(TITLE_FONT, pageH / 2 - 20, "Chess puzzles not found");
    renderer.drawCenteredText(SMALL, pageH / 2 + 10, "Copy chess.bin to the SD card root");
    GUI.drawButtonHints(renderer, "Back", "", "", "");
    renderer.displayBuffer();
    return;
  }

  if (mode == MENU) { drawMenu(); return; }
  if (mode == SETMENU) { drawSetMenu(); return; }
  if (mode == SCRUB) { drawScrub(); return; }

  char title[72];
  snprintf(title, sizeof(title), "P%u%s   %s to play   %u", (unsigned)(probIdx + 1),
           isSolved(probIdx) ? " *" : "", playerBlack ? "black" : "white", (unsigned)crating);
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, title);

  drawBoard();

  const int statusY = pageH - m.buttonHintsHeight - m.verticalSpacing - renderer.getLineHeight(SMALL) - 4;
  char status[72];
  switch (mode) {
    case WRONG: snprintf(status, sizeof(status), "Not the move  -  Back or Confirm to retry"); break;
    case SOLVED: snprintf(status, sizeof(status), "Solved  -  %u of %u", (unsigned)solvedCount, (unsigned)chCount); break;
    case REVEALED: snprintf(status, sizeof(status), "Solution"); break;
    default:
      if (selSq != 0xFF) snprintf(status, sizeof(status), "Pick a destination  -  Back cancels");
      else snprintf(status, sizeof(status), "hold Back for menu, bands and puzzle jump");
      break;
  }
  renderer.drawCenteredText(SMALL, statusY, status);

  const char* confirmLabel = (mode == SOLVED) ? "Next" : (mode == WRONG ? "Retry" : (selSq == 0xFF ? "Select" : "Play"));
  const char* backLabel = (mode == PLAYING && selSq == 0xFF && linePos == 0) ? "Back" : "Undo";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "Move", "Move");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void ChessActivity::drawMenu() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Chess menu");

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

void ChessActivity::drawSetMenu() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Rating bands");

  // Up to 32 bands can exist in a CHP1 file; no font fits 32 rows, so scroll.
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

void ChessActivity::drawScrub() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Go to puzzle");

  char buf[64];
  snprintf(buf, sizeof(buf), "P%u of %u", (unsigned)(scrubVal + 1), (unsigned)chCount);
  renderer.drawCenteredText(BITTER_16_FONT_ID, pageH / 2 - 30, buf);  // 18 has no metric on CrossInk

  if (nSets) {
    snprintf(buf, sizeof(buf), "%s%s", setNames[setOf(scrubVal)], isSolved(scrubVal) ? "   solved" : "");
    renderer.drawCenteredText(SMALL, pageH / 2 + 6, buf);
  }

  const int barW = pageW - m.contentSidePadding * 2;
  const int barX = m.contentSidePadding, barY = pageH / 2 + 40;
  renderer.drawRect(barX, barY, barW, 12, BLACK);
  const int fill = chCount ? (int)((uint32_t)barW * scrubVal / chCount) : 0;
  renderer.fillRect(barX, barY, fill, 12, BLACK);

  renderer.drawCenteredText(SMALL, barY + 34, "Hold Left / Right to accelerate");
  renderer.drawCenteredText(SMALL, barY + 34 + renderer.getLineHeight(SMALL) + 4,
                            "Up / Down jump 100. Menu has direct entry.");

  const auto labels = mappedInput.mapLabels("Back", "Go", "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
