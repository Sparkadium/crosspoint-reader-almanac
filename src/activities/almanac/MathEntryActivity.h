#pragma once
//
// MathEntryActivity.h — a one-layer math keypad for the graphing calculator.
//
// The stock KeyboardEntryActivity buries every character an expression is
// actually made of — ( ) + * / ^ — in shift and symbol layers. This keypad is
// the opposite: one flat TI-style grid where every key is a whole token, so
// "sin(" is one press, not five. Hold Confirm on a key to get the alternate
// printed in its corner (sin -> asin, sqrt -> cbrt, ln -> log2, ...).
//
// The expression is recompiled by calc_engine.h after every edit and the
// verdict is shown live above the grid, so a typo is visible the moment it is
// made instead of after OK.
//
// Returns a KeyboardResult, exactly like KeyboardEntryActivity, so the caller
// cannot tell the difference.
//
// Buttons: D-pad moves the key selection | Confirm inserts (hold = alternate)
//          Back deletes at the cursor    | hold Back cancels
//          Grid keys: cursor left/right, DEL, CLR, Cancel, OK
//
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "calc_engine.h"
#include "util/ButtonNavigator.h"

class MathEntryActivity final : public Activity {
 public:
  MathEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                    std::string title = "f(x) =", std::string initialText = "",
                    size_t maxLength = 64)
      : Activity("MathEntry", renderer, mappedInput),
        title_(std::move(title)),
        text_(std::move(initialText)),
        maxLength_(maxLength) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Special : uint8_t { SP_NONE, SP_LEFT, SP_RIGHT, SP_DEL, SP_CLR, SP_CANCEL, SP_OK };

  struct Key {
    const char* label;       // what the key shows
    const char* insert;      // what it types (nullptr for specials)
    const char* altLabel;    // corner label for the hold alternate (or nullptr)
    const char* altInsert;   // what the hold types (or nullptr)
    uint8_t special;         // SP_*
    uint8_t span;            // width in grid cells
  };

  static constexpr int ROWS = 5;
  static constexpr int COLS = 8;
  static constexpr uint16_t LONG_PRESS_MS = 600;

  const Key* rowKeys(int row, int& count) const;
  int keyAtCol(int row, int col) const;    // cell column -> key index
  int keyStartCol(int row, int key) const; // key index -> first cell column
  void activate(const Key& k, bool alt);
  void insertText(const char* s);
  void recompile();

  std::string title_;
  std::string text_;
  size_t maxLength_;
  size_t cursor_ = 0;

  CalcExpr probe_;         // live-compile scratch
  std::string verdict_;    // what the status line says about text_

  ButtonNavigator nav_;
  int selRow = ROWS - 1;   // start on the bottom row, on OK
  int selCol = COLS - 1;

  // Act on a release only if this activity saw the press; a release with no
  // matching press is left over from the activity that opened this one.
  bool backHeld = false, backLong = false;
  bool confirmHeld = false, confirmLong = false;
};
