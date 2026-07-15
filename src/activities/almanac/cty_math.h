#pragma once
// cty_math.h -- countries.bin access for the Globe module. Pure logic,
// host-verified by countries_test.cpp.
//   findCountry : streamed point-in-polygon (bbox reject, even-odd ray cast;
//                 smallest bbox wins, which resolves enclaves like Lesotho).
//                 SMALL countries get a bbox fallback: at 110m simplification
//                 a coastal capital can sit just outside its own coarse
//                 polygon (Djibouti City does), so a point that hits no
//                 polygon but lies inside a small country's box resolves to
//                 it. Only small boxes qualify, so open sea near a large
//                 country stays open sea.
//   forEachCountry : iterate name/key/fly-to for the Find list
// CTY2 adds a per-country fly-to point: the capital from Natural Earth
// populated places, or the largest ring's centroid where no capital exists.
#include <cstdint>
#include <cstring>

namespace cty {

// Reader is any type with: int read(void*, unsigned); void seek(uint32_t);
struct CountryInfo {
  char key[32];
  char name[44];
  float flyLat, flyLon;
};

// Visit(idx, CountryInfo&) for every country, in file (alphabetical) order.
// Returns the country count, or -1 on a bad file.
template <typename Reader, typename Visit>
int forEachCountry(Reader& f, Visit visit) {
  uint8_t h[6];
  f.seek(0);
  if (f.read(h, 6) != 6 || memcmp(h, "CTY2", 4) != 0) return -1;
  const uint16_t n = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
  uint32_t pos = 6;
  for (uint16_t c = 0; c < n; c++) {
    f.seek(pos);
    CountryInfo ci{};
    uint8_t kl;
    if (f.read(&kl, 1) != 1) return -1;
    f.read(ci.key, kl < 31 ? kl : 31);
    if (kl >= 31) f.seek(pos + 1 + kl);
    uint8_t dl;
    f.read(&dl, 1);
    f.read(ci.name, dl < 43 ? dl : 43);
    int16_t fly[2];
    f.read(fly, 4);
    ci.flyLat = fly[0] / 100.0f;
    ci.flyLon = fly[1] / 100.0f;
    f.seek(pos + 1 + kl + 1 + dl + 4 + 8);
    uint8_t rc2[2];
    f.read(rc2, 2);
    const uint16_t nRings = (uint16_t)rc2[0] | ((uint16_t)rc2[1] << 8);
    uint32_t p = pos + 1 + kl + 1 + dl + 4 + 8 + 2;
    for (uint16_t r = 0; r < nRings; r++) {
      f.seek(p);
      uint8_t pc2[2];
      f.read(pc2, 2);
      p += 2 + ((uint32_t)((uint16_t)pc2[0] | ((uint16_t)pc2[1] << 8))) * 4;
    }
    visit((int)c, ci);
    pos = p;
  }
  return (int)n;
}

template <typename Reader>
bool findCountry(Reader& f, float latDeg, float lonDeg,
                 char* keyOut, int keyCap, char* nameOut, int nameCap) {
  uint8_t h[6];
  f.seek(0);
  if (f.read(h, 6) != 6 || memcmp(h, "CTY2", 4) != 0) return false;
  const uint16_t n = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
  const int32_t la = (int32_t)(latDeg * 100), lo = (int32_t)(lonDeg * 100);

  bool found = false;
  int64_t bestArea = 0;
  // bbox fallback: countries no larger than ~6x6 degrees qualify
  constexpr int64_t SMALL_AREA = 600LL * 600LL;  // centidegrees squared
  constexpr int32_t MARGIN = 20;                 // 0.2 deg of slack
  bool fbFound = false;
  int64_t fbArea = 0;
  char fbKey[32] = {0}, fbName[44] = {0};
  uint32_t pos = 6;
  for (uint16_t c = 0; c < n; c++) {
    f.seek(pos);
    uint8_t kl;
    if (f.read(&kl, 1) != 1) return found;
    char key[32] = {0};
    f.read(key, kl < 31 ? kl : 31);
    if (kl >= 31) f.seek(pos + 1 + kl);
    uint8_t dl;
    f.read(&dl, 1);
    char name[44] = {0};
    f.read(name, dl < 43 ? dl : 43);
    f.seekCur(4);  // CTY2 fly-to point: not needed for hit testing
    int16_t bb[4];
    f.read(bb, 8);
    uint8_t rc2[2];
    f.read(rc2, 2);
    const uint16_t nRings = (uint16_t)rc2[0] | ((uint16_t)rc2[1] << 8);
    uint32_t p = pos + 1 + kl + 1 + dl + 4 + 8 + 2;

    const bool inBox = la >= bb[0] && la <= bb[1] && lo >= bb[2] && lo <= bb[3];
    const int64_t boxArea = (int64_t)(bb[1] - bb[0]) * (bb[3] - bb[2]);
    // The slack margin exists for coarse POLYGON outlines (Djibouti's coastal
    // capital). Point entries (nRings == 0) have no outline to be coarse:
    // their +-0.15 deg box already is the slack, and adding more makes the
    // Vatican claim half of Rome.
    const int32_t mg = nRings > 0 ? MARGIN : 0;
    if (boxArea <= SMALL_AREA && la >= bb[0] - mg && la <= bb[1] + mg &&
        lo >= bb[2] - mg && lo <= bb[3] + mg) {
      if (!fbFound || boxArea < fbArea) {
        fbFound = true;
        fbArea = boxArea;
        strncpy(fbKey, key, sizeof(fbKey) - 1);
        strncpy(fbName, name, sizeof(fbName) - 1);
      }
    }
    bool inside = false;
    for (uint16_t r = 0; r < nRings; r++) {
      f.seek(p);
      uint8_t pc2[2];
      f.read(pc2, 2);
      const uint16_t np = (uint16_t)pc2[0] | ((uint16_t)pc2[1] << 8);
      p += 2;
      if (inBox) {
        // even-odd ray cast, streaming the ring in chunks
        static int16_t buf[2 * 64];
        int32_t x0 = 0, y0 = 0, xF = 0, yF = 0;  // prev and first vertex
        uint16_t left = np;
        bool first = true;
        while (left > 0) {
          const int chunk = left < 64 ? left : 64;
          if (f.read(buf, chunk * 4) != chunk * 4) return found;
          for (int i = 0; i < chunk; i++) {
            const int32_t y1 = buf[i * 2], x1 = buf[i * 2 + 1];  // lat, lon
            if (first) { xF = x1; yF = y1; first = false; }
            else if (((y0 > la) != (y1 > la))) {
              const double xc = x0 + (double)(la - y0) / (y1 - y0) * (x1 - x0);
              if (xc > lo) inside = !inside;
            }
            x0 = x1; y0 = y1;
          }
          left -= chunk;
        }
        // closing edge back to the first vertex
        if (!first && ((y0 > la) != (yF > la))) {
          const double xc = x0 + (double)(la - y0) / (yF - y0) * (xF - x0);
          if (xc > lo) inside = !inside;
        }
      }
      p += (uint32_t)np * 4;
    }
    if (inside) {
      const int64_t area = (int64_t)(bb[1] - bb[0]) * (bb[3] - bb[2]);
      if (!found || area < bestArea) {
        bestArea = area;
        found = true;
        strncpy(keyOut, key, keyCap - 1); keyOut[keyCap - 1] = 0;
        strncpy(nameOut, name, nameCap - 1); nameOut[nameCap - 1] = 0;
      }
    }
    pos = p;
  }
  // Smallest bbox wins ACROSS both candidate kinds. A polygon hit does not
  // outrank a smaller point-entry: at 110m Malaysia's simplified coastline
  // swallows Singapore, and Italy's polygon covers the Vatican, so the tiny
  // capital box must be allowed to win inside a bigger polygon country.
  if (fbFound && (!found || fbArea < bestArea)) {
    strncpy(keyOut, fbKey, keyCap - 1); keyOut[keyCap - 1] = 0;
    strncpy(nameOut, fbName, nameCap - 1); nameOut[nameCap - 1] = 0;
    return true;
  }
  return found;
}

}  // namespace cty
