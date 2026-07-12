#pragma once
//
// ChessActivity.h — Lichess puzzle trainer, ported from the Watchy chess module
// (chess_module.h) to CrossPoint on the Xteink X4.
//
// The module was mothballed on the watch because twelve piece identities at
// 25px squares was a losing battle on a 200x200 panel. That was a verdict about
// that screen, not this code: here the squares are ~56px and the problem simply
// goes away.
//
// Transplanted UNCHANGED (it was already validated):
//   - toSq() board flip so the player is always at the bottom
//   - chessApply(): castling (king moves two files -> slide the rook),
//     en passant (pawn steps diagonally onto an empty square -> remove the
//     pawn behind it), promotion (promo byte replaces the pawn). No rights or
//     legality state is needed because the device only APPLIES moves.
//   - load / replay / attempt flow, including wrong-move retry and undo
//   - the two-phase selection rule (select an own piece, then a destination)
//
// chess_pack.py verified a mirror of chessApply against python-chess on every
// ply of all 9,977 puzzles, and chess_sim.py exercised this exact interaction
// flow. If this port ever disagrees with chess_sim.py, this port is wrong.
//
// CHP1 format (from chess_pack.py, confirmed against chess_full.bin):
//   "CHP1" | u16 count | u8 nsets
//   per set: u8 nameLen, name (rating band, e.g. "900-1199"), u16 startIndex
//   u32 offsets[count]   (absolute file offsets)
//   blob: flags u8 (bit0 = player is black), lastFrom u8, lastTo u8,
//         rating u16, board 32 bytes (one nibble per square, sq0=a1..sq63=h8;
//         0 empty, 1..6 white PNBRQK, 7..12 black), nMoves u8,
//         nMoves x (from u8, to u8, promo u8: 0 none / 2 N 3 B 4 R 5 Q)
//   Plies 0, 2, 4... are the player's moves; the odd plies are the scripted
//   opponent replies. The packer pre-applied the opponent's blunder.
//
// Known quirk, inherited: only the stored line is accepted, so an alternate
// mate-in-one counts as wrong. Lichess itself accepts alternate mates; fixing
// that would need a real rules engine on the device.
//
// Buttons: D-pad moves | Confirm selects then plays | Back deselects, then
//          undoes, then exits | hold Back opens the menu
//
#include <Preferences.h>

#include <cstdint>

#include "HalStorage.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ChessActivity final : public Activity {
 public:
  ChessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Chess", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Mode : uint8_t { PLAYING, WRONG, SOLVED, REVEALED, MENU, SETMENU, SCRUB };

  static constexpr uint16_t LONG_PRESS_MS = 700;
  static constexpr uint16_t REVEAL_STEP_MS = 800;

  // ---- engine (verbatim) ----
  uint8_t toSq(uint8_t scr) const;
  bool ownPiece(uint8_t sq) const;
  void chessApply(uint8_t frm, uint8_t to, uint8_t promo);
  bool loadChess(uint16_t idx);
  void chessReplay();
  void chessAttempt(uint8_t toBoardSq);
  void chessConfirm();
  void undoTurn();
  void solve();

  // ---- progress / navigation ----
  bool isSolved(uint16_t i) const { return solvedMap[i >> 3] & (1 << (i & 7)); }
  uint16_t countSolved(uint16_t a, uint16_t b) const;
  uint8_t setOf(uint16_t idx) const;
  uint16_t setEnd(uint8_t s) const;
  void jumpTo(uint16_t idx);
  void jumpToSet(uint8_t s);
  void jumpNextUnsolved();
  void jumpRandom();
  void startReveal();

  // ---- UI ----
  void moveCursor(int dc, int dr);
  void openMenu();
  void runMenuItem(int item);
  void openNumberEntry();
  void blitSprite(int x, int y, const uint32_t* mask, int scale) const;   // 20px legacy set
  void blitMask40(int x, int y, const uint64_t* mask, bool state) const;  // 40px native set
  void drawBoard();
  void drawMenu();
  void drawSetMenu();
  void drawScrub();

  HalFile chFile;
  Preferences prefs;
  ButtonNavigator nav_;

  uint16_t chCount = 0;
  uint32_t chBase = 7;
  uint8_t nSets = 0;
  char setNames[32][25];
  uint16_t setStarts[32];
  uint8_t solvedMap[1280];  // 9,977 puzzles -> 1,248 bytes
  uint16_t solvedCount = 0;

  uint8_t cboard[64];
  bool playerBlack = false;
  uint16_t crating = 0;
  uint8_t cline[16][3];
  uint8_t lineN = 0, linePos = 0;
  uint8_t selSq = 0xFF;
  uint8_t ccursor = 0;
  uint8_t dispFrom = 0xFF, dispTo = 0xFF;
  uint16_t probIdx = 0;

  Mode mode = PLAYING;
  Mode resumeMode = PLAYING;

  int menuSel = 0, setSel = 0;
  uint16_t scrubVal = 0;

  uint8_t revealAt = 0;
  unsigned long revealNext = 0;

  bool loadError = false;
  // Act on a release only if this activity saw the press; a release with no
  // matching press is left over from a child activity that just closed.
  bool backHeld = false, backLong = false;
  bool confirmHeld = false;
};
