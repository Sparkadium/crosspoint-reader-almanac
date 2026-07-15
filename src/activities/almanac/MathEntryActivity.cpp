// Arduino.h must come first: on the ESP32 it declares placement new
// (operator new(size_t, void*)), which std::function needs. ButtonNavigator
// takes std::function callbacks, so every file using it must see this first.
#include <Arduino.h>

#include "MathEntryActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Only fonts every variant of the firmware registers: the reader-font swaps in
// forks (Bitter, Lexend, ...) rename the serif IDs, but UI and SMALL survive.
constexpr int KEY_FONT = UI_12_FONT_ID;
constexpr int ALT_FONT = SMALL_FONT_ID;
constexpr int EXPR_FONT = UI_12_FONT_ID;
constexpr int SMALL = SMALL_FONT_ID;
constexpr bool BLACK = true;

using K = MathEntryActivity;
}  // namespace

// ---------------------------------------------------------------------------
// Layout: digits stay put like a phone pad; everything an expression is made
// of is one press. Hold alternates are the "second function" row of a TI.
// ---------------------------------------------------------------------------

const MathEntryActivity::Key* MathEntryActivity::rowKeys(int row, int& count) const {
  static const Key R0[] = {
      {"7", "7", nullptr, nullptr, SP_NONE, 1},   {"8", "8", nullptr, nullptr, SP_NONE, 1},
      {"9", "9", nullptr, nullptr, SP_NONE, 1},   {"(", "(", nullptr, nullptr, SP_NONE, 1},
      {")", ")", nullptr, nullptr, SP_NONE, 1},   {"^", "^", nullptr, nullptr, SP_NONE, 1},
      {"pi", "pi", nullptr, nullptr, SP_NONE, 1}, {"e", "e", nullptr, nullptr, SP_NONE, 1},
  };
  static const Key R1[] = {
      {"4", "4", nullptr, nullptr, SP_NONE, 1}, {"5", "5", nullptr, nullptr, SP_NONE, 1},
      {"6", "6", nullptr, nullptr, SP_NONE, 1}, {"+", "+", nullptr, nullptr, SP_NONE, 1},
      {"-", "-", nullptr, nullptr, SP_NONE, 1}, {"*", "*", nullptr, nullptr, SP_NONE, 1},
      {"/", "/", nullptr, nullptr, SP_NONE, 1}, {"x", "x", nullptr, nullptr, SP_NONE, 1},
  };
  static const Key R2[] = {
      {"1", "1", nullptr, nullptr, SP_NONE, 1},
      {"2", "2", nullptr, nullptr, SP_NONE, 1},
      {"3", "3", nullptr, nullptr, SP_NONE, 1},
      {"sin", "sin(", "asin", "asin(", SP_NONE, 1},
      {"cos", "cos(", "acos", "acos(", SP_NONE, 1},
      {"tan", "tan(", "atan", "atan(", SP_NONE, 1},
      {"sqrt", "sqrt(", "cbrt", "cbrt(", SP_NONE, 1},
      {"abs", "abs(", "round", "round(", SP_NONE, 1},
  };
  static const Key R3[] = {
      {"0", "0", nullptr, nullptr, SP_NONE, 1},
      {".", ".", nullptr, nullptr, SP_NONE, 1},
      {"ln", "ln(", "log2", "log2(", SP_NONE, 1},
      {"log", "log(", nullptr, nullptr, SP_NONE, 1},
      {"exp", "exp(", nullptr, nullptr, SP_NONE, 1},
      {"sinh", "sinh(", "cosh", "cosh(", SP_NONE, 1},
      {"tanh", "tanh(", nullptr, nullptr, SP_NONE, 1},
      {"floor", "floor(", "ceil", "ceil(", SP_NONE, 1},
  };
  static const Key R4[] = {
      {"<", nullptr, nullptr, nullptr, SP_LEFT, 1},
      {">", nullptr, nullptr, nullptr, SP_RIGHT, 1},
      {"DEL", nullptr, nullptr, nullptr, SP_DEL, 1},
      {"CLR", nullptr, nullptr, nullptr, SP_CLR, 1},
      {"Cancel", nullptr, nullptr, nullptr, SP_CANCEL, 2},
      {"OK", nullptr, nullptr, nullptr, SP_OK, 2},
  };
  static const Key* const ROWS_[K::ROWS] = {R0, R1, R2, R3, R4};
  static const int COUNTS[K::ROWS] = {8, 8, 8, 8, 6};
  count = COUNTS[row];
  return ROWS_[row];
}

int MathEntryActivity::keyStartCol(int row, int key) const {
  int n;
  const Key* ks = rowKeys(row, n);
  int c = 0;
  for (int i = 0; i < key && i < n; i++) c += ks[i].span;
  return c;
}

int MathEntryActivity::keyAtCol(int row, int col) const {
  int n;
  const Key* ks = rowKeys(row, n);
  int c = 0;
  for (int i = 0; i < n; i++) {
    c += ks[i].span;
    if (col < c) return i;
  }
  return n - 1;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void MathEntryActivity::recompile() {
  probe_.compile(text_.c_str());
  if (probe_.error()) {
    char b[80];
    snprintf(b, sizeof(b), "%s (at %d)", probe_.error(), probe_.errorPos() + 1);
    verdict_ = b;
  } else if (probe_.empty()) {
    verdict_ = "empty - OK clears the slot";
  } else {
    verdict_ = probe_.usesX() ? "OK" : "OK (constant - no x)";
  }
}

void MathEntryActivity::insertText(const char* s) {
  const size_t len = strlen(s);
  if (text_.size() + len > maxLength_) return;
  text_.insert(cursor_, s);
  cursor_ += len;
  recompile();
}

void MathEntryActivity::activate(const Key& k, bool alt) {
  switch (k.special) {
    case SP_LEFT:
      if (cursor_ > 0) cursor_--;
      return;
    case SP_RIGHT:
      if (cursor_ < text_.size()) cursor_++;
      return;
    case SP_DEL:
      if (cursor_ > 0) {
        text_.erase(cursor_ - 1, 1);
        cursor_--;
        recompile();
      }
      return;
    case SP_CLR:
      text_.clear();
      cursor_ = 0;
      recompile();
      return;
    case SP_CANCEL: {
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }
    case SP_OK:
      setResult(KeyboardResult{text_});
      finish();
      return;
    default: break;
  }
  const char* ins = (alt && k.altInsert) ? k.altInsert : k.insert;
  if (ins) insertText(ins);
}

// ---------------------------------------------------------------------------
// Lifecycle / input
// ---------------------------------------------------------------------------

void MathEntryActivity::onEnter() {
  Activity::onEnter();
  ButtonNavigator::setMappedInputManager(mappedInput);
  backHeld = backLong = confirmHeld = confirmLong = false;
  cursor_ = text_.size();
  recompile();
  requestUpdate();
}

void MathEntryActivity::loop() {
  // ---- Back: tap = delete at cursor, hold = cancel -------------------------
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { backHeld = true; backLong = false; }
  if (backHeld && !backLong && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    backLong = true;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!backHeld) return;  // leftover from the caller
    const bool wasLong = backLong;
    backHeld = backLong = false;
    if (wasLong) return;
    if (cursor_ > 0) {
      text_.erase(cursor_ - 1, 1);
      cursor_--;
      recompile();
      requestUpdate();
    }
    return;
  }

  // ---- D-pad: move key selection --------------------------------------------
  bool moved = false;
  nav_.onPressAndContinuous({MappedInputManager::Button::Right}, [&] {
    selCol = (selCol + 1) % COLS;
    moved = true;
  });
  nav_.onPressAndContinuous({MappedInputManager::Button::Left}, [&] {
    selCol = (selCol + COLS - 1) % COLS;
    moved = true;
  });
  nav_.onPressAndContinuous({MappedInputManager::Button::Down}, [&] {
    selRow = (selRow + 1) % ROWS;
    moved = true;
  });
  nav_.onPressAndContinuous({MappedInputManager::Button::Up}, [&] {
    selRow = (selRow + ROWS - 1) % ROWS;
    moved = true;
  });
  if (moved) { requestUpdate(); return; }

  // ---- Confirm: tap = key, hold = the key's alternate ----------------------
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) { confirmHeld = true; confirmLong = false; }
  if (confirmHeld && !confirmLong && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    confirmLong = true;
    int n;
    const Key* ks = rowKeys(selRow, n);
    const Key& k = ks[keyAtCol(selRow, selCol)];
    if (k.altInsert) {  // no alternate: wait for the release, act as a tap
      activate(k, /*alt=*/true);
      requestUpdate();
    } else {
      confirmLong = false;
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!confirmHeld) return;  // leftover from the caller
    const bool wasLong = confirmLong;
    confirmHeld = confirmLong = false;
    if (wasLong) return;
    int n;
    const Key* ks = rowKeys(selRow, n);
    activate(ks[keyAtCol(selRow, selCol)], /*alt=*/false);
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void MathEntryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, title_.c_str());

  // ---- expression box with a visible cursor --------------------------------
  const int exprLine = renderer.getLineHeight(EXPR_FONT);
  const int boxY = m.topPadding + m.headerHeight + m.verticalSpacing + 6;
  const int boxH = exprLine + 12;
  const int boxW = pageW - pad * 2;
  renderer.drawRect(pad, boxY, boxW, boxH, BLACK);

  // Horizontal scroll: keep the cursor inside the box by trimming the front.
  const std::string pre = text_.substr(0, cursor_);
  const std::string post = text_.substr(cursor_);
  size_t skip = 0;
  while (skip < pre.size() &&
         renderer.getTextWidth(EXPR_FONT, pre.c_str() + skip) > boxW - 16)
    skip++;
  const int tx = pad + 6, ty = boxY + 6;
  const char* preVis = pre.c_str() + skip;
  renderer.drawText(EXPR_FONT, tx, ty, preVis);
  const int cx = tx + renderer.getTextWidth(EXPR_FONT, preVis);
  renderer.drawLine(cx, ty - 1, cx, ty + exprLine, 2, BLACK);  // the cursor
  if (!post.empty()) renderer.drawText(EXPR_FONT, cx + 3, ty, post.c_str());

  // ---- live verdict ---------------------------------------------------------
  renderer.drawText(SMALL, pad, boxY + boxH + 4, verdict_.c_str());

  // ---- keypad ---------------------------------------------------------------
  const int gridTop = boxY + boxH + renderer.getLineHeight(SMALL) + 12;
  const int gridBottom = pageH - m.buttonHintsHeight - m.verticalSpacing - 4;
  const int cellW = boxW / COLS;
  const int cellH = std::max(renderer.getLineHeight(KEY_FONT) + 14,
                             (gridBottom - gridTop) / ROWS);
  const int keyFontH = renderer.getLineHeight(KEY_FONT);

  for (int r = 0; r < ROWS; r++) {
    int n;
    const Key* ks = rowKeys(r, n);
    const int selKey = keyAtCol(selRow, selCol);
    int col = 0;
    for (int i = 0; i < n; i++) {
      const Key& k = ks[i];
      const int x = pad + col * cellW;
      const int y = gridTop + r * cellH;
      const int w = cellW * k.span;
      const bool sel = (r == selRow && i == selKey);

      if (sel) renderer.fillRect(x + 1, y + 1, w - 2, cellH - 2, BLACK);
      else renderer.drawRect(x + 1, y + 1, w - 2, cellH - 2, BLACK);

      const int lw = renderer.getTextWidth(KEY_FONT, k.label);
      renderer.drawText(KEY_FONT, x + (w - lw) / 2, y + (cellH - keyFontH) / 2, k.label, !sel);
      if (k.altLabel) {
        const int aw = renderer.getTextWidth(ALT_FONT, k.altLabel);
        renderer.drawText(ALT_FONT, x + w - aw - 4, y + 2, k.altLabel, !sel);
      }
      col += k.span;
    }
  }

  const auto labels = mappedInput.mapLabels("Delete", "Insert", "Move", "Move");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
