#pragma once
//
// ListLayout.h — one place that decides how a selectable list fits on screen.
//
// Two failure modes this exists to prevent:
//
//   1. Guessing a font size. Row heights must be MEASURED from the renderer,
//      not assumed, or adding one row silently crops the last one off the
//      bottom. The ladder below tries progressively smaller title fonts until
//      the rows genuinely fit, and drops the sub-line as a last resort.
//
//   2. Assuming the list is short. Chess reads up to 32 rating bands and
//      tsumego up to 16 problem sets. No font makes 32 rows fit, so long lists
//      scroll: rowsPerPage rows are drawn, windowed around the selection.
//
// All four title fonts are registered in main.cpp via renderer.insertFont(); a
// font that is not registered draws NOTHING, so do not add IDs here casually.
//
#include <GfxRenderer.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

struct ListLayout {
  int titleFont = UI_12_FONT_ID;
  int subFont = SMALL_FONT_ID;
  int nameLine = 0;
  int subLine = 0;
  int boxH = 0;   // highlight rectangle height
  int rowH = 0;   // boxH plus the air between rows
  bool withBlurb = true;
  int top = 0;
  int bottom = 0;
  int rowsPerPage = 1;
  int firstVisible = 0;  // scroll window origin

  static constexpr int PAD_TOP = 6;
  static constexpr int GAP = 1;
  static constexpr int PAD_BOT = 6;
  static constexpr int AIR = 6;

  int rowTop(int k) const { return top + k * rowH; }          // k = visible index
  int nameTop(int k) const { return rowTop(k) + PAD_TOP; }
  int blurbTop(int k) const { return nameTop(k) + nameLine + GAP; }
  bool scrolls(int itemCount) const { return itemCount > rowsPerPage; }
};

inline ListLayout computeListLayout(const GfxRenderer& r, int itemCount, int selector,
                                    bool wantBlurb) {
  const auto& m = UITheme::getInstance().getMetrics();
  ListLayout L;
  L.subLine = r.getLineHeight(L.subFont);
  L.top = m.topPadding + m.headerHeight + m.verticalSpacing + 8;
  // Reserve a line above the button hints for status text / scroll indicator.
  L.bottom = r.getScreenHeight() - m.buttonHintsHeight - m.verticalSpacing - L.subLine - 8;
  const int avail = std::max(1, L.bottom - L.top);

  static const int FONTS[] = {NOTOSERIF_16_FONT_ID, NOTOSERIF_14_FONT_ID, NOTOSERIF_12_FONT_ID,
                              UI_12_FONT_ID};
  constexpr int N = (int)(sizeof(FONTS) / sizeof(FONTS[0]));

  // Prefer the largest font that shows every row. Failing that, scroll -- do
  // NOT throw away the sub-line to avoid scrolling; the information is worth
  // more than the scrollbar. The sub-line goes only if even the smallest font
  // cannot show a usable window.
  L.withBlurb = wantBlurb;
  auto measure = [&](int font, bool blurb) {
    L.titleFont = font;
    L.nameLine = r.getLineHeight(font);
    return ListLayout::PAD_TOP + L.nameLine + (blurb ? ListLayout::GAP + L.subLine : 0) +
           ListLayout::PAD_BOT;
  };

  bool fitsAll = false;
  for (int f = 0; f < N; f++) {
    L.boxH = measure(FONTS[f], L.withBlurb);
    if ((L.boxH + ListLayout::AIR) * itemCount <= avail) {
      fitsAll = true;
      break;
    }
  }
  if (!fitsAll) {
    L.boxH = measure(FONTS[N - 1], L.withBlurb);  // smallest font, still scrolling
    if (avail / (L.boxH + ListLayout::AIR) < 3 && L.withBlurb) {
      L.withBlurb = false;                        // last resort: titles only
      L.boxH = measure(FONTS[N - 1], false);
    }
  }
  L.rowH = L.boxH + ListLayout::AIR;

  // Whatever still does not fit, scroll.
  L.rowsPerPage = std::max(1, avail / L.rowH);
  if (L.rowsPerPage > itemCount) L.rowsPerPage = itemCount;

  L.firstVisible = 0;
  if (itemCount > L.rowsPerPage) {
    L.firstVisible = selector - L.rowsPerPage / 2;  // keep the selection centred
    if (L.firstVisible < 0) L.firstVisible = 0;
    if (L.firstVisible > itemCount - L.rowsPerPage) L.firstVisible = itemCount - L.rowsPerPage;
  }
  return L;
}
