/*
 * sky_math.h — astronomy core for the WatchyDict sky module.
 *
 * Pure C, no Arduino dependencies: this exact file is compiled both
 * into the desktop verification harness (tested against astropy) and
 * into the watch firmware. Algorithms:
 *   - Julian date, GMST:      standard (Meeus, Astronomical Algorithms)
 *   - Equatorial -> alt/az:   spherical trigonometry
 *   - Sun position:           Meeus ch. 25 low-precision (~0.01 deg)
 *   - Sunrise/sunset:         NOAA solar calculator method
 *   - Moon position/phase:    Meeus ch. 47 truncated series (~0.3 deg)
 *   - Eastern time DST rule:  2nd Sun March .. 1st Sun November (post-2007)
 *
 * All angles degrees, all times UTC unless noted.
 */

#ifndef SKY_MATH_H
#define SKY_MATH_H

#include <math.h>

#define SKY_PI 3.14159265358979323846
#define D2R (SKY_PI / 180.0)
#define R2D (180.0 / SKY_PI)

static double sky_norm360(double x) { x = fmod(x, 360.0); return x < 0 ? x + 360.0 : x; }

/* ---- Julian date from civil UTC (valid 1900..2099) ---- */
static double sky_julian(int y, int mo, int d, double hourUTC) {
    if (mo <= 2) { y -= 1; mo += 12; }
    int A = y / 100;
    int B = 2 - A + A / 4;
    double jd = floor(365.25 * (y + 4716)) + floor(30.6001 * (mo + 1))
              + d + B - 1524.5 + hourUTC / 24.0;
    return jd;
}

/* ---- Greenwich mean sidereal time, degrees ---- */
static double sky_gmst(double jd) {
    double T = (jd - 2451545.0) / 36525.0;
    double g = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
             + 0.000387933 * T * T - T * T * T / 38710000.0;
    return sky_norm360(g);
}

/* ---- equatorial (RA,Dec, deg) -> horizontal (alt,az, deg) ----
   az measured from north, through east. */
static void sky_altaz(double raDeg, double decDeg, double jd,
                      double latDeg, double lonDeg,
                      double *alt, double *az) {
    double lst = sky_norm360(sky_gmst(jd) + lonDeg);   /* east lon positive */
    double H = (lst - raDeg) * D2R;                    /* hour angle */
    double lat = latDeg * D2R, dec = decDeg * D2R;
    double sinAlt = sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(H);
    *alt = asin(sinAlt) * R2D;
    double y = -sin(H) * cos(dec);
    double x = sin(dec) * cos(lat) - cos(dec) * sin(lat) * cos(H);
    *az = sky_norm360(atan2(y, x) * R2D);
}

/* ---- Sun geocentric RA/Dec (Meeus ch. 25 low precision) ---- */
static void sky_sunRaDec(double jd, double *ra, double *dec, double *eclLon) {
    double T = (jd - 2451545.0) / 36525.0;
    double L0 = sky_norm360(280.46646 + 36000.76983 * T + 0.0003032 * T * T);
    double M  = sky_norm360(357.52911 + 35999.05029 * T - 0.0001537 * T * T);
    double Mr = M * D2R;
    double C = (1.914602 - 0.004817 * T - 0.000014 * T * T) * sin(Mr)
             + (0.019993 - 0.000101 * T) * sin(2 * Mr)
             + 0.000289 * sin(3 * Mr);
    double trueLon = L0 + C;
    double omega = 125.04 - 1934.136 * T;
    double lam = trueLon - 0.00569 - 0.00478 * sin(omega * D2R); /* apparent */
    double eps = 23.439291 - 0.0130042 * T + 0.00256 * cos(omega * D2R);
    double lamR = lam * D2R, epsR = eps * D2R;
    *ra  = sky_norm360(atan2(cos(epsR) * sin(lamR), cos(lamR)) * R2D);
    *dec = asin(sin(epsR) * sin(lamR)) * R2D;
    if (eclLon) *eclLon = sky_norm360(lam);
}

/* ---- equation of time, minutes (NOAA / Meeus) ---- */
static double sky_eqTime(double jd) {
    double T = (jd - 2451545.0) / 36525.0;
    double L0 = sky_norm360(280.46646 + 36000.76983 * T + 0.0003032 * T * T);
    double M  = sky_norm360(357.52911 + 35999.05029 * T - 0.0001537 * T * T);
    double e  = 0.016708634 - 0.000042037 * T - 0.0000001267 * T * T;
    double eps = (23.439291 - 0.0130042 * T) * D2R;
    double y = tan(eps / 2); y *= y;
    double L0r = L0 * D2R, Mr = M * D2R;
    double E = y * sin(2 * L0r) - 2 * e * sin(Mr)
             + 4 * e * y * sin(Mr) * cos(2 * L0r)
             - 0.5 * y * y * sin(4 * L0r)
             - 1.25 * e * e * sin(2 * Mr);
    return E * R2D * 4.0;                              /* minutes */
}

/* ---- sunrise/sunset, NOAA method ----
   Returns 1 on success and fills riseUTC/setUTC in fractional hours.
   Returns 0 for polar day/night. */
static int sky_sunRiseSet(int y, int mo, int d, double latDeg, double lonDeg,
                          double *riseUTC, double *setUTC) {
    double riseH = 12.0, setH = 12.0;
    for (int it = 0; it < 3; it++) {
        /* solar noon from equation of time at current estimate */
        double noon = 12.0 - lonDeg / 15.0
                    - sky_eqTime(sky_julian(y, mo, d, 12.0 - lonDeg / 15.0)) / 60.0;
        double ra, dec;
        double latR = latDeg * D2R;
        sky_sunRaDec(sky_julian(y, mo, d, riseH), &ra, &dec, 0);
        double cosH = (sin(-0.833 * D2R) - sin(latR) * sin(dec * D2R))
                    / (cos(latR) * cos(dec * D2R));
        if (cosH > 1.0 || cosH < -1.0) return 0;
        riseH = noon - acos(cosH) * R2D / 15.0;
        sky_sunRaDec(sky_julian(y, mo, d, setH), &ra, &dec, 0);
        cosH = (sin(-0.833 * D2R) - sin(latR) * sin(dec * D2R))
             / (cos(latR) * cos(dec * D2R));
        if (cosH > 1.0 || cosH < -1.0) return 0;
        setH = noon + acos(cosH) * R2D / 15.0;
    }
    *riseUTC = fmod(riseH + 48.0, 24.0);
    *setUTC  = fmod(setH  + 48.0, 24.0);
    return 1;
}

/* ---- Moon geocentric RA/Dec and phase (Meeus ch. 47 truncated) ---- */
static void sky_moonRaDec(double jd, double *ra, double *dec, double *eclLon) {
    double T = (jd - 2451545.0) / 36525.0;
    double Lp = sky_norm360(218.3164477 + 481267.88123421 * T);   /* mean lon */
    double D  = sky_norm360(297.8501921 + 445267.1114034 * T);    /* elongation */
    double M  = sky_norm360(357.5291092 + 35999.0502909 * T);     /* sun anom */
    double Mp = sky_norm360(134.9633964 + 477198.8675055 * T);    /* moon anom */
    double F  = sky_norm360(93.2720950 + 483202.0175233 * T);     /* arg lat */
    double Dr = D * D2R, Mr = M * D2R, Mpr = Mp * D2R, Fr = F * D2R;
    /* principal longitude terms (deg) */
    double lon = Lp
        + 6.288774 * sin(Mpr)
        + 1.274027 * sin(2 * Dr - Mpr)
        + 0.658314 * sin(2 * Dr)
        + 0.213618 * sin(2 * Mpr)
        - 0.185116 * sin(Mr)
        - 0.114332 * sin(2 * Fr)
        + 0.058793 * sin(2 * Dr - 2 * Mpr)
        + 0.057066 * sin(2 * Dr - Mr - Mpr)
        + 0.053322 * sin(2 * Dr + Mpr)
        + 0.045758 * sin(2 * Dr - Mr)
        - 0.040923 * sin(Mr - Mpr)
        - 0.034720 * sin(Dr)
        - 0.030383 * sin(Mr + Mpr);
    /* principal latitude terms (deg) */
    double lat =
          5.128122 * sin(Fr)
        + 0.280602 * sin(Mpr + Fr)
        + 0.277693 * sin(Mpr - Fr)
        + 0.173237 * sin(2 * Dr - Fr)
        + 0.055413 * sin(2 * Dr - Mpr + Fr)
        + 0.046271 * sin(2 * Dr - Mpr - Fr)
        + 0.032573 * sin(2 * Dr + Fr);
    double eps = (23.439291 - 0.0130042 * T) * D2R;
    double lonR = sky_norm360(lon) * D2R, latR = lat * D2R;
    *ra = sky_norm360(atan2(sin(lonR) * cos(eps) - tan(latR) * sin(eps),
                            cos(lonR)) * R2D);
    *dec = asin(sin(latR) * cos(eps) + cos(latR) * sin(eps) * sin(lonR)) * R2D;
    if (eclLon) *eclLon = sky_norm360(lon);
}

/* illuminated fraction 0..1 and waxing flag from sun/moon elongation */
static double sky_moonIllum(double jd, int *waxing) {
    double sunLon, moonLon, ra, dec;
    sky_sunRaDec(jd, &ra, &dec, &sunLon);
    sky_moonRaDec(jd, &ra, &dec, &moonLon);
    double elong = sky_norm360(moonLon - sunLon);      /* 0=new, 180=full */
    if (waxing) *waxing = (elong < 180.0) ? 1 : 0;
    return (1.0 - cos(elong * D2R)) / 2.0;
}

/* phase index 0..7: new, wax cresc, first qtr, wax gibb, full,
   wan gibb, last qtr, wan cresc */
static int sky_moonPhaseIdx(double jd) {
    double sunLon, moonLon, ra, dec;
    sky_sunRaDec(jd, &ra, &dec, &sunLon);
    sky_moonRaDec(jd, &ra, &dec, &moonLon);
    double elong = sky_norm360(moonLon - sunLon);
    return ((int)floor((elong + 22.5) / 45.0)) & 7;
}

/* ---- Eastern time: UTC offset in hours for a local date (post-2007 rule):
   DST from 2:00 local on the 2nd Sunday of March
   to 2:00 local on the 1st Sunday of November. ---- */
static int sky_dowSakamoto(int y, int m, int d) {   /* 0=Sunday */
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}
static int sky_easternUtcOffset(int y, int mo, int d, int hourLocal) {
    if (mo < 3 || mo > 11) return -5;
    if (mo > 3 && mo < 11) return -4;
    if (mo == 3) {
        int firstDow = sky_dowSakamoto(y, 3, 1);
        int secondSun = 1 + ((7 - firstDow) % 7) + 7;
        if (d > secondSun || (d == secondSun && hourLocal >= 2)) return -4;
        return -5;
    }
    /* November */
    int firstDow = sky_dowSakamoto(y, 11, 1);
    int firstSun = 1 + ((7 - firstDow) % 7);
    if (d > firstSun || (d == firstSun && hourLocal >= 2)) return -5;
    return -4;
}

#endif
