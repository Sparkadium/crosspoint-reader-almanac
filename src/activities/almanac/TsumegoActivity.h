#pragma once
//
// TsumegoActivity.h — Go life-and-death trainer, ported from WatchyAlmanac's
// tsumego.h to CrossPoint on the Xteink X4.
//
// The Go engine is transplanted UNCHANGED: flood-fill liberties, capture and
// suicide detection, ko (banned off-tree only, since some tsumego lines encode
// threat-free ko), the variation-tree walker (skipSubtree/findChild/terminal),
// history + replay-based undo, solution search, and the NVS solved bitmap.
// It is pure array logic and never touched the display.
//
// Rewritten for this device:
//   - /problems.bin read from the SD card via HalFile (was SPIFFS)
//   - MappedInputManager instead of raw GPIO edge/hold/repeat polling
//   - GfxRenderer instead of GxEPD2 (no circle primitives, hand-rolled)
//   - true 4-way cursor: the watch only had "next empty point" / "down a row"
//     because it had four buttons. The X4 has a D-pad, so the cursor moves in
//     two dimensions like a real board.
//   - solution playback is timed in loop() instead of blocking delay(800)
//
// TSU2 format (from tsumego_pack.py, unchanged):
//   Header : "TSU2" | uint16 problemCount | uint8 setCount
//            then setCount x { uint8 nameLen, name[nameLen], uint16 startIdx }
//            ("TSU1" has no set table; offsets begin at byte 6)
//   Offsets: uint32 x problemCount, absolute file offsets of each problem blob
//   Blob   : flags, W, H, nBlack, black[]..., nWhite, white[]..., then the
//            move tree: {pos, isCorrect} nodes, 0xFF = POP, 0xFE = END_TREE,
//            0xFD = PASS
//
// Buttons: D-pad moves the cursor | Confirm places (hold = pass)
//          Back undoes, or exits when there is nothing to undo (hold = menu)
//
#include <Preferences.h>

#include <cstdint>

#include "HalStorage.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TsumegoActivity final : public Activity {
 public:
  TsumegoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Tsumego", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // ---- format constants (must match tsumego_pack.py) ----
  static constexpr uint8_t POP = 0xFF;
  static constexpr uint8_t END_TREE = 0xFE;
  static constexpr uint8_t PASS_POS = 0xFD;
  static constexpr uint16_t MAX_BLOB = 2048;
  static constexpr uint8_t MAX_HIST = 48;
  static constexpr uint8_t F_WHITE_TO_PLAY = 0x01;
  static constexpr uint8_t F_WALL_L = 0x02;
  static constexpr uint8_t F_WALL_T = 0x04;
  static constexpr uint8_t F_WALL_R = 0x08;
  static constexpr uint8_t F_WALL_B = 0x10;
  static constexpr uint16_t LONG_PRESS_MS = 600;
  static constexpr uint16_t REVEAL_STEP_MS = 800;

  enum Mode : uint8_t { PLAYING, OFFTREE, FAILED, SOLVED, REVEALED, MENU, SETMENU, SCRUB };

  // ---- engine (ported verbatim) ----
  bool loadGo(uint16_t idx);
  bool openSide(int c, int r) const;
  int floodLibs(uint8_t pos);
  bool applyMove(uint8_t pos, uint8_t color, bool fromTree);
  uint16_t skipSubtree(uint16_t nodeOff);
  uint16_t findChild(uint16_t list, uint8_t pos);
  bool terminal(uint16_t nodeOff) const { return blob[nodeOff + 2] == POP; }
  void pushHist(uint8_t pos) { if (histLen < MAX_HIST) hist[histLen++] = pos; }
  void replay();
  void userMove(uint8_t pos);
  void undoTurn();
  bool isSolved(uint16_t i) const { return solvedMap[i >> 3] & (1 << (i & 7)); }
  uint16_t countSolved(uint16_t a, uint16_t b) const;
  uint8_t setOf(uint16_t idx) const;
  uint16_t setEnd(uint8_t s) const;
  void solve();
  int8_t findRight(uint16_t list, uint8_t depth);
  void startReveal();
  void jumpTo(uint16_t idx);
  void jumpToSet(uint8_t s);
  void jumpNextUnsolved();
  void jumpRandom();

  // ---- UI ----
  void moveCursor(int dc, int dr);
  void openMenu();
  void runMenuItem(int item);
  void openNumberEntry();  // type a problem number directly
  void drawBoard();
  void drawMenu();
  void drawSetMenu();
  void drawScrub();

  HalFile binFile;
  Preferences prefs;
  ButtonNavigator nav_;

  uint16_t problemCount = 0;
  uint32_t offsetsBase = 6;
  uint8_t nSets = 0;
  char setNames[16][25];
  uint16_t setStarts[16];
  uint8_t solvedMap[1664];

  uint8_t blob[MAX_BLOB];
  uint16_t blobLen = 0;
  uint8_t flags = 0, W = 0, H = 0;
  uint16_t treeStart = 0;

  uint8_t board[252];
  uint8_t toPlay = 1;
  uint16_t probIdx = 0;
  uint8_t hist[MAX_HIST];
  uint8_t histLen = 0;
  uint16_t curList = 0;
  uint8_t cursor = 0;
  Mode mode = PLAYING;
  Mode resumeMode = PLAYING;
  int16_t koPos = -1;
  int16_t lastMove = -1;
  uint8_t offtreePos = 0;

  uint8_t grp[252];
  uint8_t grpN = 0;
  bool seen[252];
  uint8_t solPath[32];
  int8_t revealLen = 0, revealAt = 0;
  uint8_t revealColor = 1;
  unsigned long revealNext = 0;

  int menuSel = 0, setSel = 0;
  uint16_t scrubVal = 0;
  uint16_t solvedCount = 0;
  uint16_t partialCount = 0;

  bool loadError = false;
  bool confirmHeld = false, confirmLong = false;
  bool backHeld = false, backLong = false;
};
