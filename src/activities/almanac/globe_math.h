#pragma once
//
// globe_math.h — the geometry of the Globe module: orthographic view basis,
// sun position from a UTC epoch (NOAA simplified formulas), and the per-row
// terminator solver that shades the night side without per-pixel trig.
//
// Pure logic, no display, no Arduino — verified on the host by globe_test.cpp
// against the same PIL mockup math that was checked against the real sky.
//
// Frames: WORLD x -> (lat 0, lon 0), y -> (lat 0, 90E), z -> north pole.
//         VIEW  rows of m: screen-east, screen-north, out-of-screen.
//         A world point P projects to sx = m0.P, sy = m1.P, visible m2.P > 0.
//
#include <cmath>
#include <cstdint>

namespace globe {

struct Basis { float m[9]; };

inline Basis viewBasis(float latDeg, float lonDeg) {
  const float la = latDeg * 0.0174532925f, lo = lonDeg * 0.0174532925f;
  const float sla = sinf(la), cla = cosf(la), slo = sinf(lo), clo = cosf(lo);
  Basis b;
  // screen east
  b.m[0] = -slo;       b.m[1] = clo;        b.m[2] = 0;
  // screen north (tangent)
  b.m[3] = -sla * clo; b.m[4] = -sla * slo; b.m[5] = cla;
  // out of screen (center direction)
  b.m[6] = cla * clo;  b.m[7] = cla * slo;  b.m[8] = sla;
  return b;
}

// Proleptic-Gregorian civil date from a Unix epoch (Howard Hinnant's
// civil_from_days), needed to feed the NOAA formulas.
inline void civilFromEpoch(int64_t epoch, int& y, int& mo, int& d, float& utcHours) {
  int64_t days = epoch / 86400;
  int64_t rem = epoch - days * 86400;
  if (rem < 0) { rem += 86400; days -= 1; }
  utcHours = rem / 3600.0f;
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = (unsigned)(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)(yoe + era * 400);
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  mo = (int)(mp < 10 ? mp + 3 : mp - 9);
  if (mo <= 2) y += 1;
}

// Subsolar point for a UTC epoch. NOAA "General Solar Position Calculations"
// truncated series: declination good to ~0.1 deg, equation of time ~0.2 min —
// far below one pixel of terminator at R=214.
inline void subsolar(int64_t epoch, float& latDeg, float& lonDeg) {
  int y, mo, d; float uh;
  civilFromEpoch(epoch, y, mo, d, uh);
  static const int CUM[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  const int doy = CUM[mo - 1] + d + (leap && mo > 2 ? 1 : 0);
  const float g = 6.2831853f / (leap ? 366.0f : 365.0f) * (doy - 1 + (uh - 12.0f) / 24.0f);
  const float decl = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g)
                     - 0.006758f * cosf(2 * g) + 0.000907f * sinf(2 * g)
                     - 0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
  const float eq = 229.18f * (0.000075f + 0.001868f * cosf(g) - 0.032077f * sinf(g)
                              - 0.014615f * cosf(2 * g) - 0.040849f * sinf(2 * g));
  latDeg = decl * 57.29578f;
  lonDeg = -15.0f * (uh - 12.0f + eq / 60.0f);
  while (lonDeg > 180) lonDeg -= 360;
  while (lonDeg < -180) lonDeg += 360;
}

inline void unitVec(float latDeg, float lonDeg, float v[3]) {
  const float la = latDeg * 0.0174532925f, lo = lonDeg * 0.0174532925f;
  v[0] = cosf(la) * cosf(lo);
  v[1] = cosf(la) * sinf(lo);
  v[2] = sinf(la);
}

// Night intervals on one screen row. py in [-1,1] (up positive). A visible
// point at (px,py) has pz = sqrt(1-px^2-py^2); it is night iff
// f(px) = a*px + b*py + c*pz < 0, with (a,b,c) the sun vector in VIEW frame.
// f is linear plus c times a CONCAVE term, so with c < 0 (sun behind the
// globe) f is convex and the night set is ONE interval — but with c > 0 (you
// are looking at the lit face) f is concave and night is up to TWO slivers
// against the limb. The first version of this function merged those two into
// one span and shaded straight across the daylight between them.
// The threshold k selects the altitude contour: k = 0 is the terminator
// itself, k = sin(-6 deg) is the civil-twilight edge, so shading the k=0
// region lightly and the k=sin(-6) region darkly gives the grey twilight
// band between them. Writes pairs into out[0..3]; returns how many (0..2).
inline int nightSpans(const float sv[3], float py, float k, float out[4]) {
  const float lim2 = 1.0f - py * py;
  if (lim2 <= 0) return 0;
  const float xl = sqrtf(lim2);
  const float a = sv[0], b = sv[1], c = sv[2];

  auto dayAt = [&](float px) {
    const float pz2 = lim2 - px * px;
    const float pz = pz2 > 0 ? sqrtf(pz2) : 0;
    return a * px + b * py + c * pz >= k;
  };

  // boundary candidates: limb ends + roots of (k - b py - a px)^2 = c^2 (lim2 - px^2)
  const float g = k - b * py;
  float cand[4]; int n = 0;
  cand[n++] = -xl;
  const float A = a * a + c * c, B = -2 * a * g, C = g * g - c * c * lim2;
  if (A > 1e-12f) {
    const float disc = B * B - 4 * A * C;
    if (disc >= 0) {
      const float sq = sqrtf(disc);
      const float r0 = (-B - sq) / (2 * A), r1 = (-B + sq) / (2 * A);
      if (r0 > -xl && r0 < xl) cand[n++] = r0;
      if (r1 > -xl && r1 < xl) cand[n++] = r1;
    }
  }
  cand[n++] = xl;
  // sort (n <= 4)
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && cand[j] < cand[j - 1]; j--) {
      const float t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t;
    }
  int cnt = 0;
  for (int i = 0; i + 1 < n && cnt < 2; i++) {
    if (!dayAt((cand[i] + cand[i + 1]) * 0.5f)) {
      if (cnt > 0 && out[cnt * 2 - 1] == cand[i]) {
        out[cnt * 2 - 1] = cand[i + 1];  // adjacent: extend, do not split
      } else {
        out[cnt * 2] = cand[i];
        out[cnt * 2 + 1] = cand[i + 1];
        cnt++;
      }
    }
  }
  return cnt;
}

}  // namespace globe
