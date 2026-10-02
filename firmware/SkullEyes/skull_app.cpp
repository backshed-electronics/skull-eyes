// skull_app.cpp - all Skull Eyes logic. Settings are near the top.
// (Kept in a .cpp so the Arduino IDE's automatic prototype step can't
//  reorder anything.)

// Build with one eye style only (Classic Pumpkin) for roughly half the build
// time: set to 1 and the Ghostly Skeleton artwork is left out of the build.
#define SINGLE_STYLE_ONLY 0

#include <Arduino.h>
#include <Preferences.h>

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select the XIAO ESP32-S3 board (Tools > Board), with Tools > PSRAM > OPI PSRAM."
#endif
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include "eye_render.h"
#include "skull_web.h"

// =====================================================================
//                         SETTINGS YOU MAY CHANGE
// =====================================================================

// Colors look wrong (blue/negative-looking)? Flip this.
static constexpr bool DISPLAY_INVERT = true;

// A display mounted upside down? Set its rotation to 2 (0 = normal).
static constexpr uint8_t LEFT_ROTATION  = 0;
static constexpr uint8_t RIGHT_ROTATION = 0;

// The angry brows should slant DOWN toward the middle of the face.
// If they slant down toward the outside (looks worried/sad), flip this.
static constexpr bool SWAP_BROW_SLANT = false;

// Textured eyelids (true) or plain black lids (false).
static constexpr bool LID_ARTWORK = true;

// How angry the resting brow is: 0 = none, 0.35 = default, 0.6 = furious.
static constexpr float BROW_ANGER = 0.35f;

// ---- Motion feel ----
// Eye movement speed. 1.0 = slow, intense stare (default). 2.0 = twice as fast.
static constexpr float MOVE_SPEED = 1.0f;
// How long a glide takes: base time plus extra for longer moves (at MOVE_SPEED 1.0).
static constexpr float GLIDE_BASE_MS = 450.0f;
static constexpr float GLIDE_PER_UNIT_MS = 900.0f;

// Blink speed. 1.0 = default (about half a second per blink). Lower = slower.
static constexpr float BLINK_SPEED = 1.0f;
// Random time between blinks, in seconds.
static constexpr float BLINK_EVERY_MIN_S = 4.5f;
static constexpr float BLINK_EVERY_MAX_S = 11.0f;

// How quickly squints close and open (seconds; bigger = slower squeeze).
static constexpr float SQUINT_TAU_S = 0.45f;

// How far the iris can travel, in pixels from centre. The side-to-side figure is
// the default for the settings page ("Eye travel"); up/down stays fixed.
static constexpr float GAZE_RANGE_X_PX = 40.0f;
static constexpr float GAZE_RANGE_Y_PX = 20.0f;

// At a full side-glance the iris foreshortens (like a real eye turning) and the
// tissue behind it stretches. 0 turns each effect off.
static constexpr float GAZE_SQUASH = 0.32f;     // 0.32 = 32% narrower at full travel
static constexpr float TISSUE_PULL = 0.85f;     // strength of the stretched tissue

// Display clock. Set on the settings page (40/60/80 MHz); this is the fallback
// used before the settings have loaded. It caps the frame rate: pushing both
// eyes takes about 46 ms at 40 MHz, 23 ms at 80 MHz.
static constexpr uint32_t SPI_HZ_DEFAULT = 40000000;
static uint32_t SPI_HZ = SPI_HZ_DEFAULT;

// ---- Screen power switch (optional hardware) ----
// With a relay or high-side MOSFET switch feeding +5 V to the screens and radar,
// off-times really are off. Without one, screens just go black at off-times.
// See README "Power switch" for wiring. Set true once it's wired.
static constexpr bool POWER_SWITCH_INSTALLED = false;
static constexpr int  POWER_SWITCH_PIN = D5;          // GPIO7, free on this wiring
static constexpr bool POWER_SWITCH_ON_HIGH = true;    // pin HIGH = screens powered

// Background (DMA) sending of frames. Leave true. If the displays ever stay
// black or garbled after an update, set false to use the older, slower path.
static constexpr bool USE_DMA = true;

// ---- Radar (only matters once the RD-03D is connected) ----
// Eyes look AWAY from you when you walk to one side? Flip this.
static constexpr bool RADAR_X_INVERT = false;
// Near cutoff now lives on the settings page (default 750 mm). This is only the
// fallback used before the settings have loaded.
static constexpr int   RADAR_MIN_RANGE_MM = 750;    // ignore closer than this
static constexpr int   RADAR_MAX_RANGE_MM = 6000;   // ignore farther than this
static constexpr float RADAR_MAX_ANGLE_DEG = 60.0f; // ignore wider than this
static constexpr float RADAR_FULL_GAZE_DEG = 60.0f; // angle that puts eyes fully to the side
                                                   // (matches the RD-03D's +/-60 coverage)
static constexpr int   RADAR_CLOSE_MM = 1200;       // "too close" glare distance

// =====================================================================
//                              PINS
// =====================================================================
static constexpr int PIN_TFT_SCK      = D8;   // GPIO8
static constexpr int PIN_TFT_MOSI     = D10;  // GPIO10
static constexpr int PIN_TFT_DC       = D3;   // GPIO5
static constexpr int PIN_TFT_CS_LEFT  = D1;   // GPIO3
static constexpr int PIN_TFT_CS_RIGHT = D2;   // GPIO4
static constexpr int PIN_TFT_RST      = D4;   // GPIO6
static constexpr int PIN_RADAR_RX     = D7;   // GPIO20
static constexpr int PIN_RADAR_TX_NC  = D6;   // GPIO21, not wired
static constexpr uint32_t RADAR_BAUD  = 256000;

Adafruit_GC9A01A leftTft (&SPI, PIN_TFT_DC, PIN_TFT_CS_LEFT,  -1);
Adafruit_GC9A01A rightTft(&SPI, PIN_TFT_DC, PIN_TFT_CS_RIGHT, -1);

// =====================================================================
//                              HELPERS
// =====================================================================
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float randf(float lo, float hi) { return lo + (hi - lo) * ((float)esp_random() / 4294967295.0f); }
static inline uint32_t randMs(uint32_t lo, uint32_t hi) { return hi <= lo ? lo : lo + esp_random() % (hi - lo + 1); }
static inline bool chance(float p) { return randf(0, 1) < p; }
// Move v toward target with time constant tau (seconds), frame-rate independent.
static inline float approach(float v, float target, float tau, float dt) {
  if (tau <= 0.0f) return target;
  return v + (target - v) * (1.0f - expf(-dt / tau));
}
static inline float smooth01(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

// =====================================================================
//                               RADAR
// =====================================================================
struct RadarTarget {
  bool valid = false;
  int16_t x_mm = 0, y_mm = 0, speed_cm_s = 0;
  float range_mm = 0, angle_deg = 0;
};
RadarTarget radar;
uint32_t radarLastSeenMs = 0;
uint32_t radarFrames = 0;
float radarAngleSmoothed = 0;

// ---- diagnostics (prints what the radar is actually saying) ----
// Set false once tracking is dialled in.
static constexpr bool RADAR_DEBUG = true;
uint32_t radarBytes = 0, radarBadHeader = 0, radarBadTail = 0;
uint32_t radarSlotsSeen = 0, radarAccepted = 0;
uint32_t rejNear = 0, rejFar = 0, rejAngle = 0, rejStatic = 0, rejClutter = 0;

// ---- the room's fixed echoes ----
// Appliances, cabinets and walls all look like targets. Their positions are
// learned (on request, or quietly whenever something sits still for a while)
// and then ignored, unless something is clearly walking through that spot.
MapTarget mapSeen[3];
int mapSeenCount = 0;

struct ClutterSpot { int16_t x, y; };
static constexpr int CLUTTER_MAX = 16;
static constexpr int CLUTTER_RADIUS_MM = 600;   // how close counts as "that thing again"
static constexpr int CLUTTER_WALK_CM_S = 15;    // clearly walking: seen even in a clutter spot
ClutterSpot clutter[CLUTTER_MAX];
int clutterCount = 0;
uint32_t learnUntilMs = 0;

static void clutterLoad() {
  Preferences p;
  p.begin("room", true);
  size_t n = p.getBytesLength("spots");
  if (n && n <= sizeof(clutter)) {
    p.getBytes("spots", clutter, n);
    clutterCount = n / sizeof(ClutterSpot);
  }
  p.end();
}

static void clutterSave() {
  Preferences p;
  p.begin("room", false);
  if (clutterCount) p.putBytes("spots", clutter, clutterCount * sizeof(ClutterSpot));
  else p.remove("spots");
  p.end();
}

static bool clutterHas(int32_t x, int32_t y) {
  for (int i = 0; i < clutterCount; ++i) {
    const int32_t dx = x - clutter[i].x, dy = y - clutter[i].y;
    if (dx * dx + dy * dy < (int32_t)CLUTTER_RADIUS_MM * CLUTTER_RADIUS_MM) return true;
  }
  return false;
}

static void clutterAdd(int32_t x, int32_t y) {
  if (clutterHas(x, y)) return;
  if (clutterCount >= CLUTTER_MAX) return;
  clutter[clutterCount].x = (int16_t)x;
  clutter[clutterCount].y = (int16_t)y;
  clutterCount++;
  Serial.printf("[room] learned a fixed echo at %.1f m, %+.0f deg (%d known)\n",
                sqrtf((float)x * x + (float)y * y) / 1000.0f,
                atan2f((float)x, (float)y) * 180.0f / PI, clutterCount);
}
uint8_t lastFrame[30];
bool haveLastFrame = false;
int16_t dbgX = 0, dbgY = 0, dbgV = 0;
float dbgRange = 0, dbgAngle = 0;
const char *dbgVerdict = "none";

static int16_t decodeRadarSigned(uint8_t lo, uint8_t hi) {
  uint16_t raw = (uint16_t)lo | ((uint16_t)hi << 8);
  int16_t mag = (int16_t)(raw & 0x7FFF);
  return (raw & 0x8000) ? mag : -mag;
}

static void processRadarFrame(const uint8_t *f) {
  radarFrames++;
  memcpy(lastFrame, f, 30);
  haveLastFrame = true;

  // The radar reports up to three targets. Indoors most of them are furniture,
  // walls and appliances, so a target has to be MOVING to be picked up. Once
  // someone is being followed they can stand still for a while and keep the
  // eyes' attention; only after that does it let go.
  static uint32_t lastMovingMs = 0;
  static float lastX = 0, lastY = 0;
  const uint32_t now = millis();
  const int nearCut = webRadarNearCutoffMm();
  const bool movingOnly = webFollowMovingOnly();
  const uint32_t holdMs = (uint32_t)webStillHoldSecs() * 1000;
  const int MOVING_CM_S = 5;                    // slower than this counts as still

  RadarTarget best;
  float bestScore = 1e9f;
  bool bestMoving = false;
  MapTarget seen[3];
  int seenCount = 0, bestSeenIdx = -1;

  for (int i = 0; i < 3; ++i) {
    int o = 4 + i * 8;
    const int scale = webRadarScale();
    int32_t x = (int32_t)decodeRadarSigned(f[o + 0], f[o + 1]) * scale;
    int32_t y = (int32_t)decodeRadarSigned(f[o + 2], f[o + 3]) * scale;
    int16_t v = decodeRadarSigned(f[o + 4], f[o + 5]);
    uint16_t pd = (uint16_t)f[o + 6] | ((uint16_t)f[o + 7] << 8);
    if (x == 0 && y == 0 && v == 0 && pd == 0) continue;
    radarSlotsSeen++;
    const int myIdx = seenCount < 3 ? seenCount++ : 2;
    seen[myIdx].x = (int16_t)x; seen[myIdx].y = (int16_t)y;
    seen[myIdx].speed = v; seen[myIdx].state = 1;
    float range = sqrtf((float)x * x + (float)y * y);
    float angle = atan2f((float)x, (float)y) * 180.0f / PI;
    dbgX = (int16_t)x; dbgY = (int16_t)y; dbgV = v; dbgRange = range; dbgAngle = angle;
    if (range < nearCut) { rejNear++; dbgVerdict = "too near"; seen[myIdx].state = 2; continue; }
    if (range > RADAR_MAX_RANGE_MM) { rejFar++; dbgVerdict = "too far"; seen[myIdx].state = 5; continue; }
    if (fabsf(angle) > RADAR_MAX_ANGLE_DEG) { rejAngle++; dbgVerdict = "too wide"; seen[myIdx].state = 5; continue; }

    const bool moving = abs(v) >= MOVING_CM_S;

    if (learnUntilMs && (int32_t)(now - learnUntilMs) < 0) {
      clutterAdd(x, y);               // learning: whatever is visible is furniture
      continue;
    }
    if (clutterHas(x, y) && abs(v) < CLUTTER_WALK_CM_S) {
      rejClutter++; dbgVerdict = "known fixed echo"; seen[myIdx].state = 3; continue;
    }
    // Near where we last saw the person? Then it is probably still them.
    const float dx = (float)x - lastX, dy = (float)y - lastY;
    const bool continues = (now - lastMovingMs < holdMs) && sqrtf(dx * dx + dy * dy) < 1200.0f;

    if (movingOnly && !moving && !continues) { rejStatic++; dbgVerdict = "not moving"; seen[myIdx].state = 4; continue; }

    // Moving targets always win; otherwise the nearest.
    float score = range - (moving ? 100000.0f : 0.0f) - (continues ? 50000.0f : 0.0f);
    if (score < bestScore) {
      bestScore = score;
      bestSeenIdx = myIdx;
      bestMoving = moving;
      best.valid = true;
      best.x_mm = (int16_t)x; best.y_mm = (int16_t)y; best.speed_cm_s = v;
      best.range_mm = range; best.angle_deg = angle;
    }
  }

  // Anything that sits in one place for half a minute is part of the room.
  static int32_t stillX = 0, stillY = 0;
  static uint32_t stillSinceMs = 0;
  if (best.valid && !bestMoving) {
    const int32_t dx = best.x_mm - stillX, dy = best.y_mm - stillY;
    if (dx * dx + dy * dy > 400 * 400) { stillX = best.x_mm; stillY = best.y_mm; stillSinceMs = now; }
    else if (stillSinceMs && now - stillSinceMs > 30000) {
      clutterAdd(best.x_mm, best.y_mm);
      clutterSave();
      stillSinceMs = 0;
      best.valid = false;             // and stop following it
    }
  } else {
    stillSinceMs = 0;
  }

  if (bestSeenIdx >= 0 && best.valid) seen[bestSeenIdx].state = 0;   // the one being followed
  mapSeenCount = seenCount;
  for (int i = 0; i < seenCount; ++i) mapSeen[i] = seen[i];

  if (best.valid) {
    radarAccepted++;
    dbgVerdict = bestMoving ? "accepted (moving)" : "accepted (still, still watching)";
    if (bestMoving) { lastMovingMs = now; }
    lastX = best.x_mm; lastY = best.y_mm;
    if (millis() - radarLastSeenMs > 1500) radarAngleSmoothed = best.angle_deg;
    radar = best;
    radarLastSeenMs = millis();
  }
}

static void pollRadar() {
  static uint8_t frame[30];
  static int idx = 0;
  while (Serial1.available()) {
    uint8_t b = (uint8_t)Serial1.read();
    radarBytes++;
    if (idx == 0) { if (b == 0xAA) frame[idx++] = b; else radarBadHeader++; continue; }
    if (idx == 1) { if (b == 0xFF) frame[idx++] = b; else { radarBadHeader++; idx = (b == 0xAA) ? 1 : 0; } continue; }
    if (idx == 2) { if (b == 0x03) frame[idx++] = b; else { radarBadHeader++; idx = 0; } continue; }
    if (idx == 3) { if (b == 0x00) frame[idx++] = b; else { radarBadHeader++; idx = 0; } continue; }
    frame[idx++] = b;
    if (idx == 30) {
      if (frame[28] == 0x55 && frame[29] == 0xCC) processRadarFrame(frame);
      else radarBadTail++;
      idx = 0;
    }
  }
}

static bool radarHasTarget() { return radarLastSeenMs != 0 && millis() - radarLastSeenMs < 600; }

// =====================================================================
//                          EYE STATE / BEHAVIOR
// =====================================================================
// Everything the renderer reads each frame. Behaviors set *targets*; the
// smoothing step moves the actual values toward them.
struct Look {
  float gx = 0, gy = 0;          // gaze, -1..1 (x: +right, y: +down)
  float pupil = 1.0f;            // 1 = normal slit, 0.35 = razor thin, 1.3 = wide
  float glow = 1.0f;             // glow multiplier around the pupil
  float squintL = 0, squintR = 0;
  float droop = 0;               // sleepy lids
  float wide = 0;                // startled lids
  float verge = 0;               // pixels each eye turns inward (near target)
};
Look cur, tgt;

// ---- Gaze motion ----
// GLIDE: a deliberate eased move from A to B over a set time (idle behaviors).
// FOLLOW: smooth continuous chase of a moving target (scan, roll, radar).
enum GazeMode : uint8_t { GLIDE, FOLLOW };
GazeMode gazeMode = GLIDE;
float glideFromX = 0, glideFromY = 0, glideToX = 0, glideToY = 0;
uint32_t glideStartMs = 0, glideDurMs = 1;
float followTau = 0.3f;

// Glide to a spot. Bigger moves take longer, like a slow deliberate turn.
static void aimAt(float x, float y, float speedMul = 1.0f) {
  x = clampf(x, -1.1f, 1.1f);
  y = clampf(y, -1.1f, 1.1f);
  if (gazeMode == GLIDE && fabsf(x - glideToX) < 0.01f && fabsf(y - glideToY) < 0.01f) return;
  float dist = sqrtf((x - cur.gx) * (x - cur.gx) + (y - cur.gy) * (y - cur.gy));
  gazeMode = GLIDE;
  glideFromX = cur.gx; glideFromY = cur.gy;
  glideToX = x; glideToY = y;
  glideStartMs = millis();
  glideDurMs = (uint32_t)((GLIDE_BASE_MS + GLIDE_PER_UNIT_MS * dist) / (MOVE_SPEED * speedMul));
  if (glideDurMs < 1) glideDurMs = 1;
}

// Continuously chase (x, y) with time constant tau seconds.
static void followAt(float x, float y, float tau) {
  gazeMode = FOLLOW;
  tgt.gx = clampf(x, -1.1f, 1.1f);
  tgt.gy = clampf(y, -1.1f, 1.1f);
  followTau = tau / MOVE_SPEED;
  glideToX = 99;   // forces the next aimAt() to start a fresh glide
}

float pupilTau = 0.4f;
float squintTau = SQUINT_TAU_S;

enum Mood : uint8_t { WANDER, SIDE_EYE, SCAN, GLARE, SQUINT, SLEEPY, STARE, ROLL, DOUBLE_TAKE,
                      TRACK, LOST, MOOD_COUNT };
const char *MOOD_NAMES[] = { "wander", "side-eye", "scan", "glare", "squint", "sleepy", "stare",
                             "eye-roll", "double-take", "TRACKING", "lost target" };
Mood mood = WANDER;
uint32_t moodStartMs = 0, moodEndMs = 0;
uint32_t nextMoveMs = 0;
int moodStep = 0;
float moodSide = 1;
float moodX = 0, moodY = 0;
bool moodOneEye = false;

// Blinks run independently of moods.
bool blinking = false;
uint32_t blinkStartMs = 0, nextBlinkMs = 0;
uint16_t blinkCloseMs = 150, blinkHoldMs = 70, blinkOpenMs = 260;
bool doubleBlinkQueued = false;

// Startle overlay (pupil snap + wide lids), triggered by several behaviors.
uint32_t startleUntilMs = 0;

// Set from the chosen art style: the skeleton styles get a colder, slower feel
// (steady glow instead of candle flicker, and more staring than antics).
bool coldFlavor = false;

// Sleep mode: 0 = eyes open, 1 = eyes shut. Rides on top of everything else.
float sleepShut = 0.0f;
bool wasAsleep = false;

// Candle-like flicker of the pupil glow.
float flicker = 1.0f, flickerTarget = 1.0f;
uint32_t nextFlickerMs = 0;

// Radar "they stopped moving" timer, for the stare-down squint.
uint32_t trackStillSinceMs = 0;

static uint32_t nextBlinkDelay() {
  return randMs((uint32_t)(BLINK_EVERY_MIN_S * 1000), (uint32_t)(BLINK_EVERY_MAX_S * 1000));
}

static void setMood(Mood m, uint32_t durMs) {
  mood = m;
  moodStartMs = millis();
  moodEndMs = moodStartMs + durMs;
  moodStep = 0;
  nextMoveMs = 0;
  moodSide = chance(0.5f) ? 1.0f : -1.0f;
  moodX = randf(-0.35f, 0.35f);
  moodY = randf(-0.1f, 0.15f);
  moodOneEye = chance(0.3f);
  Serial.printf("[mood] %s\n", MOOD_NAMES[m]);
}

static void startBlink(bool slow) {
  blinking = true;
  blinkStartMs = millis();
  float k = 1.0f / BLINK_SPEED;
  blinkCloseMs = (uint16_t)((slow ? 350 : 150) * k);
  blinkHoldMs  = (uint16_t)((slow ? 260 : 70) * k);
  blinkOpenMs  = (uint16_t)((slow ? 650 : 260) * k);
}

static float blinkAmount() {
  if (!blinking) return 0;
  uint32_t e = millis() - blinkStartMs;
  if (e < blinkCloseMs) return smooth01((float)e / blinkCloseMs);
  e -= blinkCloseMs;
  if (e < blinkHoldMs) return 1;
  e -= blinkHoldMs;
  if (e < blinkOpenMs) return 1 - smooth01((float)e / blinkOpenMs);
  blinking = false;
  if (doubleBlinkQueued) {
    doubleBlinkQueued = false;
    nextBlinkMs = millis() + 180;
  } else {
    nextBlinkMs = millis() + (mood == SLEEPY ? randMs(2500, 4500) : nextBlinkDelay());
  }
  return 0;
}

static void startle() {
  startleUntilMs = millis() + 900;
}

// Pick a random idle behavior. WANDER is the most common.
static void pickIdleMood() {
  struct Entry { Mood m; uint8_t w; uint32_t lo, hi; };
  static const Entry warm[] = {
    { WANDER, 34, 8000, 16000 }, { SIDE_EYE, 9, 4500, 7000 }, { SCAN, 7, 6000, 6000 },
    { GLARE, 8, 4000, 6500 },    { SQUINT, 14, 5000, 8000 },  { SLEEPY, 6, 9000, 13000 },
    { STARE, 10, 5000, 8000 },   { ROLL, 5, 3200, 3200 },     { DOUBLE_TAKE, 7, 5000, 5000 },
  };
  // Colder, stiller, more unsettling: long stares, slow scans, no eye-rolls.
  static const Entry cold[] = {
    { WANDER, 26, 10000, 18000 }, { SIDE_EYE, 11, 5000, 8000 }, { SCAN, 12, 7000, 7000 },
    { GLARE, 12, 5000, 8000 },    { SQUINT, 16, 6000, 9000 },   { SLEEPY, 3, 9000, 13000 },
    { STARE, 20, 7000, 12000 },
  };
  const Entry *table = coldFlavor ? cold : warm;
  const int count = coldFlavor ? (int)(sizeof(cold) / sizeof(cold[0])) : (int)(sizeof(warm) / sizeof(warm[0]));
  int total = 0;
  for (int i = 0; i < count; ++i) total += table[i].w;
  int r = esp_random() % total;
  for (int i = 0; i < count; ++i) {
    const Entry &t = table[i];
    if ((r -= t.w) < 0) {
      if (t.m == mood && t.m != WANDER) { setMood(WANDER, randMs(6000, 12000)); return; }
      setMood(t.m, randMs(t.lo, t.hi));
      return;
    }
  }
}

// Idle behaviors: each sets targets for gaze, pupil and lids.
static void runIdleMood(uint32_t now) {
  const float t = (float)(now - moodStartMs) / 1000.0f;       // seconds into mood
  const float len = (float)(moodEndMs - moodStartMs) / 1000.0f;
  const float left = len - t;                                  // seconds remaining

  // Defaults, which moods then override.
  tgt.pupil = 1.0f + 0.06f * sinf(now * 0.0012f);            // slow "breathing"
  tgt.squintL = tgt.squintR = 0;
  tgt.droop = 0;
  tgt.wide = 0;
  tgt.glow = 1.0f;
  tgt.verge = 0;
  pupilTau = 0.4f;
  squintTau = SQUINT_TAU_S;

  switch (mood) {
    case WANDER:
      // Slow deliberate look to a spot, then a long intense hold.
      if (now >= nextMoveMs) {
        float x = randf(-0.85f, 0.85f), y = randf(-0.4f, 0.3f);
        if (chance(0.3f)) { x *= 0.15f; y *= 0.15f; }            // back to dead centre
        aimAt(x, y);
        nextMoveMs = now + glideDurMs + randMs(2200, 5500);
      }
      break;

    case SIDE_EYE:
      // Suspicious look to one side, one eye narrowed more than the other.
      aimAt(0.95f * moodSide, 0.05f);
      tgt.squintL = moodSide > 0 ? 0.5f : 0.85f;
      tgt.squintR = moodSide > 0 ? 0.85f : 0.5f;
      tgt.pupil = 0.7f;
      if (left < 1.2f) tgt.squintL = tgt.squintR = 0;           // relax before leaving
      break;

    case SCAN:
      // Slow, smooth sweep from one side to the other, like searching.
      followAt(moodSide * (-0.9f + 1.8f * smooth01(t / len)), -0.05f, 0.35f);
      tgt.squintL = tgt.squintR = 0.3f;
      break;

    case GLARE:
      // Dead ahead, heavy squint, razor pupil, fire flares up.
      aimAt(0, 0.08f);
      tgt.squintL = tgt.squintR = 0.95f;
      tgt.pupil = 0.4f;
      tgt.glow = 1.5f;
      if (left < 1.2f) { tgt.squintL = tgt.squintR = 0; tgt.pupil = 1.0f; tgt.glow = 1.0f; }
      break;

    case SQUINT:
      // Slow narrowing of the eyes while locked on one spot. Sometimes one eye
      // narrows more than the other (skeptical). Releases slowly at the end.
      aimAt(moodX, moodY);
      tgt.squintL = tgt.squintR = 0.8f;
      if (moodOneEye) { if (moodSide > 0) tgt.squintL = 0.45f; else tgt.squintR = 0.45f; }
      tgt.pupil = 0.55f;
      tgt.glow = 1.3f;
      pupilTau = 0.9f;
      squintTau = SQUINT_TAU_S * 1.6f;                            // extra slow squeeze
      if (left < 1.8f) { tgt.squintL = tgt.squintR = 0; tgt.pupil = 1.0f; tgt.glow = 1.0f; }
      nextBlinkMs = max(nextBlinkMs, now + 500);                  // don't blink mid-squint
      break;

    case SLEEPY: {
      // Lids sink, eyes drift down, slow blinks... then snap awake.
      float wake = len - 2.0f;
      if (t < wake) {
        tgt.droop = 0.75f * smooth01(t / 3.5f);
        followAt(0.3f * sinf(t * 0.4f), 0.45f, 0.8f);
        tgt.pupil = 1.25f;
        tgt.glow = 0.7f;
        if (!blinking && now >= nextBlinkMs) startBlink(true);
      } else if (moodStep == 0) {
        startle();
        aimAt(randf(-0.6f, 0.6f), -0.15f, 2.5f);                 // wakes with a quicker look
        moodStep = 1;
      }
      break;
    }

    case STARE:
      // Unblinking dead-centre stare while the pupil slowly narrows.
      aimAt(0, 0);
      tgt.wide = 0.5f;
      tgt.pupil = 1.1f - 0.6f * smooth01(t / len);
      tgt.glow = 1.0f + 0.4f * smooth01(t / len);
      pupilTau = 0.8f;
      nextBlinkMs = max(nextBlinkMs, now + 500);                  // hold the stare
      break;

    case ROLL: {
      // Slow eye roll: side -> up -> other side, then settle.
      float p = clampf(t / 2.2f, 0, 1);
      float a = PI * smooth01(p);
      if (p < 1) followAt(moodSide * cosf(a) * 0.9f, -sinf(a), 0.18f);
      else aimAt(-moodSide * 0.3f, 0.1f);
      tgt.squintL = tgt.squintR = p >= 1 ? 0.4f : 0;
      break;
    }

    case DOUBLE_TAKE:
      // Glance to the side, back to centre, then snap back with a startle.
      if (t < 1.8f)      aimAt(0.8f * moodSide, 0);
      else if (t < 3.0f) aimAt(0, 0);
      else {
        if (moodStep == 0) { startle(); moodStep = 1; }
        aimAt(0.95f * moodSide, -0.05f, 2.5f);                    // the "snap" is quicker
      }
      break;

    default: break;
  }

  if (now >= moodEndMs) pickIdleMood();
}

// Radar tracking behavior.
static void runTracking(float dt, uint32_t now) {
  float angle = webRadarInvert() ? -radar.angle_deg : radar.angle_deg;

  // Response 1..10 sets how much smoothing sits between the radar and the eyes.
  // At 10 there is almost none: the eyes go where the radar says, immediately.
  const float r = (float)webTrackResponse();
  const float angleTau = 0.30f - 0.028f * (r - 1.0f);      // 0.30 s .. 0.05 s
  const float chaseFast = 0.20f - 0.018f * (r - 1.0f);     // 0.20 s .. 0.04 s
  const float chaseSlow = 0.38f - 0.032f * (r - 1.0f);     // 0.38 s .. 0.09 s
  radarAngleSmoothed += (angle - radarAngleSmoothed) * (1.0f - expf(-dt / angleTau));

  float near = 1.0f - clampf((radar.range_mm - 500.0f) / 3500.0f, 0, 1);  // 1 = very close
  float want = clampf(radarAngleSmoothed / (float)webRadarFullGazeDeg(), -1, 1);
  // Big change = firmer turn; small change = slow smooth follow.
  followAt(want, 0.15f * near, fabsf(want - cur.gx) > 0.3f ? chaseFast : chaseSlow);
  tgt.verge = 4.0f * near;                         // go a little cross-eyed when close

  float speed = fabsf((float)radar.speed_cm_s);
  tgt.pupil = 0.6f - 0.2f * clampf(speed / 100.0f, 0, 1);    // focused; thinner when they move
  tgt.glow = 1.15f + 0.35f * near;
  tgt.droop = 0;
  tgt.wide = 0;
  pupilTau = 0.3f;
  squintTau = SQUINT_TAU_S;

  // Stand still in front of it and it slowly narrows its eyes at you.
  if (speed > 15) trackStillSinceMs = now;
  bool staredown = now - trackStillSinceMs > 2500;
  bool tooClose = radar.range_mm < RADAR_CLOSE_MM;
  float sq = 0.2f;
  if (staredown) sq = 0.75f;
  if (tooClose) sq = 0.9f;
  tgt.squintL = tgt.squintR = sq;
  if (staredown || tooClose) { tgt.pupil = 0.4f; tgt.glow = 1.5f; }
}

static void updateBehavior(float dt) {
  const uint32_t now = millis();
  const bool target = radarHasTarget();

  static uint32_t lastTrackEndMs = 0;
  if (target && mood != TRACK) {
    // Only jump in surprise if nobody has been around for a few seconds.
    if (lastTrackEndMs == 0 || now - lastTrackEndMs > 3000) startle();
    setMood(TRACK, 0);
    trackStillSinceMs = now;
    Serial.printf("[radar] target at %.0f mm, %.0f deg\n", radar.range_mm, radar.angle_deg);
  } else if (!target && mood == TRACK) {
    lastTrackEndMs = now;
    setMood(LOST, 2000);
  }

  if (mood == TRACK) {
    runTracking(dt, now);
  } else if (mood == LOST) {
    // Keep staring where they vanished, then go back to idle with a search sweep.
    tgt.pupil = 0.8f; tgt.verge = 0; tgt.squintL = tgt.squintR = 0.35f;
    if (now >= moodEndMs) setMood(SCAN, 6000);
  } else {
    runIdleMood(now);
  }

  // Blinks
  if (!blinking && now >= nextBlinkMs && mood != SLEEPY) {
    startBlink(false);
    doubleBlinkQueued = chance(0.07f);
  }

  // Startle overlay beats everything else for a moment.
  float pupilWant = tgt.pupil, wideWant = tgt.wide, pTau = pupilTau, wTau = 0.35f;
  if (now < startleUntilMs) { pupilWant = 0.35f; wideWant = 1.0f; pTau = 0.06f; wTau = 0.12f; }

  // Fire flicker: new random level every 60-160 ms, smoothed.
  if (now >= nextFlickerMs) {
    if (coldFlavor) {
      // Slow cold pulse, with the occasional dim like a guttering spirit.
      flickerTarget = chance(0.10f) ? randf(0.55f, 0.72f) : randf(0.88f, 1.06f);
      nextFlickerMs = now + randMs(280, 900);
    } else {
      flickerTarget = chance(0.08f) ? randf(0.55f, 0.75f) : randf(0.85f, 1.2f);
      nextFlickerMs = now + randMs(60, 160);
    }
  }
  flicker = approach(flicker, flickerTarget, coldFlavor ? 0.35f : 0.07f, dt);

  // Gaze
  if (gazeMode == GLIDE) {
    float p = clampf((float)(now - glideStartMs) / (float)glideDurMs, 0, 1);
    float e = p * p * p * (p * (p * 6 - 15) + 10);               // smootherstep ease in/out
    cur.gx = glideFromX + (glideToX - glideFromX) * e;
    cur.gy = glideFromY + (glideToY - glideFromY) * e;
  } else {
    cur.gx = approach(cur.gx, tgt.gx, followTau, dt);
    cur.gy = approach(cur.gy, tgt.gy, followTau, dt);
  }

  // Everything else eases toward its target.
  cur.pupil = approach(cur.pupil, pupilWant, pTau, dt);
  cur.glow = approach(cur.glow, tgt.glow, 0.35f, dt);
  cur.squintL = approach(cur.squintL, tgt.squintL, squintTau, dt);
  cur.squintR = approach(cur.squintR, tgt.squintR, squintTau, dt);
  cur.droop = approach(cur.droop, tgt.droop, 0.6f, dt);
  cur.wide = approach(cur.wide, wideWant, wTau, dt);
  cur.verge = approach(cur.verge, tgt.verge, 0.4f, dt);

  // Sleep until someone moves: lids close, and snap open when the radar finds
  // a person. Waking startles, so the eyes open wide and lock straight on.
  if (webSleepUntilMoved() && !webTestSweep()) {
    const bool awake = radarHasTarget() ||
                       (radarLastSeenMs && now - radarLastSeenMs < (uint32_t)webWakeHoldSecs() * 1000);
    if (awake && wasAsleep) {
      wasAsleep = false;
      startle();
      setMood(TRACK, 0);
      Serial.println("[sleep] woke up - someone is there");
    }
    if (!awake && !wasAsleep && sleepShut > 0.9f) {
      wasAsleep = true;
      Serial.println("[sleep] nobody about - eyes closed");
    }
    sleepShut = approach(sleepShut, awake ? 0.0f : 1.0f, awake ? 0.10f : 0.7f, dt);
    if (!awake) {                     // drift back to centre while dozing off
      cur.gx = approach(cur.gx, 0.0f, 0.8f, dt);
      cur.gy = approach(cur.gy, 0.1f, 0.8f, dt);
    }
  } else if (sleepShut > 0.0f) {
    sleepShut = approach(sleepShut, 0.0f, 0.2f, dt);
  }

  // Test sweep from the settings page: full travel, side to side, nothing else.
  if (webTestSweep()) {
    cur.gx = sinf(now * 0.0016f);
    cur.gy = 0; cur.squintL = cur.squintR = 0; cur.droop = 0; cur.wide = 0;
    cur.pupil = 1.0f; cur.verge = 0;
  }
}

// =====================================================================
//                              RENDERING
// =====================================================================
// Two ways to feed the displays:
//   DMA (normal): the SPI hardware sends one strip in the background while the
//     CPU is already building the next, so drawing and sending overlap.
//   Fallback: the Adafruit library sends each strip while the CPU waits. Used
//     automatically if the DMA setup fails at start-up (Serial says which).
static constexpr int STRIP_ROWS = 24;        // 11.5 KB a buffer, leaving internal RAM for art
static constexpr int STRIP_PIXELS = eye::W * STRIP_ROWS;
static eye::Frame frameL, frameR;

uint32_t statComposeUs = 0, statWaitUs = 0, statTotalUs = 0;

#if defined(ESP_PLATFORM)
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#define HAVE_DMA_PATH 1
#else
#define HAVE_DMA_PATH 0
#endif

bool useDma = false;
static uint16_t *stripBuf[2] = { nullptr, nullptr };
static uint16_t *fallbackStrip = nullptr;    // only allocated if DMA isn't used

#if HAVE_DMA_PATH
static esp_lcd_panel_io_handle_t ioLeft = nullptr, ioRight = nullptr;
static SemaphoreHandle_t stripFree = nullptr;   // counts strip buffers not in flight

static bool IRAM_ATTR onStripSent(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  BaseType_t woke = pdFALSE;
  xSemaphoreGiveFromISR(stripFree, &woke);
  return woke == pdTRUE;
}

static bool initDmaPath() {
  stripBuf[0] = (uint16_t *)heap_caps_malloc(STRIP_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  stripBuf[1] = (uint16_t *)heap_caps_malloc(STRIP_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  stripFree = xSemaphoreCreateCounting(2, 2);
  if (!stripBuf[0] || !stripBuf[1] || !stripFree) {
    Serial.println("[display] DMA: out of memory");
    return false;
  }

  // Hand the SPI hardware over from the Arduino library to the DMA driver.
  SPI.end();

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_TFT_MOSI;
  bus.miso_io_num = -1;
  bus.sclk_io_num = PIN_TFT_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = STRIP_PIXELS * 2 + 16;
  esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    Serial.printf("[display] DMA: spi_bus_initialize failed (%s)\n", esp_err_to_name(err));
    return false;
  }

  esp_lcd_panel_io_spi_config_t io = {};
  io.dc_gpio_num = PIN_TFT_DC;
  io.spi_mode = 0;
  io.pclk_hz = SPI_HZ;
  io.trans_queue_depth = 4;
  io.on_color_trans_done = onStripSent;
  io.lcd_cmd_bits = 8;
  io.lcd_param_bits = 8;

  io.cs_gpio_num = PIN_TFT_CS_LEFT;
  err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &ioLeft);
  if (err == ESP_OK) {
    io.cs_gpio_num = PIN_TFT_CS_RIGHT;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &ioRight);
  }
  if (err != ESP_OK) {
    Serial.printf("[display] DMA: panel IO failed (%s)\n", esp_err_to_name(err));
    if (ioLeft) { esp_lcd_panel_io_del(ioLeft); ioLeft = nullptr; }
    spi_bus_free(SPI2_HOST);
    return false;
  }
  return true;
}

static void drawEyeDma(esp_lcd_panel_io_handle_t io, const eye::Frame *f) {
  // Full-screen window, then stream the strips as one continuous memory write.
  // f == nullptr draws solid black.
  // NOT const: from flash the SPI driver would allocate a temporary DMA buffer
  // on every call - twice per eye, every frame - and eventually run out.
  static uint8_t win[4] = { 0, 0, 0, (uint8_t)(eye::W - 1) };
  esp_lcd_panel_io_tx_param(io, 0x2A, win, 4);   // column range 0..239
  esp_lcd_panel_io_tx_param(io, 0x2B, win, 4);   // row range 0..239

  for (int s = 0; s < eye::H / STRIP_ROWS; ++s) {
    uint32_t t0 = micros();
    xSemaphoreTake(stripFree, portMAX_DELAY);    // wait for a free strip buffer
    uint32_t t1 = micros();
    uint16_t *buf = stripBuf[s & 1];
    if (f) eye::composeRows(*f, s * STRIP_ROWS, STRIP_ROWS, buf, true);
    else memset(buf, 0, STRIP_PIXELS * 2);
    statWaitUs += t1 - t0;
    statComposeUs += micros() - t1;
    esp_lcd_panel_io_tx_color(io, s == 0 ? 0x2C : -1, buf, STRIP_PIXELS * 2);
  }
  // Let this eye finish before the other display gets the bus.
  uint32_t t0 = micros();
  xSemaphoreTake(stripFree, portMAX_DELAY);
  xSemaphoreTake(stripFree, portMAX_DELAY);
  xSemaphoreGive(stripFree);
  xSemaphoreGive(stripFree);
  statWaitUs += micros() - t0;
}
#endif

static void drawEyeBlocking(Adafruit_GC9A01A &tft, const eye::Frame *f) {
  if (!fallbackStrip) fallbackStrip = (uint16_t *)malloc(STRIP_PIXELS * 2);
  if (!fallbackStrip) return;
  tft.startWrite();
  tft.setAddrWindow(0, 0, eye::W, eye::H);
  for (int y = 0; y < eye::H; y += STRIP_ROWS) {
    uint32_t t0 = micros();
    if (f) eye::composeRows(*f, y, STRIP_ROWS, fallbackStrip, false);
    else memset(fallbackStrip, 0, STRIP_PIXELS * 2);
    uint32_t t1 = micros();
    tft.writePixels(fallbackStrip, STRIP_PIXELS, true, false);
    statComposeUs += t1 - t0;
    statWaitUs += micros() - t1;
  }
  tft.endWrite();
}

static void drawEye(bool isLeft, const eye::Frame *f) {
#if HAVE_DMA_PATH
  if (useDma) { drawEyeDma(isLeft ? ioLeft : ioRight, f); return; }
#endif
  drawEyeBlocking(isLeft ? leftTft : rightTft, f);
}

// Send a no-parameter command (sleep/wake/display on/off) to both displays.
static void displayCommand(uint8_t cmd) {
#if HAVE_DMA_PATH
  if (useDma) {
    esp_lcd_panel_io_tx_param(ioLeft, cmd, nullptr, 0);
    esp_lcd_panel_io_tx_param(ioRight, cmd, nullptr, 0);
    return;
  }
#endif
  leftTft.sendCommand(cmd);
  rightTft.sendCommand(cmd);
}

float glowScale = 1.0f;
uint32_t appliedLookVersion = 0;

// Re-tint the artwork from the colour sliders on the settings page.
static void applyLookSettings() {
  int bright, pale, warm, glow;
  webGetColorTrim(bright, pale, warm, glow);
  glowScale = glow / 100.0f;
  uint32_t t0 = millis();
  eye::applyColorTrim(bright * 256 / 100, pale * 256 / 100, warm * 256 / 100);
  appliedLookVersion = webLookVersion();
  Serial.printf("[look] colour: %d%% bright, %d%% pale, warm %d, glow %d%% (%lu ms)\n",
                bright, pale, warm, glow, (unsigned long)(millis() - t0));
}

static void buildFrame(eye::Frame &f, bool isLeft, float blink) {
  // Left display sits on the viewer's left; "inward" for it is +x.
  float verge = isLeft ? cur.verge : -cur.verge;
  const float travel = (float)webGazeTravelPx();
  f.dx = (int16_t)lroundf(clampf(cur.gx, -1.2f, 1.2f) * travel + verge);
  f.dy = (int16_t)lroundf(clampf(cur.gy, -1.2f, 1.2f) * GAZE_RANGE_Y_PX);
  f.pupilHalfW16 = (int32_t)(eye::pupilHalfWPx() * 16.0f * clampf(cur.pupil, 0.2f, 1.5f));
  f.pupilHalfH16 = (int32_t)(eye::pupilHalfHPx() * 16.0f);
  // 0 = follow the style's own shape, 1 = slit, 2 = round.
  const int shape = webPupilShape();
  f.pupilRound = (shape == 0) ? eye::styleWantsRoundPupil() : (shape == 2);
  f.pupilR16 = (int32_t)(eye::pupilRoundRPx() * 16.0f * clampf(cur.pupil, 0.25f, 1.6f));
  f.glow256 = (uint16_t)clampf(256.0f * cur.glow * flicker * glowScale, 0, 900);
  eye::buildGlow(f);
  f.lidArt = LID_ARTWORK && webLidSet() != 100;
  // Foreshortening, tilt and stretched tissue use the slower drawing path, and
  // they need the iris art in RAM to be worth it at all.
  const bool fancy = eye::irisInRam() && !eye::artInPsram() && webFancyTurn();
  int lx, ly, rx, ry;
  webGetEyeNudge(lx, ly, rx, ry);
  f.offX = (int16_t)(isLeft ? lx : rx);
  f.offY = (int16_t)(isLeft ? ly : ry);

  // Screen tilt, for an eye glued in crooked.
  float lrot, rrot;
  webGetEyeRotation(lrot, rrot);
  const float rot = fancy ? (isLeft ? lrot : rrot) * 0.01745329f : 0.0f;
  const float cosT = cosf(rot), sinT = sinf(rot);
  f.cos16 = (int32_t)lroundf(cosT * 65536.0f);
  f.sin16 = (int32_t)lroundf(sinT * 65536.0f);

  // Turned far to a side: the iris foreshortens and the tissue behind it drags.
  // Both need the iris art in RAM; from flash they would cost ~10x the drawing
  // time, so they are skipped rather than slowing everything to a crawl.
  const float agx = fancy ? clampf(fabsf(cur.gx), 0, 1) : 0.0f;
  f.squash16 = (int32_t)lroundf((1.0f - GAZE_SQUASH * agx) * 65536.0f);
  f.pull256 = (uint16_t)lroundf(255.0f * TISSUE_PULL * agx);
  float px = cur.gx, py = cur.gy * 0.6f;
  float plen = sqrtf(px * px + py * py);
  if (plen < 0.001f) plen = 1.0f;
  f.pullX = (int16_t)lroundf(256.0f * px / plen);
  f.pullY = (int16_t)lroundf(256.0f * py / plen);

  eye::LidShape s;
  s.blink = blink;
  s.squint = isLeft ? cur.squintL : cur.squintR;
  s.droop = cur.droop;
  s.wide = cur.wide;
  s.angry = BROW_ANGER;
  s.mirror = isLeft ? SWAP_BROW_SLANT : !SWAP_BROW_SLANT;
  s.offX = f.offX;
  s.offY = f.offY;
  s.cosT = cosT;
  s.sinT = sinT;
  eye::buildLids(f, s);
}

// =====================================================================
//                     ON / OFF (schedule) HANDLING
// =====================================================================
bool eyesOn = true;
bool displaysPowered = true;

static void powerSwitch(bool on) {
  if (!POWER_SWITCH_INSTALLED) return;
  pinMode(POWER_SWITCH_PIN, OUTPUT);
  digitalWrite(POWER_SWITCH_PIN, on == POWER_SWITCH_ON_HIGH ? HIGH : LOW);
}

// Stop driving the display wires so nothing leaks into unpowered screens.
static void releaseDisplayPins() {
#if HAVE_DMA_PATH
  if (useDma) {
    esp_lcd_panel_io_del(ioLeft);
    esp_lcd_panel_io_del(ioRight);
    ioLeft = ioRight = nullptr;
    spi_bus_free(SPI2_HOST);
  } else
#endif
  {
    SPI.end();
  }
  const int pins[] = { PIN_TFT_SCK, PIN_TFT_MOSI, PIN_TFT_DC, PIN_TFT_CS_LEFT, PIN_TFT_CS_RIGHT, PIN_TFT_RST };
  for (int p : pins) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  Serial1.end();
  pinMode(PIN_RADAR_RX, INPUT);
}

static void eyesGoToSleep() {
  drawEye(true, nullptr);                  // black first, so nothing lingers
  drawEye(false, nullptr);
  if (POWER_SWITCH_INSTALLED) {
    releaseDisplayPins();
    powerSwitch(false);
    displaysPowered = false;
    Serial.println("[power] off-time: screens and radar switched off");
  } else {
    displayCommand(0x28);                  // display off
    displayCommand(0x10);                  // sleep in
    Serial.println("[power] off-time: screens black (no power switch installed)");
  }
  setCpuFrequencyMhz(80);                  // slowest speed Wi-Fi allows; saves a little
  eyesOn = false;
}

static void eyesWakeUp() {
  if (!displaysPowered) {
    // Screens were unpowered and have lost their set-up. The simplest reliable
    // way back is a clean restart, which powers them and sets them up again.
    Serial.println("[power] on-time: restarting to power the screens back up");
    delay(100);
    ESP.restart();
  }
  setCpuFrequencyMhz(160);
  displayCommand(0x11);                    // sleep out
  delay(120);
  displayCommand(0x29);                    // display on
  eyesOn = true;
  setMood(WANDER, randMs(6000, 12000));
  startBlink(true);                        // wakes with a slow blink
  Serial.println("[power] on-time: eyes awake");
}

// Times each drawing path on this actual board, so we can see where the
// milliseconds go instead of guessing. Draws into the strip buffer only;
// nothing is sent to the displays.
static void benchmarkDrawing() {
  uint16_t *buf = stripBuf[0] ? stripBuf[0] : (uint16_t *)malloc(STRIP_PIXELS * 2);
  if (!buf) return;

  auto run = [&](const char *name, eye::Frame &f) {
    eye::buildGlow(f);
    eye::LidShape l; l.angry = BROW_ANGER; l.cosT = 1; l.sinT = 0;
    eye::buildLids(f, l);
    uint32_t t0 = micros();
    for (int rep = 0; rep < 3; ++rep)
      for (int y = 0; y < eye::H; y += STRIP_ROWS)
        eye::composeRows(f, y, STRIP_ROWS, buf, true);
    uint32_t us = (micros() - t0) / 3;
    Serial.printf("[bench] %-26s %5.1f ms per eye\n", name, us / 1000.0f);
  };

  // One frame reused for every test: each one carries per-column lid tables, so
  // five of them would sit on 11 KB of RAM for the whole run.
  static eye::Frame f;
  f = eye::Frame();
  f.pupilHalfW16 = (int32_t)(eye::pupilHalfWPx() * 16.0f);
  f.pupilHalfH16 = (int32_t)(eye::pupilHalfHPx() * 16.0f);
  f.pupilRound = eye::styleWantsRoundPupil();
  f.pupilR16 = (int32_t)(eye::pupilRoundRPx() * 16.0f);
  f.glow256 = 300;
  Serial.printf("[bench] art: %u KB internal, %u KB PSRAM%s\n",
                (unsigned)(eye::artInternalBytes() / 1024), (unsigned)(eye::artPsramBytes() / 1024),
                eye::artInPsram() ? " - PSRAM is slow for this; the slanted path stays off" : "");
  run("eyes centred", f);

  f.dx = 40;
  run("looking aside (plain)", f);

  f.squash16 = (int32_t)(0.68f * 65536);
  run("+ foreshortening", f);

  f.pull256 = 217; f.pullX = 256; f.pullY = 0;
  run("+ stretched tissue", f);

  f.cos16 = (int32_t)(0.978f * 65536); f.sin16 = (int32_t)(0.208f * 65536);
  run("+ 12 deg tilt", f);

  if (buf != stripBuf[0]) free(buf);
}

// =====================================================================
//                            SETUP / LOOP
// =====================================================================
uint32_t lastLoopUs = 0, fpsFrames = 0, fpsStartMs = 0;
uint32_t ramArtKB = 0;

void skullSetup() {
  // Screens and radar get power first, so they're ready for set-up below.
  powerSwitch(true);

  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(20);  // during set-up, let the diagnostics actually get out
#endif
  delay(300);

  // Saved settings first: the display clock comes from them.
  const int style = webStyleFromFlash();
  SPI_HZ = (uint32_t)webSpiMhz() * 1000000UL;

  pinMode(PIN_TFT_CS_LEFT, OUTPUT);
  pinMode(PIN_TFT_CS_RIGHT, OUTPUT);
  digitalWrite(PIN_TFT_CS_LEFT, HIGH);
  digitalWrite(PIN_TFT_CS_RIGHT, HIGH);

  // Shared hardware reset of both displays.
  pinMode(PIN_TFT_RST, OUTPUT);
  digitalWrite(PIN_TFT_RST, HIGH); delay(10);
  digitalWrite(PIN_TFT_RST, LOW);  delay(20);
  digitalWrite(PIN_TFT_RST, HIGH); delay(150);

  // The Adafruit library does the display set-up (init sequence, rotation,
  // color inversion), which is proven on this hardware.
  SPI.begin(PIN_TFT_SCK, -1, PIN_TFT_MOSI, -1);
  leftTft.begin(SPI_HZ);
  rightTft.begin(SPI_HZ);
  leftTft.setRotation(LEFT_ROTATION);
  rightTft.setRotation(RIGHT_ROTATION);
  leftTft.invertDisplay(DISPLAY_INVERT);
  rightTft.invertDisplay(DISPLAY_INVERT);
  leftTft.fillScreen(0);
  rightTft.fillScreen(0);

  // Artwork first: it needs far more room than anything else, and a fragmented
  // heap is what starved it before.
  coldFlavor = (style != 0);
#if defined(ESP_PLATFORM)
  Serial.printf("[display] before art: %u KB free, largest block %u KB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
#endif
  // Leave 140 KB free. Wi-Fi, the web page and the display driver need roughly
  // 90 KB between them, and running them close to empty is what made the eyes
  // flicker out. The iris usually stays in flash as a result, which costs
  // little on the plain drawing path because each row is read straight through.
  // Internal RAM is what makes drawing fast; PSRAM is a poor substitute for
  // this kind of scattered reading. Leave only what Wi-Fi and the display
  // driver really need.
  const int lidSet = webLidSet();
  ramArtKB = eye::useRamCopies(style, lidSet == 100 ? -1 : lidSet, 64 * 1024) / 1024;

  // Then the DMA driver takes over sending the frames.
#if HAVE_DMA_PATH
  if (USE_DMA) {
    useDma = initDmaPath();
    if (!useDma) {
      SPI.begin(PIN_TFT_SCK, -1, PIN_TFT_MOSI, -1);   // back to the Adafruit path
      Serial.println("[display] Using fallback (non-DMA) drawing.");
    }
  }
#endif

  clutterLoad();
  Serial.printf("[room] %d fixed echoes remembered\n", clutterCount);

  Serial1.setRxBufferSize(1024);
  Serial1.begin(RADAR_BAUD, SERIAL_8N1, PIN_RADAR_RX, PIN_RADAR_TX_NC);

  Serial.println();
  Serial.println("Skull Eyes (Rev AM)");

  // The artwork goes into RAM FIRST, keeping 80 KB free for Wi-Fi and the
  // settings page. Doing it the other way round left the iris - the one piece
  // read for every pixel of every frame - stuck in slow flash.
  static const char *styleNames[ART_STYLE_COUNT];
  for (int i = 0; i < ART_STYLE_COUNT; ++i) styleNames[i] = ART_STYLES[i].name;
  webSetStyleNames(styleNames, ART_STYLE_COUNT);
  static const char *lidNames[ART_LID_SET_COUNT];
  for (int i = 0; i < ART_LID_SET_COUNT; ++i) lidNames[i] = ART_LID_SETS[i].name;
  webSetLidNames(lidNames, ART_LID_SET_COUNT);

  webSetup();               // Wi-Fi and the settings page get what's left
  applyLookSettings();

  uint32_t now = millis();
  nextBlinkMs = now + randMs(1500, 3500);
  setMood(WANDER, randMs(4000, 8000));
  fpsStartMs = now;
  lastLoopUs = micros();

  Serial.printf("[display] %s drawing, art: %u KB internal + %u KB PSRAM of %u KB total,"
                " free internal RAM: %u KB\n",
                useDma ? "DMA" : "fallback", (unsigned)(eye::artInternalBytes() / 1024),
                (unsigned)(eye::artPsramBytes() / 1024), (unsigned)(eye::artBytesTotal() / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  Serial.printf("[look] style: %s\n", ART_STYLES[style].name);
  Serial.printf("[display] clock %u MHz - both eyes take about %u ms to send, so the frame rate "
                "cannot exceed about %u fps\n", (unsigned)(SPI_HZ / 1000000),
                (unsigned)(2 * 240UL * 240 * 16 / (SPI_HZ / 1000)), (unsigned)(SPI_HZ / 1000000 / 2));
  if (!eye::irisInRam())
    Serial.println("[display] WARNING: iris art did not fit in RAM - tilt, foreshortening and "
                   "stretched tissue are off to keep the frame rate up.");
  Serial.printf("[power] screen power switch: %s\n", POWER_SWITCH_INSTALLED ? "installed" : "not installed");
  benchmarkDrawing();
  Serial.println("Idle behaviors running. Radar tracking starts automatically when an RD-03D reports a person.");
  Serial.flush();
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);   // from here on, never let the Serial Monitor stall the eyes
#endif
}

static void reportStats(uint32_t now) {
  float secs = (now - fpsStartMs) / 1000.0f;
  float n = (float)fpsFrames;
  float fps = eyesOn && n > 0 ? n / secs : 0;
  if (eyesOn && n > 0) {
    float total = statTotalUs / 1000.0f / n;
    float comp = statComposeUs / 1000.0f / n;
    float wait = statWaitUs / 1000.0f / n;
    uint32_t rowsPlain, rowsSlanted;
    eye::takeRowCounts(rowsPlain, rowsSlanted);
    Serial.printf("[stats] %.1f fps | per frame: drawing %.1f ms, waiting on SPI %.1f ms, other %.1f ms"
                  " | free RAM %u KB | iris in %s, %s path (%lu plain / %lu slanted rows)"
                  " | radar frames: %lu\n",
                  fps, comp, wait, total - comp - wait,
                  (unsigned)(ESP.getFreeHeap() / 1024),
                  eye::artInPsram() ? "PSRAM (slow)" : (eye::irisInRam() ? "internal RAM" : "flash"),
                  rowsSlanted > rowsPlain ? "SLANTED" : "plain",
                  (unsigned long)rowsPlain, (unsigned long)rowsSlanted,
                  (unsigned long)radarFrames);
    if (ESP.getFreeHeap() < 25 * 1024)
      Serial.println("[warn] RAM is nearly gone - displays and Wi-Fi may misbehave");
  }
  webReportStats(fps, eyesOn ? MOOD_NAMES[mood] : "asleep (off-time)", useDma, ramArtKB, POWER_SWITCH_INSTALLED);
  webReportRadar(radarHasTarget(), radar.range_mm, radar.angle_deg, (unsigned long)radarFrames);
  webReportClutter(clutterCount, learnUntilMs != 0);

  if (RADAR_DEBUG) {
    Serial.printf("[radar] bytes %lu | frames %lu (bad header %lu, bad tail %lu) | targets seen %lu, used %lu"
                  " | rejected: near(<%d mm) %lu far %lu wide %lu not-moving %lu, known-clutter %lu\n",
                  (unsigned long)radarBytes, (unsigned long)radarFrames, (unsigned long)radarBadHeader,
                  (unsigned long)radarBadTail, (unsigned long)radarSlotsSeen, (unsigned long)radarAccepted,
                  webRadarNearCutoffMm(), (unsigned long)rejNear, (unsigned long)rejFar,
                  (unsigned long)rejAngle, (unsigned long)rejStatic, (unsigned long)rejClutter);
    if (radarSlotsSeen)
      Serial.printf("[radar] last target: x %d mm, y %d mm, speed %d cm/s -> %.2f m at %+.0f deg (%s)"
                    " [scale x%d; if this reads 10x too small, switch the page to centimetres];"
                    " eyes want gx %+.2f, now %+.2f (iris %+d px of %d)\n",
                    dbgX, dbgY, dbgV, dbgRange / 1000.0f, dbgAngle, dbgVerdict, webRadarScale(),
                    tgt.gx, cur.gx, (int)lroundf(cur.gx * webGazeTravelPx()), webGazeTravelPx());
    if (haveLastFrame) {
      char hex[3 * 30 + 24];
      int n = snprintf(hex, sizeof(hex), "[radar] raw frame:");
      for (int i = 0; i < 30; ++i) n += snprintf(hex + n, sizeof(hex) - n, " %02X", lastFrame[i]);
      Serial.println(hex);
    }
    radarBytes = radarFrames = radarBadHeader = radarBadTail = 0;
    radarSlotsSeen = radarAccepted = rejNear = rejFar = rejAngle = rejStatic = rejClutter = 0;
  }
  fpsFrames = 0; statComposeUs = statWaitUs = statTotalUs = 0; fpsStartMs = now;
}

// Wi-Fi, the settings page and the radar map run on core 0 so they stop
// stealing time from drawing, which owns core 1.
static void webTaskFn(void *) {
  for (;;) {
    webLoop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
static bool webTaskRunning = false;

void skullLoop() {
  if (!webTaskRunning) {
    webTaskRunning = true;
    xTaskCreatePinnedToCore(webTaskFn, "web", 8192, nullptr, 1, nullptr, 0);
    Serial.println("[board] web + Wi-Fi on core 0, drawing on core 1");
  }

  // Schedule / manual on-off, checked twice a second.
  static uint32_t lastCheckMs = 0;
  uint32_t now = millis();
  if (now - lastCheckMs >= 500) {
    lastCheckMs = now;
    if (webLookVersion() != appliedLookVersion) applyLookSettings();
    bool want = webEyesShouldBeOn();
    if (!want && eyesOn) eyesGoToSleep();
    else if (want && !eyesOn) { eyesWakeUp(); lastLoopUs = micros(); }
  }

  if (!eyesOn) {
    if (displaysPowered) pollRadar();      // keep the radar buffer from overflowing
    if (now - fpsStartMs >= 5000) reportStats(now);
    delay(20);
    return;
  }

  uint32_t nowUs = micros();
  float dt = clampf((nowUs - lastLoopUs) / 1e6f, 0.001f, 0.2f);
  statTotalUs += nowUs - lastLoopUs;
  lastLoopUs = nowUs;

  // Learn / forget the room, asked for on the settings page.
  if (webTakeLearnRequest()) {
    clutterCount = 0;
    learnUntilMs = millis() + 20000;
    Serial.println("[room] learning for 20 s - stay out of the radar's view");
  }
  if (webTakeForgetRequest()) {
    clutterCount = 0;
    clutterSave();
    Serial.println("[room] forgot every fixed echo");
  }
  if (learnUntilMs && (int32_t)(millis() - learnUntilMs) >= 0) {
    learnUntilMs = 0;
    clutterSave();
    Serial.printf("[room] done: %d fixed echoes will be ignored\n", clutterCount);
  }

  pollRadar();
  updateBehavior(dt);
  webReportMap(mapSeen, mapSeenCount, (const int16_t *)clutter, clutterCount,
               cur.gx * (float)webRadarFullGazeDeg() * (webRadarInvert() ? -1.0f : 1.0f),
               webRadarNearCutoffMm());

  float blink = blinkAmount();
  if (sleepShut > blink) blink = sleepShut;        // asleep: lids shut over everything

  // Fully shut and nothing changing? Don't redraw the same closed eye.
  static bool lastFrameShut = false;
  const bool shutNow = sleepShut > 0.995f;
  if (shutNow && lastFrameShut) {
    delay(20);
    if (now - fpsStartMs >= 5000) reportStats(now);
    return;
  }
  lastFrameShut = shutNow;

  buildFrame(frameL, true, blink);
  buildFrame(frameR, false, blink);

  drawEye(true, &frameL);
  pollRadar();
  drawEye(false, &frameR);
  fpsFrames++;

  now = millis();
  if (now - fpsStartMs >= 5000) reportStats(now);
}
