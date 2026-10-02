#pragma once
/*
  eye_render.h - layered eye compositor.

  Builds rows of a 240x240 RGB565 eye image from:
    1. SOCKET_HALF fixed dark socket (stored 120x120, doubled on the fly)
    2. IRIS_ART    iris disc + halo, shifted by the gaze offset
    3. slit pupil + hot glow, computed live (width and glow change every frame)
    4. curved upper/lower lids, computed live (blink, squint, angry slant)

  Speed notes:
    - The art is copied from flash into RAM once at start-up (useRamCopies()).
      Reading it straight from flash every frame was the main slowdown.
    - The hot loop is placed in IRAM so streaming art through the cache can't
      evict the code that's drawing it.
    - Integer-only per-pixel math (the C3 has no floating-point unit).
  No Arduino dependencies, so the same code compiles on a PC for test renders.
*/
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "eye_art.h"

#if defined(ESP_PLATFORM)
  #include "esp_attr.h"
  #include "esp_heap_caps.h"
  #define EYE_HOT IRAM_ATTR
#else
  #define EYE_HOT
#endif

namespace eye {

static constexpr int W = 240;
static constexpr int H = 240;
static constexpr int GLOW_LEN = ART_GLOW_LEN;

// Where the art is read from. Starts pointing at flash; useRamCopies() moves
// it into RAM.
static const uint16_t *gSocket = ART_STYLES[0].socket;
// The iris is stored as 8-bit palette indices plus a 256-colour table: half the
// size of full colour, which is what lets it sit in RAM beside Wi-Fi.
static const uint8_t *gIrisRows[240];       // one pointer per row: RAM where it fit, flash otherwise
static uint16_t gPal[256];                  // always in RAM; colour trim works on this
static constexpr int IRIS_CHUNK_ROWS = 24;  // 5.8 KB per block
static const uint8_t *gDist = DIST_LUT;
static int gLidSet = 0;
static const uint8_t *gLidUp = ART_LID_SETS[0].up;      // palette indices
static const uint8_t *gLidLo = ART_LID_SETS[0].lo;
static uint16_t gLidPal[256];                           // always in RAM

// The displays are round: about a fifth of every square frame is corner that
// nobody can see. These hold the first and last visible column of each row.
static int16_t gCircL[240], gCircR[240];
static bool gCircReady = false;

static void buildCircleSpans() {
  for (int y = 0; y < 240; ++y) {
    const float dy = (float)y + 0.5f - 120.0f;
    const float h = 120.0f * 120.0f - dy * dy;
    if (h <= 0) { gCircL[y] = 1; gCircR[y] = 0; continue; }
    const int half = (int)sqrtf(h);
    gCircL[y] = (int16_t)(120 - half < 0 ? 0 : 120 - half);
    gCircR[y] = (int16_t)(120 + half > 239 ? 239 : 120 + half);
  }
  gCircReady = true;
}
static const uint16_t *gMemb = ART_STYLES[0].membrane;
static const uint8_t *gGlowR = ART_STYLES[0].glowR;
static const uint8_t *gGlowG = ART_STYLES[0].glowG;
static const uint8_t *gGlowB = ART_STYLES[0].glowB;
static constexpr int LID_DEPTH = ART_LID_DEPTH;
static int gStyle = 0;
static bool gIrisInRam = false;    // false => the slanted drawing path would crawl
static bool gArtInPsram = false;  // true => art is in external PSRAM, much slower per pixel
static size_t gArtInternal = 0, gArtPsram = 0;
static uint32_t gRowsPlain = 0, gRowsSlanted = 0;   // which path rows actually took
// Color trim, applied to the RAM copies of the art (and to the glow table every
// frame, so the two always match). 256 = unchanged.
static int gBright256 = 256;   // overall brightness
static int gPale256 = 0;       // 0 = full color, 256 = grey/white
static int gWarm256 = 0;       // + warmer, - colder
static constexpr int STYLE_COUNT = ART_STYLE_COUNT;
static int16_t gSpanL[H], gSpanR[H];

// Copy art into internal RAM, most important first, but always leave at least
// `reserve` bytes free (Wi-Fi and the settings page need working room).
// Anything that doesn't fit stays in flash (slower, still correct).
// Returns bytes copied.
inline size_t useRamCopies(int style, int lidSet, size_t reserve) {
  if (style < 0 || style >= STYLE_COUNT) style = 0;
  gStyle = style;
  const ArtStyle &a = ART_STYLES[style];
  if (lidSet < 0 || lidSet >= ART_LID_SET_COUNT) lidSet = a.lidSet;
  gLidSet = lidSet;
  const LidSet &L = ART_LID_SETS[lidSet];
  gSocket = a.socket; gLidUp = L.up; gLidLo = L.lo; gMemb = a.membrane;
  memcpy(gPal, a.irisPal, sizeof(gPal));
  memcpy(gLidPal, L.pal, sizeof(gLidPal));
  gGlowR = a.glowR; gGlowG = a.glowG; gGlowB = a.glowB; gDist = DIST_LUT;
  for (int y = 0; y < H; ++y) gIrisRows[y] = a.irisIdx + (size_t)y * W;

  size_t total = 0;
  auto grab = [&](const void *src, size_t n) -> void * {
#if defined(ESP_PLATFORM)
    void *p = nullptr;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= n + reserve &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= n) {
      p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (p) gArtInternal += n;
    }
    // PSRAM is a distant second: external, and slow for scattered reads. Still
    // beats flash, but the slanted drawing path is not worth running from it.
    if (!p && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= n) {
      p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (p) { gArtPsram += n; gArtInPsram = true; }
    }
#else
    (void)reserve;
    void *p = malloc(n);
#endif
    if (p) { memcpy(p, src, n); total += n; }
    return p;
  };

  // The iris is copied in blocks of rows rather than one 115 KB lump, so a
  // fragmented heap can still take it. Each row then points at RAM or flash.
  gIrisInRam = true;
  for (int y0 = 0; y0 < H; y0 += IRIS_CHUNK_ROWS) {
    const int rows = (y0 + IRIS_CHUNK_ROWS <= H) ? IRIS_CHUNK_ROWS : (H - y0);
    void *p = grab(a.irisIdx + (size_t)y0 * W, (size_t)rows * W);
    if (p) {
      for (int r = 0; r < rows; ++r) gIrisRows[y0 + r] = (const uint8_t *)p + (size_t)r * W;
    } else {
      gIrisInRam = false;
    }
  }

  // Then in order of how badly each piece suffers from living in flash. The lids
  // are looked up at a different depth for every pixel, so they matter most.
  if (void *p = grab(L.up, LID_DEPTH * 120))        gLidUp = (const uint8_t *)p;
  if (void *p = grab(L.lo, LID_DEPTH * 120))        gLidLo = (const uint8_t *)p;
  if (void *p = grab(DIST_LUT, sizeof(DIST_LUT)))   gDist = (const uint8_t *)p;      // pupil tips
  if (void *p = grab(a.socket, 120 * 120 * 2))      gSocket = (const uint16_t *)p;   // every row
  if (void *p = grab(a.membrane, ART_MEMB_SIZE * ART_MEMB_SIZE * 2)) gMemb = (const uint16_t *)p;
  if (!gCircReady) buildCircleSpans();
  memcpy(gSpanL, a.spanL, sizeof(gSpanL));
  memcpy(gSpanR, a.spanR, sizeof(gSpanR));
  return total;
}

// Trim one 8-bit-per-channel color in place.
static inline void trimRGB(int &r, int &g, int &b) {
  if (gPale256) {
    const int lum = (r * 77 + g * 150 + b * 29) >> 8;
    r += ((lum - r) * gPale256) >> 8;
    g += ((lum - g) * gPale256) >> 8;
    b += ((lum - b) * gPale256) >> 8;
  }
  if (gWarm256) {
    r += (r * gWarm256) >> 9;
    b -= (b * gWarm256) >> 9;
  }
  if (gBright256 != 256) {
    r = (r * gBright256) >> 8;
    g = (g * gBright256) >> 8;
    b = (b * gBright256) >> 8;
  }
  if (r < 0) r = 0; else if (r > 255) r = 255;
  if (g < 0) g = 0; else if (g > 255) g = 255;
  if (b < 0) b = 0; else if (b > 255) b = 255;
}

// Re-copy the art from flash and apply the current trim. Takes a few
// milliseconds; called when the colour settings change, not per frame.
// Anything that did not fit in RAM keeps its original colours.
inline void applyColorTrim(int bright256, int pale256, int warm256) {
  gBright256 = bright256; gPale256 = pale256; gWarm256 = warm256;
  const ArtStyle &a = ART_STYLES[gStyle];
  struct Job { uint16_t *dst; const uint16_t *src; size_t n; };
  for (int i = 0; i < 256; ++i) {                     // iris: just the palette
    const uint16_t c = a.irisPal[i];
    int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    trimRGB(r, g, b);
    gPal[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  }
  for (int i = 0; i < 256; ++i) {                     // lids: palette only
    const uint16_t c = ART_LID_SETS[gLidSet].pal[i];
    int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    trimRGB(r, g, b);
    gLidPal[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  }
  const Job jobs[] = {
    { (uint16_t *)gSocket, a.socket,   120 * 120 },
    { (uint16_t *)gMemb,   a.membrane, (size_t)ART_MEMB_SIZE * ART_MEMB_SIZE },
  };
  for (const Job &j : jobs) {
    if (j.dst == j.src) continue;              // still in flash, can't be trimmed
    for (size_t i = 0; i < j.n; ++i) {
      const uint16_t c = j.src[i];
      int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
      trimRGB(r, g, b);
      j.dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
}

// Geometry of the style in use: beast eyes nearly fill the glass, a human iris
// is small inside a white sclera, so these differ per style.
inline float irisRadiusPx()  { return ART_STYLES[gStyle].irisRadiusPx; }
inline float pupilHalfWPx()  { return ART_STYLES[gStyle].pupilHalfW; }
inline float pupilHalfHPx()  { return ART_STYLES[gStyle].pupilHalfH; }
inline float pupilRoundRPx() { return ART_STYLES[gStyle].pupilRoundR; }
inline int   styleLidSet() { return ART_STYLES[gStyle].lidSet; }
inline bool  styleWantsRoundPupil() { return ART_STYLES[gStyle].roundPupilDefault; }

// True when the iris art is in RAM. When it isn't, rotation/squash/tissue are
// skipped: sampling flash pixel by pixel costs about ten times as much.
inline bool irisInRam() { return gIrisInRam; }

// True when any art had to go to PSRAM instead of internal RAM.
inline bool artInPsram() { return gArtInPsram; }
inline size_t artInternalBytes() { return gArtInternal; }
inline size_t artPsramBytes() { return gArtPsram; }

// How many rows took each drawing path since the last call (then resets).
inline void takeRowCounts(uint32_t &plain, uint32_t &slanted) {
  plain = gRowsPlain; slanted = gRowsSlanted;
  gRowsPlain = gRowsSlanted = 0;
}

// How much art there is in total, for the start-up report.
inline size_t artBytesTotal() {
  return 240 * 240 + 2 * (size_t)LID_DEPTH * 120 + 120 * 120 * 2
         + (size_t)ART_MEMB_SIZE * ART_MEMB_SIZE * 2 + sizeof(DIST_LUT);
}

// Everything the compositor needs for one eye, one frame.
struct Frame {
  int16_t dx = 0, dy = 0;          // iris offset in pixels (+x right, +y down)
  int16_t offX = 0, offY = 0;      // whole-eye nudge, to line up screens that
                                   // ended up mounted slightly off from each other
  int32_t cos16 = 65536, sin16 = 0;  // eye rotation (16.16), for a screen glued crooked
  int32_t squash16 = 65536;        // horizontal squash of the iris, 16.16. Below
                                   // 1.0 when the eye is turned far to a side, the
                                   // way a real iris foreshortens.
  int64_t invH48 = 0;              // (1<<48)/pupilHalfH16^2, filled by buildGlow
  uint16_t pull256 = 0;            // how hard the tissue behind the eye is stretched
  int16_t pullX = 0, pullY = 0;    // gaze direction, 0..256 scale (tissue drags opposite)
  int32_t pupilHalfW16 = 0;        // pupil half width at its middle, 1/16 px
  int32_t pupilHalfH16 = 0;        // pupil half height, 1/16 px
  bool pupilRound = false;         // true = round pupil, false = vertical slit
  int32_t pupilR16 = 0;            // round pupil radius, 1/16 px
  uint16_t glow256 = 256;          // glow strength, 256 = normal
  uint8_t glowR5[GLOW_LEN], glowG6[GLOW_LEN], glowB5[GLOW_LEN];  // glow already scaled
  int32_t upper16[W];              // upper lid edge per column, 1/16 px rows
  int32_t lower16[W];              // lower lid edge per column, 1/16 px rows
  int32_t upperMax16 = 0;          // lowest point of the upper lid
  int32_t lowerMin16 = 0;          // highest point of the lower lid
  int32_t upperMin16 = 0;          // highest point of the upper lid
  int32_t lowerMax16 = 0;          // lowest point of the lower lid
  bool lidArt = true;              // false = plain black lids
};

// Shadow the lids cast on the eye, by pixels from the lid edge (256 = none).
static const uint16_t UPPER_SHADOW[10] = { 110, 135, 158, 180, 199, 216, 230, 241, 250, 256 };
static const uint16_t LOWER_SHADOW[5]  = { 170, 200, 225, 245, 256 };
static constexpr int32_t SHADOW_REACH16 = 10 * 16;

static inline uint16_t blend565(uint16_t a, uint16_t b, uint32_t a256) {
  // a * a256/256 + b * (1 - a256/256)
  uint32_t ib = 256 - a256;
  uint32_t r = ((a >> 11) * a256 + (b >> 11) * ib) >> 8;
  uint32_t g = (((a >> 5) & 63) * a256 + ((b >> 5) & 63) * ib) >> 8;
  uint32_t bl = ((a & 31) * a256 + (b & 31) * ib) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Lid shape inputs (all 0..1 unless noted).
struct LidShape {
  float blink = 0;     // 1 = fully shut
  float squint = 0;    // 1 = heavy squint (upper lid past halfway, lower lid rises)
  float droop = 0;     // sleepy upper lid, 1 = upper lid at centre line
  float wide = 0;      // startled, lifts upper lid and flattens the angry slant
  float angry = 0.35f; // slant of the upper lid toward the nose
  bool mirror = false; // true for the eye whose nose side is on its left
  int offX = 0, offY = 0;  // same whole-eye nudge as Frame
  float cosT = 1.0f, sinT = 0.0f;  // eye rotation, same as the Frame's
};

static inline uint16_t scale565(uint16_t c, uint32_t a256) {
  uint32_t r = ((c >> 11) * a256) >> 8;
  uint32_t g = (((c >> 5) & 63) * a256) >> 8;
  uint32_t b = ((c & 31) * a256) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t addSat565(uint16_t c, uint32_t r5, uint32_t g6, uint32_t b5) {
  uint32_t r = (c >> 11) + r5;
  uint32_t g = ((c >> 5) & 63) + g6;
  uint32_t b = (c & 31) + b5;
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Pre-scale the glow table for this frame's glow strength.
inline void buildGlow(Frame &f) {
  const int64_t h2 = (int64_t)f.pupilHalfH16 * f.pupilHalfH16;
  f.invH48 = h2 > 0 ? (((int64_t)1 << 48) / h2) : 0;
  const uint32_t gs = f.glow256;
  for (int i = 0; i < GLOW_LEN; ++i) {
    int r = gGlowR[i], g = gGlowG[i], b = gGlowB[i];
    trimRGB(r, g, b);
    f.glowR5[i] = (uint8_t)(((r * gs) >> 8) >> 3);
    f.glowG6[i] = (uint8_t)(((g * gs) >> 8) >> 2);
    f.glowB5[i] = (uint8_t)(((b * gs) >> 8) >> 3);
  }
}

// Build the lid curves for one eye (240 columns, float is fine here).
inline void buildLids(Frame &f, const LidShape &s) {
  float close = s.blink;
  if (s.squint * 0.75f > close) close = s.squint * 0.75f;
  if (s.droop * 0.85f > close) close = s.droop * 0.85f;
  const float angry = s.angry * (1.0f - s.wide);
  const float topY = -1.15f + close * 1.15f + 0.10f - s.wide * 0.12f * (1.0f - close);
  const float botY = 1.15f - s.blink * 1.15f - s.squint * 0.55f;
  const float c = s.cosT, sn = s.sinT;

  // Lid edge in eye-local coordinates: v = curve(u).
  auto curve = [&](float u, bool upper) {
    if (s.mirror) u = -u;
    return upper ? topY + 0.28f * u * u + angry * 0.25f * (u + 1.0f)
                 : botY - 0.18f * u * u;
  };

  int32_t upMax = -1000000, loMin = 1000000, upMin = 1000000, loMax = -1000000;
  for (int x = 0; x < W; ++x) {
    const float X = ((float)(x - s.offX) + 0.5f - 120.0f) / 120.0f;
    float ys[2];
    for (int k = 0; k < 2; ++k) {
      const bool upper = (k == 0);
      // Solve for the screen row where the rotated lid edge crosses this column.
      // Two rounds of substitution is plenty at the angles involved.
      float yv = curve(X, upper);
      if (sn != 0.0f) {
        for (int it = 0; it < 2; ++it) {
          const float u = X * c + yv * sn;
          const float v = curve(u, upper);
          yv = (v + X * sn) / (c != 0.0f ? c : 1.0f);
        }
      }
      ys[k] = yv;
    }
    int32_t up16 = (int32_t)lroundf((ys[0] + 1.0f) * 120.0f * 16.0f) + s.offY * 16;
    int32_t lo16 = (int32_t)lroundf((ys[1] + 1.0f) * 120.0f * 16.0f) + s.offY * 16;
    if (up16 > lo16) up16 = lo16 = (up16 + lo16) / 2;   // lids meet at a seam
    if (up16 < upMin) upMin = up16;
    if (lo16 > loMax) loMax = lo16;
    f.upper16[x] = up16;
    f.lower16[x] = lo16;
    if (up16 > upMax) upMax = up16;
    if (lo16 < loMin) loMin = lo16;
  }
  f.upperMax16 = upMax;
  f.lowerMin16 = loMin;
  f.upperMin16 = upMin;
  f.lowerMax16 = loMax;
}

// Compose rows [y0, y0+n) into out (n*240 pixels). swapBytes = big-endian
// output, which is what the display wants when fed by DMA.
//
// Two paths: a straight one when the eye is upright and centred-ish (plain
// copies), and a general one that samples the iris through a rotate/squash
// transform and draws the stretched tissue behind the eye. The straight path is
// used whenever rotation, squash and pull are all off.
static EYE_HOT void composeRows(const Frame &f, int y0, int n, uint16_t *out, bool swapBytes) {
  const int dx = f.dx + f.offX, dy = f.dy + f.offY;   // offsets shift the whole eye
  const int cx16 = (120 + dx) * 16;                   // iris centre, 1/16 px
  const int cy16 = (120 + dy) * 16;
  const int64_t h16 = f.pupilHalfH16;
  const int64_t h16sq = h16 * h16;
  const int32_t squashOff = f.squash16 - 65536;
  const bool plain = (f.sin16 == 0 && f.cos16 == 65536 && f.pull256 == 0 &&
                      squashOff > -1024 && squashOff < 1024);

  for (int row = 0; row < n; ++row) {
    const int y = y0 + row;
    uint16_t *o = out + row * W;
    const int32_t yc16 = y * 16 + 8;

    // Only the part of the row inside the round glass is worth drawing; the
    // corners are blacked out once and skipped by every pass below.
    const int cL = gCircL[y], cR = gCircR[y];
    if (cL > 0) memset(o, 0, cL * sizeof(uint16_t));
    if (cR < W - 1) memset(o + cR + 1, 0, (W - 1 - cR) * sizeof(uint16_t));
    if (cL > cR) continue;

    const bool rowCovered = (yc16 + 8 <= f.upperMin16) || (yc16 - 8 >= f.lowerMax16);
    int opL = 1, opR = 0;

    if (rowCovered) {
      memset(o + cL, 0, (cR - cL + 1) * sizeof(uint16_t));
    } else if (plain) {
      // ---- socket, then the iris layer on top, in one walk of the row ----
      const int sy = y - f.offY;
      const uint16_t *sh = (sy >= 0 && sy < H) ? gSocket + (sy >> 1) * 120 : nullptr;
      const int yl = y - dy;
      const uint8_t *iris = (yl >= 0 && yl < H) ? gIrisRows[yl] - dx : nullptr;

      int lx0 = cL, lx1 = cR + 1;                 // where the iris layer exists
      if (iris) {
        if (dx > cL) lx0 = dx;
        if (W + dx < lx1) lx1 = W + dx;
        if (gSpanL[yl] <= gSpanR[yl]) {
          opL = gSpanL[yl] + dx;
          opR = gSpanR[yl] + dx;
          if (opL < lx0) opL = lx0;
          if (opR > lx1 - 1) opR = lx1 - 1;
        }
      } else {
        lx0 = lx1 = cL;
      }

      // socket only, left of the iris layer
      for (int x = cL; x < lx0; ++x) o[x] = sh ? sh[(x - f.offX) >> 1] : 0;
      // halo: iris added on top of socket
      for (int x = lx0, e = (opL <= opR ? opL : lx1); x < e; ++x)
        o[x] = (uint16_t)((sh ? sh[(x - f.offX) >> 1] : 0) + gPal[iris[x]]);
      // the iris disc itself
      if (opL <= opR) for (int x = opL; x <= opR; ++x) o[x] = gPal[iris[x]];
      // halo on the far side
      for (int x = (opL <= opR ? opR + 1 : lx1); x < lx1; ++x)
        o[x] = (uint16_t)((sh ? sh[(x - f.offX) >> 1] : 0) + gPal[iris[x]]);
      // socket only, right of the iris layer
      for (int x = lx1; x <= cR; ++x) o[x] = sh ? sh[(x - f.offX) >> 1] : 0;
    } else {
      // ---- general path: rotate + squash the iris, drag the tissue ----
      const int32_t rely16 = yc16 - cy16;
      const int32_t relx16 = (cL * 16 + 8) - cx16;
      int32_t ur = (int32_t)(((int64_t)relx16 * f.cos16 + (int64_t)rely16 * f.sin16) >> 4);
      int32_t vr = (int32_t)((-(int64_t)relx16 * f.sin16 + (int64_t)rely16 * f.cos16) >> 4);
      const int32_t durx = f.cos16, dvrx = -f.sin16;
      const int32_t inv = (int32_t)(((int64_t)65536 << 16) / (f.squash16 ? f.squash16 : 65536));
      int32_t u = (int32_t)(((int64_t)ur * inv) >> 16);
      const int32_t du = (int32_t)(((int64_t)durx * inv) >> 16);
      const int sy = y - f.offY;
      const uint16_t *sh = (sy >= 0 && sy < H) ? gSocket + (sy >> 1) * 120 : nullptr;
      const int pullX = f.pullX, pullY = f.pullY;
      const uint32_t pull = f.pull256;

      for (int x = cL; x <= cR; ++x, ur += durx, vr += dvrx, u += du) {
        const int sxp = 120 + (u >> 16);
        const int syp = 120 + (vr >> 16);
        uint16_t c;
        if ((unsigned)syp < (unsigned)H && (unsigned)sxp < (unsigned)W &&
            sxp >= gSpanL[syp] && sxp <= gSpanR[syp]) {
          c = gPal[gIrisRows[syp][sxp]];
          if (opL > opR) opL = x;
          opR = x;
        } else {
          c = sh ? sh[(x - f.offX) >> 1] : 0;
          if ((unsigned)syp < (unsigned)H && (unsigned)sxp < (unsigned)W)
            c = (uint16_t)(c + gPal[gIrisRows[syp][sxp]]);
          if (pull) {
            const int32_t px = ur >> 16, py = vr >> 16;
            const int32_t along = (-(px * pullX + py * pullY)) >> 8;
            if (along > 0) {
              int mx = (px + 120) >> 1, my = (py + 120) >> 1;
              if (mx < 0) mx = 0; else if (mx >= ART_MEMB_SIZE) mx = ART_MEMB_SIZE - 1;
              if (my < 0) my = 0; else if (my >= ART_MEMB_SIZE) my = ART_MEMB_SIZE - 1;
              const uint16_t m = gMemb[my * ART_MEMB_SIZE + mx];
              uint32_t k = (uint32_t)along * 5;
              if (k > 256) k = 256;
              k = (k * pull) >> 8;
              c = addSat565(c, ((m >> 11) * k) >> 8, (((m >> 5) & 63) * k) >> 8, ((m & 31) * k) >> 8);
            }
          }
        }
        o[x] = c;
      }
    }

    // ---- pupil and glow ----
    if (opL <= opR) {
      const bool rotated = (f.sin16 != 0);
      const int32_t rely16 = yc16 - cy16;
      const int32_t relx16 = (opL * 16 + 8) - cx16;
      int32_t lx16 = rotated
          ? (int32_t)(((int64_t)relx16 * f.cos16 + (int64_t)rely16 * f.sin16) >> 16) : relx16;
      int32_t lv16 = rotated
          ? (int32_t)((-(int64_t)relx16 * f.sin16 + (int64_t)rely16 * f.cos16) >> 16) : rely16;
      const int32_t stepU = rotated ? (int32_t)(((int64_t)f.cos16 * 16) >> 16) : 16;
      const int32_t stepV = rotated ? (int32_t)((-(int64_t)f.sin16 * 16) >> 16) : 0;

      const int64_t r16 = f.pupilR16;
      const int64_t r16sq = r16 * r16;
      int32_t rowHw16 = 0, rowBeyond = GLOW_LEN;
      bool rowInSlit = false;
      if (!rotated) {
        const int64_t lv2 = (int64_t)lv16 * lv16;
        if (f.pupilRound) {
          rowInSlit = lv2 < r16sq;                       // inside the circle's height
          rowHw16 = rowInSlit ? (int32_t)sqrtf((float)(r16sq - lv2)) : 0;
          const int32_t alv = lv16 < 0 ? -lv16 : lv16;
          rowBeyond = rowInSlit ? 0 : (int32_t)((alv - r16) >> 4);
        } else {
          rowInSlit = lv2 < h16sq;
          rowHw16 = rowInSlit ? (int32_t)((int64_t)f.pupilHalfW16 * (h16sq - lv2) / h16sq) : 0;
          const int32_t alv = lv16 < 0 ? -lv16 : lv16;
          rowBeyond = rowInSlit ? 0 : (int32_t)((alv - h16) >> 4);
        }
      }
      if (rotated || rowBeyond < GLOW_LEN) {
        // only the band around the pupil can change
        int xa = opL, xb = opR;
        if (!rotated) {
          const int reach = (rowHw16 >> 4) + GLOW_LEN + 1;
          const int cxp = cx16 >> 4;
          if (cxp - reach > xa) xa = cxp - reach;
          if (cxp + reach < xb) xb = cxp + reach;
          lx16 += (xa - opL) * 16;
        }
        for (int x = xa; x <= xb; ++x, lx16 += stepU, lv16 += stepV) {
          bool inSlit;
          int32_t hw16, beyond;
          if (rotated) {
            const int64_t lv2 = (int64_t)lv16 * lv16;
            const int64_t limit = f.pupilRound ? r16sq : h16sq;
            inSlit = lv2 < limit;
            if (inSlit) {
              if (f.pupilRound) {
                hw16 = (int32_t)sqrtf((float)(r16sq - lv2));
              } else {
                const int32_t frac = (int32_t)(((h16sq - lv2) * f.invH48) >> 32);
                hw16 = (int32_t)(((int64_t)f.pupilHalfW16 * frac) >> 16);
              }
              beyond = 0;
            } else {
              hw16 = 0;
              const int32_t alv = lv16 < 0 ? -lv16 : lv16;
              beyond = (alv - (int32_t)(f.pupilRound ? r16 : h16)) >> 4;
              if (beyond >= GLOW_LEN) continue;
            }
          } else {
            inSlit = rowInSlit; hw16 = rowHw16; beyond = rowBeyond;
          }
          int32_t ax16 = lx16 < 0 ? -lx16 : lx16;
          int gi;
          int32_t d16 = 1000;
          if (inSlit) {
            d16 = ax16 - hw16;
            if (d16 <= -8) { o[x] = 0; continue; }
            gi = d16 > 0 ? (d16 >> 4) : 0;
          } else {
            const int axp = ax16 >> 4;
            if (axp >= 64) continue;
            gi = gDist[beyond * 64 + axp];
          }
          uint16_t c = o[x];
          if (gi < GLOW_LEN) c = addSat565(c, f.glowR5[gi], f.glowG6[gi], f.glowB5[gi]);
          if (d16 < 8) c = scale565(c, (uint32_t)(d16 + 8) * 16);
          o[x] = c;
        }
      }
    }

    // ---- lids ----
    if (yc16 - 8 < f.upperMax16 + SHADOW_REACH16 || yc16 + 8 > f.lowerMin16 - 5 * 16) {
      for (int x = cL; x <= cR; ++x) {
        const int32_t up = f.upper16[x], lo = f.lower16[x];
        int32_t vu = yc16 - up + 8;
        int32_t vl = lo - yc16 + 8;
        if (vu >= 16 && vl >= 16) {
          int32_t su = (yc16 - up) >> 4, sl = (lo - yc16) >> 4;
          uint32_t k = 256;
          if (su < 10) k = UPPER_SHADOW[su];
          if (sl < 5 && LOWER_SHADOW[sl] < k) k = LOWER_SHADOW[sl];
          if (k < 256) o[x] = scale565(o[x], k);
          continue;
        }
        uint16_t lidc = 0;
        if (f.lidArt) {
          const bool upper = vu < vl;
          int32_t d16 = (upper ? (up - yc16) : (yc16 - lo)) - 8;
          const uint8_t *tex = upper ? gLidUp : gLidLo;
          const int col = x >> 1;
          if (d16 <= 0) {
            lidc = gLidPal[tex[col]];
          } else if (d16 < (LID_DEPTH - 1) * 16) {
            int32_t i = d16 >> 4, fr = d16 & 15;
            lidc = fr ? blend565(gLidPal[tex[i * 120 + col]], gLidPal[tex[(i + 1) * 120 + col]],
                                 (uint32_t)(256 - fr * 16))
                      : gLidPal[tex[i * 120 + col]];
          } else {
            lidc = gLidPal[tex[(LID_DEPTH - 1) * 120 + col]];
          }
        }
        int32_t v = vu < vl ? vu : vl;
        o[x] = (v <= 0 || rowCovered) ? lidc : blend565(o[x], lidc, (uint32_t)v * 16);
      }
    }

    // ---- byte order for the display, two pixels at a time ----
    if (swapBytes) {
      int x = cL;
      if (x & 1) { o[x] = (uint16_t)((o[x] << 8) | (o[x] >> 8)); ++x; }
      uint32_t *p = (uint32_t *)(o + x);
      const int pairs = (cR - x + 1) >> 1;
      for (int i = 0; i < pairs; ++i) {
        const uint32_t v = p[i];
        p[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      }
      for (x += pairs * 2; x <= cR; ++x) o[x] = (uint16_t)((o[x] << 8) | (o[x] >> 8));
    }
  }
}

}  // namespace eye
