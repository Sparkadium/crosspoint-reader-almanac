#pragma once
// tex_math.h -- textured ("photo") globe rendering for the Almanac.
//
// Takes a TEX1 file (equirectangular 8-bit luminance, 1024x512, built by
// tools/almanac/make_texture.py from Solar System Scope imagery with the
// tone curve already baked in) and renders it as an orthographic globe,
// Floyd-Steinberg dithered to 1-bit, raster order, serpentine scan.
//
// RAM strategy for the ESP32-C3 (no PSRAM, fragile heap): the full-res
// texture (512KB) never lives in RAM. On photo-mode entry the file is
// streamed ONCE through a 2x2 box filter and packed to 4-bit into a single
// 64KB resident buffer (512x256, 16 grey levels). FS dithering at render
// time recovers the tonal resolution; the cost is some softness at hard
// edges (measured: MAE 3%, p95 11% against full-res sampling). Every render
// after that touches no storage.
//
// Optional second texture at load time: SCREEN blend (clouds over the day
// map), composited per-texel while streaming, so composites cost zero extra
// RAM at render time.
//
// Projection per pixel: pz and asin come from small LUTs built at load;
// longitude uses one float divide + a 257-entry atan table with octant
// fixup. No per-pixel libm calls. ~0.5s for a full R=214 disc at 160MHz.
//
// Pure logic, no Arduino, no display: host-verified by tools/almanac/
// tex_test.cpp (resident load bit-exact vs the Python reference, projection
// coords vs double precision, FS bit-exact vs the Python reference). If
// this file disagrees with that harness, this file is wrong.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include "globe_math.h"

namespace tex {

constexpr int SRC_W = 1024, SRC_H = 512;   // TEX1 file
constexpr int RES_W = 512, RES_H = 256;    // resident buffer (4-bit)
constexpr uint32_t RES_BYTES = (uint32_t)RES_W * RES_H / 2;  // 65536
constexpr int CHUNK_BYTES = 4096, N_CHUNKS = RES_BYTES / CHUNK_BYTES;

// The resident buffer as 16 x 4KB chunks: a fragmented C3 heap that cannot
// produce one 64KB block will almost always produce sixteen 4KB ones (same
// lesson as the fixed-ceiling decompression buffers). One shift+mask of
// indirection per sample.
struct ChunkedBuf {
  uint8_t* c[N_CHUNKS] = {nullptr};
  bool alloc() {
    for (int i = 0; i < N_CHUNKS; i++)
      if (!c[i] && !(c[i] = (uint8_t*)malloc(CHUNK_BYTES))) { release(); return false; }
    return true;
  }
  void release() {
    for (int i = 0; i < N_CHUNKS; i++) { free(c[i]); c[i] = nullptr; }
  }
  inline void put(uint32_t bi, uint8_t v) { c[bi >> 12][bi & 4095] = v; }
  inline uint8_t get(uint32_t bi) const { return c[bi >> 12][bi & 4095]; }
};

// ---------------------------------------------------------------------------
// TEX1 header. Reader is any type with: int read(void*, unsigned);
// void seek(uint32_t);   (same concept as cty_math.h)
// ---------------------------------------------------------------------------
template <typename Reader>
bool readHeader(Reader& f, uint16_t& w, uint16_t& h) {
  uint8_t hd[8];
  f.seek(0);
  if (f.read(hd, 8) != 8 || memcmp(hd, "TEX1", 4) != 0) return false;
  w = (uint16_t)hd[4] | ((uint16_t)hd[5] << 8);
  h = (uint16_t)hd[6] | ((uint16_t)hd[7] << 8);
  return w == SRC_W && h == SRC_H;
}

// ---------------------------------------------------------------------------
// Resident load: stream the file once, 2x2 box filter, optional SCREEN blend
// with a second file, pack 4-bit into buf (RES_BYTES). Integer semantics are
// exact and mirrored by the test harness:
//   box      = (a + b + c + d + 2) >> 2                (round to nearest)
//   screen   = 255 - ((255-a) * (255-b) + 127) / 255   (round to nearest)
//   quantize = (v * 15 + 127) / 255                    (round to nearest)
// Returns false on a bad header or short read. blend may be null.
// ---------------------------------------------------------------------------
template <typename Reader>
bool loadResident(Reader& f, Reader* blend, ChunkedBuf& buf) {
  uint16_t w, h;
  if (!readHeader(f, w, h)) return false;
  if (blend && !readHeader(*blend, w, h)) return false;
  static uint8_t rows[2][SRC_W];       // 2KB scratch, only if photo mode used
  static uint8_t rowsB[2][SRC_W];
  for (int ry = 0; ry < RES_H; ry++) {
    if (f.read(rows[0], SRC_W) != SRC_W) return false;
    if (f.read(rows[1], SRC_W) != SRC_W) return false;
    if (blend) {
      if (blend->read(rowsB[0], SRC_W) != SRC_W) return false;
      if (blend->read(rowsB[1], SRC_W) != SRC_W) return false;
      for (int r = 0; r < 2; r++)
        for (int x = 0; x < SRC_W; x++) {
          const int a = rows[r][x], b = rowsB[r][x];
          rows[r][x] = (uint8_t)(255 - ((255 - a) * (255 - b) + 127) / 255);
        }
    }
    for (int rx = 0; rx < RES_W; rx++) {
      const int v = (rows[0][2 * rx] + rows[0][2 * rx + 1] + rows[1][2 * rx] +
                     rows[1][2 * rx + 1] + 2) >> 2;
      const uint8_t q = (uint8_t)((v * 15 + 127) / 255);
      const uint32_t i = (uint32_t)ry * RES_W + rx;
      if (i & 1) buf.put(i >> 1, (uint8_t)(buf.get(i >> 1) | q));  // low nibble
      else       buf.put(i >> 1, (uint8_t)(q << 4));
    }
  }
  return true;
}

inline uint8_t sample4(const ChunkedBuf& buf, int ty, int tx) {
  const uint32_t i = (uint32_t)ty * RES_W + tx;
  const uint8_t b = buf.get(i >> 1);
  return (i & 1) ? (b & 0x0F) : (b >> 4);
}

// ---------------------------------------------------------------------------
// Projection tables. Built once per photo-mode entry (a few ms of libm on
// the host CPU is fine at entry time; never per pixel).
//   pzLut  : r^2 in [0,1] quantized to 4096 -> pz in [0,1] as uint16/65535
//   tyLut  : wz in [-1,1] quantized to 2048 -> texture row 0..RES_H-1
//   atanLut: t in [0,1] quantized to 256   -> angle in tx units, 0..RES_W/8
// tx covers 2*pi with RES_W units, so one octant (pi/4) is RES_W/8 = 64.
// ---------------------------------------------------------------------------
struct Luts {
  // three separate small allocations (8.2KB + 2KB + 0.3KB) instead of one
  // 10.6KB contiguous struct -- same fragmentation logic as ChunkedBuf
  uint16_t* pz = nullptr;
  uint8_t* ty = nullptr;
  uint8_t* atn = nullptr;
  bool alloc() {
    if (!pz) pz = (uint16_t*)malloc(sizeof(uint16_t) * 4097);
    if (!ty) ty = (uint8_t*)malloc(2049);
    if (!atn) atn = (uint8_t*)malloc(257);
    if (pz && ty && atn) return true;
    release();
    return false;
  }
  void release() {
    free(pz); pz = nullptr;
    free(ty); ty = nullptr;
    free(atn); atn = nullptr;
  }
  void build() {
    for (int i = 0; i <= 4096; i++) {
      const float r2 = (float)i / 4096.0f;
      const float p = r2 >= 1.0f ? 0.0f : sqrtf(1.0f - r2);
      pz[i] = (uint16_t)(p * 16384.0f + 0.5f);  // Q14
    }
    for (int i = 0; i <= 2048; i++) {
      const float wz = (float)i / 1024.0f - 1.0f;  // -1..1
      const float lat = asinf(wz < -1 ? -1 : (wz > 1 ? 1 : wz));  // radians
      int t = (int)((0.5f - lat / 3.14159265f) * RES_H);
      if (t < 0) t = 0;
      if (t >= RES_H) t = RES_H - 1;
      ty[i] = (uint8_t)t;
    }
    for (int i = 0; i <= 256; i++) {
      const float t = (float)i / 256.0f;
      // angle in [0, pi/4] mapped to tx units: RES_W per 2*pi
      atn[i] = (uint8_t)(atanf(t) * (RES_W / (2.0f * 3.14159265f)) + 0.5f);
    }
  }
};

// tx from (wx, wy) via octant reduction; one float divide, no libm.
// Returns 0..RES_W-1 where tx 0 = lon -180 (matches TEX1 layout: the file's
// column 0 is lon -180, and lon = atan2(wy,wx), so tx = (lon/2pi + 0.5)*W).
inline int txFrom(const Luts& L, int32_t wx, int32_t wy) {
  const int32_t ax = wx < 0 ? -wx : wx, ay = wy < 0 ? -wy : wy;
  int a;  // angle of (|wx|,|wy|) in tx units, 0..RES_W/4
  if (ax >= ay) {
    a = L.atn[ax ? (int)((ay * 256 + ax / 2) / ax) : 0];
  } else {
    a = RES_W / 4 - L.atn[(int)((ax * 256 + ay / 2) / ay)];
  }
  int t;  // full-circle angle in tx units, 0..RES_W-1, measured from +x CCW
  if (wx >= 0) t = (wy >= 0) ? a : (RES_W - a);
  else         t = (wy >= 0) ? (RES_W / 2 - a) : (RES_W / 2 + a);
  t = (t + RES_W / 2) % RES_W;  // shift so tx 0 = lon -180
  return t == RES_W ? 0 : t;
}

// ---------------------------------------------------------------------------
// Floyd-Steinberg state: TWO int16 error rows (screen width + 2 each),
// serpentine. cur is what this row inherits; 3/5/1 sixteenths are written
// into nxt; the 7/16 travels along the scan as a carry. A single shared row
// would let the 1/16 forward-diagonal contaminate the pixel about to be
// processed on the SAME row, so two rows it is (~2KB for a 480-wide panel).
// All error splits use >>4 (floor), mirrored exactly by the Python
// reference in tex_test.
// ---------------------------------------------------------------------------
struct FsRow {
  int16_t* err;   // 2 * (width + 2) entries; caller allocates
  int width;
  int16_t* cur;
  int16_t* nxt;
  void reset() {
    memset(err, 0, sizeof(int16_t) * 2 * (width + 2));
    cur = err;
    nxt = err + (width + 2);
  }
  void nextRow() {
    int16_t* t = cur; cur = nxt; nxt = t;
    memset(nxt, 0, sizeof(int16_t) * (width + 2));
  }
};

// ---------------------------------------------------------------------------
// Render the photo globe. Raster order, top to bottom, serpentine FS.
//   B        view basis (globe::viewBasis)
//   cx,cy,R  disc center and radius in screen pixels
//   xl,yt,xr,yb  clip rect, half-open on the right/bottom
//   bgWhite  what the caller painted outside the disc (bg pixels are NOT
//            plotted here; error never diffuses across the limb)
//   plot(x, y, white)  write one pixel
// Terminator is NOT applied here -- the Moon overlays its hard-edged night
// side afterwards (solid fill, physically right for an airless body), and
// the Earth photo modes are flat-lit in v1 (day/night blending needs a
// second resident buffer; see PHOTO_MODES notes).
// ---------------------------------------------------------------------------
template <typename Plot>
void renderPhotoGlobe(const globe::Basis& B, int cx, int cy, int R,
                      int xl, int yt, int xr, int yb,
                      const ChunkedBuf& buf, const Luts& L, FsRow& fs,
                      Plot plot) {
  // Q14 fixed point: the C3 has hardware integer MUL/DIV but no FPU, so a
  // float pipeline here runs 5-10x slower through soft-float calls.
  const int32_t exq = (int32_t)(B.m[0] * 16384), eyq = (int32_t)(B.m[1] * 16384);
  const int32_t nxq = (int32_t)(B.m[3] * 16384), nyq = (int32_t)(B.m[4] * 16384),
                nzq = (int32_t)(B.m[5] * 16384);
  const int32_t oxq = (int32_t)(B.m[6] * 16384), oyq = (int32_t)(B.m[7] * 16384),
                ozq = (int32_t)(B.m[8] * 16384);

  const int rowTop = cy - R < yt ? yt : cy - R;
  const int rowBot = cy + R >= yb ? yb - 1 : cy + R;
  fs.reset();

  for (int Y = rowTop; Y <= rowBot; Y++) {
    const int32_t pyq = (int32_t)(cy - Y) * 16384 / R;          // Q14
    const int32_t py2q = (pyq * pyq) >> 14;                     // Q14
    if (py2q >= 16384) { fs.nextRow(); continue; }
    const int32_t xlq = L.pz[(uint32_t)(py2q << 14) >> 16];     // sqrt(1-py2), Q14
    int X0 = cx - (int)((xlq * R) >> 14), X1 = cx + (int)((xlq * R) >> 14);
    if (X0 < xl) X0 = xl;
    if (X1 >= xr) X1 = xr - 1;
    if (X1 < X0) { fs.nextRow(); continue; }

    // row-constant world components from py*n, Q14
    const int32_t cxw = (pyq * nxq) >> 14, cyw = (pyq * nyq) >> 14,
                  czw = (pyq * nzq) >> 14;

    const bool ltr = ((Y - rowTop) & 1) == 0;
    const int xa = ltr ? X0 : X1, xb = ltr ? X1 : X0, xs = ltr ? 1 : -1;
    int16_t* cur = fs.cur;
    int16_t* nxt = fs.nxt;
    int carry = 0;  // the 7/16, travelling along the scan direction

    for (int X = xa; ltr ? X <= xb : X >= xb; X += xs) {
      const int32_t pxq = (int32_t)(X - cx) * 16384 / R;        // Q14
      const int32_t r2q = (pxq * pxq + pyq * pyq) >> 14;        // Q14
      if (r2q >= 16384) { carry = 0; continue; }  // bg: painted by caller
      const int32_t pzq = L.pz[(uint32_t)(r2q << 14) >> 16];    // Q14

      const int32_t wx = cxw + ((pxq * exq + pzq * oxq) >> 14);
      const int32_t wy = cyw + ((pxq * eyq + pzq * oyq) >> 14);
      const int32_t wz = czw + ((pzq * ozq) >> 14);   // e.z == 0

      int zi = (int)(((wz + 16384) * 2048) >> 15);
      if (zi < 0) zi = 0;
      if (zi > 2048) zi = 2048;
      const int ty = L.ty[zi];
      const int tx = txFrom(L, wx, wy);

      const int v = sample4(buf, ty, tx) * 17;      // 0..255
      const int want = v + cur[X + 1] + carry;
      const int outv = want >= 128 ? 255 : 0;
      const int e = want - outv;
      plot(X, Y, outv != 0);
      carry = (e * 7) >> 4;
      nxt[X + 1 - xs] = (int16_t)(nxt[X + 1 - xs] + ((e * 3) >> 4));
      nxt[X + 1]      = (int16_t)(nxt[X + 1] + ((e * 5) >> 4));
      nxt[X + 1 + xs] = (int16_t)(nxt[X + 1 + xs] + ((e * 1) >> 4));
    }
    fs.nextRow();
  }
}

// ---------------------------------------------------------------------------
// Hard night-side fill for airless bodies (the Moon): solid black past the
// terminator, no twilight band. Uses the same per-row span solver the
// shaded modes use. sv is the sun vector in VIEW frame.
// ---------------------------------------------------------------------------
template <typename Fill>
void fillNightSolid(const float sv[3], int cx, int cy, int R,
                    int xl, int yt, int xr, int yb, Fill fillRow) {
  const int rowTop = cy - R < yt ? yt : cy - R;
  const int rowBot = cy + R >= yb ? yb - 1 : cy + R;
  for (int Y = rowTop; Y <= rowBot; Y++) {
    const float py = (float)(cy - Y) / (float)R;
    float spans[4];
    const int n = globe::nightSpans(sv, py, 0.0f, spans);
    for (int s = 0; s < n; s++) {
      int X0 = cx + (int)ceilf(spans[s * 2] * R);
      int X1 = cx + (int)floorf(spans[s * 2 + 1] * R);
      if (X0 < xl) X0 = xl;
      if (X1 >= xr) X1 = xr - 1;
      if (X1 >= X0) fillRow(X0, X1, Y);
    }
  }
}

}  // namespace tex
