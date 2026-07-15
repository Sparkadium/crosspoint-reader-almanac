// night_test.cpp -- brute-force verification of globe::nightSpans: for random
// sun/view configurations, compare the scanline answer against direct
// per-pixel evaluation of a*px + b*py + c*pz. Boundary pixels (within 1e-3 of
// the terminator) are excluded: they legitimately fall either way.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "../../src/activities/almanac/globe_math.h"
int main() {
  srand(42);
  long checked = 0, wrong = 0;
  for (int cfg = 0; cfg < 400; cfg++) {
    float sv[3];
    float mag = 0;
    for (int i = 0; i < 3; i++) { sv[i] = (rand() % 2001 - 1000) / 1000.0f; mag += sv[i] * sv[i]; }
    mag = sqrtf(mag);
    if (mag < 1e-3f) continue;
    for (int i = 0; i < 3; i++) sv[i] /= mag;
    for (int r = 0; r < 61; r++) {
      const float py = -1.0f + 2.0f * r / 60;
      float spans[4];
      const int n = globe::nightSpans(sv, py, spans);
      const float lim2 = 1.0f - py * py;
      if (lim2 <= 0) continue;
      const float xl = sqrtf(lim2);
      for (int cc = 0; cc <= 120; cc++) {
        const float px = -xl + 2 * xl * cc / 120.0f;
        const float pz = sqrtf(fmaxf(0.0f, lim2 - px * px));
        const float f = sv[0] * px + sv[1] * py + sv[2] * pz;
        if (fabsf(f) < 1e-3f) continue;  // on the terminator
        const bool night = f < 0;
        bool inSpan = false;
        for (int s = 0; s < n; s++)
          if (px >= spans[s * 2] && px <= spans[s * 2 + 1]) inSpan = true;
        checked++;
        if (night != inSpan) {
          if (wrong < 5)
            printf("MISMATCH cfg%d sv(%.3f,%.3f,%.3f) py=%.3f px=%.3f f=%.4f night=%d span=%d\n",
                   cfg, sv[0], sv[1], sv[2], py, px, (double)f, (int)night, (int)inSpan);
          wrong++;
        }
      }
    }
  }
  printf("%ld pixel checks, %ld mismatches\n", checked, wrong);
  return wrong ? 1 : 0;
}
