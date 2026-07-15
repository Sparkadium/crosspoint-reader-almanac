// countries_test.cpp -- host check of countries.bin + cty::findCountry.
#include <cstdio>
#include <cstring>
#include <cmath>
#include "../../src/activities/almanac/cty_math.h"
struct FileReader {
  FILE* f;
  int read(void* b, unsigned n) { return (int)fread(b, 1, n, f); }
  void seek(uint32_t o) { fseek(f, o, SEEK_SET); }
  bool seekCur(int64_t o) { return fseek(f, (long)o, SEEK_CUR) == 0; }
};
int main() {
  FileReader r{fopen("countries.bin", "rb")};
  if (!r.f) { printf("no countries.bin\n"); return 1; }
  struct { float la, lo; const char* want; } T[] = {
      {45.32f, -75.75f, "canada"}, {48.85f, 2.35f, "france"},
      {35.68f, 139.69f, "japan"}, {-33.86f, 151.2f, "australia"},
      {38.9f, -77.03f, "united-states"}, {55.75f, 37.61f, "russia"},
      {-29.6f, 28.2f, "lesotho"},        // enclave: smallest-bbox rule
      {30.0f, -140.0f, ""},              // mid-Pacific: nothing
      {52.5f, 13.4f, "germany"}, {-15.79f, -47.88f, "brazil"},
      {21.0f, 96.0f, "burma"}, {37.55f, 126.99f, "south-korea"},
      {11.59f, 43.15f, "djibouti"},   // coastal capital outside its own
                                      // 110m polygon: the bbox fallback
      {1.30f, 103.85f, "singapore"},  // microstate inside Malaysia's coarse
                                      // 110m coastline: smallest box wins
      {41.90f, 12.45f, "holy-see-vatican-city"},  // inside Italy's polygon
      {41.95f, 12.70f, "italy"},      // eastern Rome: outside the Vatican box
      {37.8f, 5.0f, ""},              // Mediterranean off Algeria: big-bbox
                                      // countries get NO fallback
  };
  int fail = 0;
  for (auto& t : T) {
    char key[32] = "", name[44] = "";
    const bool ok = cty::findCountry(r, t.la, t.lo, key, 32, name, 44);
    const bool pass = t.want[0] ? (ok && !strcmp(key, t.want)) : !ok;
    printf("%s  (%.2f,%.2f) -> %s [%s]\n", pass ? "ok  " : "FAIL", t.la, t.lo,
           ok ? key : "(none)", ok ? name : "");
    if (!pass) fail++;
  }
  // fly-to points: capitals within a degree of truth
  struct { const char* key; float la, lo; } C[] = {
      {"canada", 45.42f, -75.70f}, {"japan", 35.69f, 139.75f},
      {"united-states", 38.90f, -77.01f}, {"south-korea", 37.57f, 126.98f},
      {"monaco", 43.73f, 7.42f}, {"the-gambia", 13.45f, -16.58f},
      {"drc", -4.32f, 15.31f}, {"congo-brazzaville", -4.26f, 15.28f},
  };
  int total = 0;
  cty::forEachCountry(r, [&](int, const cty::CountryInfo& ci) {
    total++;
    for (auto& c : C)
      if (!strcmp(ci.key, c.key)) {
        const bool ok = fabsf(ci.flyLat - c.la) < 1.0f && fabsf(ci.flyLon - c.lo) < 1.0f;
        printf("%s  fly %s -> %.2f,%.2f\n", ok ? "ok  " : "FAIL", ci.key, ci.flyLat, ci.flyLon);
        if (!ok) fail++;
      }
  });
  printf("%d countries iterated, %d failures\n", total, fail);
  return fail ? 1 : 0;
}
