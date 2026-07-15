// globe_test.cpp -- host check: read globe.bin, render an ASCII globe with the
// EXACT math the device will run (basis, projection, subsolar, nightSpan).
//   g++ -std=c++17 -fno-exceptions -O2 globe_test.cpp -o globe_test && ./globe_test [epoch]
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include "../../src/activities/almanac/globe_math.h"
int main(int argc, char** argv) {
  const int64_t epoch = argc > 1 ? atoll(argv[1]) : (int64_t)time(nullptr);
  float slat, slon;
  globe::subsolar(epoch, slat, slon);
  printf("epoch %lld  subsolar %.1f, %.1f\n", (long long)epoch, slat, slon);

  const globe::Basis B = globe::viewBasis(45.3f, -75.8f);
  float sw[3], sv[3];
  globe::unitVec(slat, slon, sw);
  for (int i = 0; i < 3; i++) sv[i] = B.m[i*3]*sw[0] + B.m[i*3+1]*sw[1] + B.m[i*3+2]*sw[2];

  const int W = 72, H = 36;
  char g[H][W + 1];
  for (int r = 0; r < H; r++) { memset(g[r], ' ', W); g[r][W] = 0; }
  // night shading via nightSpan per row
  for (int r = 0; r < H; r++) {
    const float py = 1.0f - 2.0f * (r + 0.5f) / H;
    float x0, x1;
    if (globe::nightSpan(sv, py, x0, x1))
      for (int c = 0; c < W; c++) {
        const float px = -1.0f + 2.0f * (c + 0.5f) / W;
        if (px >= x0 && px <= x1 && ((r + c) & 1)) g[r][c] = '.';
      }
  }
  // coastlines from globe.bin
  FILE* f = fopen("globe.bin", "rb");
  if (!f) { printf("globe.bin missing\n"); return 1; }
  char hdr[4]; uint16_t nl;
  fread(hdr, 1, 4, f); fread(&nl, 2, 1, f);
  if (memcmp(hdr, "GLB1", 4)) { printf("bad magic\n"); return 1; }
  long pts = 0;
  for (int L = 0; L < nl; L++) {
    uint8_t flags; uint16_t np;
    fread(&flags, 1, 1, f); fread(&np, 2, 1, f);
    for (int i = 0; i < np; i++) {
      int16_t v[3]; fread(v, 2, 3, f); pts++;
      if (flags) continue;  // skip graticule in ASCII
      float P[3] = {v[0] / 32000.0f, v[1] / 32000.0f, v[2] / 32000.0f};
      const float sx = B.m[0]*P[0] + B.m[1]*P[1] + B.m[2]*P[2];
      const float sy = B.m[3]*P[0] + B.m[4]*P[1] + B.m[5]*P[2];
      const float sz = B.m[6]*P[0] + B.m[7]*P[1] + B.m[8]*P[2];
      if (sz <= 0) continue;
      const int c = (int)((sx + 1) / 2 * W), r = (int)((1 - sy) / 2 * H);
      if (r >= 0 && r < H && c >= 0 && c < W) g[r][c] = '#';
    }
  }
  fclose(f);
  printf("%u lines, %ld points\n", nl, pts);
  for (int r = 0; r < H; r++) printf("%s\n", g[r]);
  return 0;
}
