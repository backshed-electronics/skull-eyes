#pragma once
// skull_web.h - Wi-Fi, settings page, on/off schedule and over-the-air updates.
#include <Arduino.h>

// Start Wi-Fi (home network + the prop's own hotspot), clock, settings page.
void webSetup();

// Call often from loop(). Handles page requests and Wi-Fi/clock upkeep.
void webLoop();

// True when the eyes should be running right now (schedule + manual override).
bool webEyesShouldBeOn();

// Art style chosen on the page (index into the style table).
int webArtStyle();

// The saved style, readable before webSetup() has run.
int webStyleFromFlash();

// Per-eye rotation in degrees, for a screen glued crooked.
void webGetEyeRotation(float &leftDeg, float &rightDeg);

// Colour trim: brightness %, paleness % (toward grey/white), warmth (-50..50),
// and glow strength %.
void webGetColorTrim(int &brightPct, int &palePct, int &warm, int &glowPct);

// Bumped whenever anything on the Look card is saved, so the eyes can re-tint.
uint32_t webLookVersion();

// False = skip foreshortening, tilt and stretched tissue (the slower drawing path).
bool webFancyTurn();

// True when targets that never move should be ignored as furniture.
bool webIgnoreStatic();

// Display clock in MHz (40, 60 or 80). Takes effect after a restart.
int webSpiMhz();

// How quickly the eyes chase the radar: 1 = smooth and slow, 10 = snap to it.
int webTrackResponse();

// Sleep with the eyes shut until the radar sees movement.
bool webSleepUntilMoved();

// Stay awake this long after the last person leaves (seconds).
int webWakeHoldSecs();

// Live radar picture for the page's map.
struct MapTarget {
  int16_t x, y;      // mm, +x to the right, +y away from the radar
  int16_t speed;     // cm/s
  uint8_t state;     // 0 followed, 1 seen, 2 too near, 3 known clutter, 4 not moving, 5 out of range
};
void webReportMap(const MapTarget *targets, int count, const int16_t *clutterXY, int clutterCount,
                  float gazeDeg, int nearCutMm);

// One-shot requests from the page: learn the room / forget it.
bool webTakeLearnRequest();
bool webTakeForgetRequest();

// How many fixed echoes are currently known, for the page to show.
void webReportClutter(int count, bool learning);

// Follow only targets that are moving (ignores furniture and walls).
bool webFollowMovingOnly();

// Keep following a person for this long after they stand still (seconds).
int webStillHoldSecs();

// True while the "Test sweep" button on the page is running (it times out).
bool webTestSweep();

// The angle (degrees off centre) at which the eyes look as far as they go.
int webRadarFullGazeDeg();

// Multiplier applied to the radar's raw x/y to get millimetres (1 or 10).
int webRadarScale();

// Ignore radar targets closer than this many mm (set on the page).
int webRadarNearCutoffMm();

// True when the radar's left/right should be flipped (set on the page).
bool webRadarInvert();

// Live radar status for the page.
void webReportRadar(bool hasTarget, float range_mm, float angle_deg, unsigned long frames);

// Eyelids: -1 = whatever the style ships with, 0..n a specific set,
// 100 = none (plain black lids).
int webLidSet();

// Tell the page what the eyelid sets are called (call before webSetup).
void webSetLidNames(const char *const *names, int count);

// Pupil shape: 0 = whatever the style wants, 1 = vertical slit, 2 = round.
int webPupilShape();

// How far the eyes travel side to side, in pixels from centre.
int webGazeTravelPx();

// Per-eye nudge in pixels, for screens that ended up mounted slightly off.
void webGetEyeNudge(int &leftX, int &leftY, int &rightX, int &rightY);

// Tell the page what the available art styles are called (call before webSetup).
void webSetStyleNames(const char *const *names, int count);

// Status the page shows.
void webReportStats(float fps, const char *mood, bool dma, uint32_t artInRamKB, bool powerSwitch);
