#pragma once
//
// moon_math.h — the Moon's orientation for the Almanac's Moon globe.
//
// Gives, for a Julian date:
//   sub-Earth point   (selenographic lat/lon of the spot facing Earth --
//                      this IS the optical libration: spin the globe there
//                      and you see the Moon exactly as it faces you tonight)
//   sub-solar point   (where the Sun is overhead: drives the terminator)
//   illuminated fraction
//
// Method: Meeus, Astronomical Algorithms ch. 47 (lunar position, truncated
// series -- the same terms sky_math.h uses) + ch. 53 (optical librations).
// Physical libration (~0.03 deg) and nutation (~0.005 deg) are dropped; the
// sub-solar longitude uses the lambda_sun + 180 approximation (~0.15 deg).
//
// Verified by tools/almanac/moon_test.cpp against JPL DE421 -- the actual
// integrated lunar libration angles, decoded from the ephemeris -- across a
// ten-year grid. If this file disagrees with that harness, this file is wrong.
//
// Pure C-style, no Arduino: compiles on the host and the device unchanged.
//
#include <math.h>

#include "sky_math.h"  // D2R/R2D, sky_norm360, sky_julian, sky_sunRaDec

typedef struct {
  double subEarthLat, subEarthLon;  // selenographic degrees, lon east +
  double subSolarLat, subSolarLon;
  double illum;                     // 0..1
} MoonGeo;

/* Moon geocentric ecliptic lon/lat, Meeus ch. 47 truncated — the longitude
   terms are the same set sky_math.h uses; latitude adds its seven. */
static void moon_eclLonLat(double jd, double* lonDeg, double* latDeg) {
  double T = (jd - 2451545.0) / 36525.0;
  double Lp = sky_norm360(218.3164477 + 481267.88123421 * T);
  double D = sky_norm360(297.8501921 + 445267.1114034 * T);
  double M = sky_norm360(357.5291092 + 35999.0502909 * T);
  double Mp = sky_norm360(134.9633964 + 477198.8675055 * T);
  double F = sky_norm360(93.2720950 + 483202.0175233 * T);
  double Dr = D * D2R, Mr = M * D2R, Mpr = Mp * D2R, Fr = F * D2R;
  *lonDeg = sky_norm360(
      Lp + 6.288774 * sin(Mpr) + 1.274027 * sin(2 * Dr - Mpr) + 0.658314 * sin(2 * Dr) +
      0.213618 * sin(2 * Mpr) - 0.185116 * sin(Mr) - 0.114332 * sin(2 * Fr) +
      0.058793 * sin(2 * Dr - 2 * Mpr) + 0.057066 * sin(2 * Dr - Mr - Mpr) +
      0.053322 * sin(2 * Dr + Mpr) + 0.045758 * sin(2 * Dr - Mr) - 0.040923 * sin(Mr - Mpr) -
      0.034720 * sin(Dr) - 0.030383 * sin(Mr + Mpr));
  *latDeg = 5.128122 * sin(Fr) + 0.280602 * sin(Mpr + Fr) + 0.277693 * sin(Mpr - Fr) +
            0.173237 * sin(2 * Dr - Fr) + 0.055413 * sin(2 * Dr - Mpr + Fr) +
            0.046271 * sin(2 * Dr - Mpr - Fr) + 0.032573 * sin(2 * Dr + Fr);
}

/* Optical libration (Meeus 53.1) for a body seen along ecliptic (lam, beta).
   Returns selenographic lat/lon of the sub-point. */
static void moon_subPoint(double jd, double lamDeg, double betaDeg, double* latOut,
                          double* lonOut) {
  const double I = 1.54242 * D2R;  // inclination of the mean lunar equator
  double T = (jd - 2451545.0) / 36525.0;
  double F = sky_norm360(93.2720950 + 483202.0175233 * T);
  double Om = sky_norm360(125.0445479 - 1934.1362891 * T);
  double W = (sky_norm360(lamDeg - Om)) * D2R;
  double beta = betaDeg * D2R;
  double A = atan2(sin(W) * cos(beta) * cos(I) - sin(beta) * sin(I), cos(W) * cos(beta));
  double lon = A * R2D - F;
  lon = fmod(lon + 540.0, 360.0) - 180.0;  // normalize to +-180
  *lonOut = lon;
  *latOut = asin(-sin(W) * cos(beta) * sin(I) - sin(beta) * cos(I)) * R2D;
}

static MoonGeo moon_geo(double jd) {
  MoonGeo g;
  double mlon, mlat;
  moon_eclLonLat(jd, &mlon, &mlat);
  moon_subPoint(jd, mlon, mlat, &g.subEarthLat, &g.subEarthLon);

  double ra, dec, slon;
  sky_sunRaDec(jd, &ra, &dec, &slon);
  /* Sun as seen FROM the Moon is (very nearly) opposite its geocentric
     direction; the parallax correction (<0.16 deg) is dropped. */
  moon_subPoint(jd, slon + 180.0, 0.0, &g.subSolarLat, &g.subSolarLon);

  double elong = sky_norm360(mlon - slon);
  g.illum = (1.0 - cos(elong * D2R)) / 2.0;
  return g;
}
