// skull_web.cpp - Wi-Fi, settings page, on/off schedule, over-the-air updates.
//
// How you reach the settings page:
//   * On your home Wi-Fi:  http://skull.local   (or the IP address it prints)
//   * No home Wi-Fi:       join the Wi-Fi network "SkullEyes" (password below),
//                          the page opens by itself, or go to http://192.168.4.1
//
// The "SkullEyes" hotspot only runs while the prop is NOT connected to your
// home Wi-Fi (no network saved, wrong password, or out of range for 45 s). Once
// home Wi-Fi connects, the hotspot shuts off to save battery.
//
// Everything set on the page is saved in the XIAO's flash and survives power-off.

#include "skull_web.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>
#include <sys/time.h>

// ---------------------------------------------------------------------
// Defaults (all of these can be changed from the page later)
// ---------------------------------------------------------------------
static const char *HOTSPOT_NAME = "SkullEyes";
static const char *DEFAULT_HOTSPOT_PASSWORD = "skulleyes";   // 8+ characters
static const char *HOSTNAME = "skull";   // -> http://skull.local
// Kept as "pumpkin" deliberately: this is the name the settings are stored
// under, and changing it would lose everything saved on the old firmware.
static const char *SETTINGS_NAMESPACE = "pumpkin";

struct Zone { const char *name; const char *posix; };
static const Zone ZONES[] = {
  { "Eastern",            "EST5EDT,M3.2.0,M11.1.0" },
  { "Central",            "CST6CDT,M3.2.0,M11.1.0" },
  { "Mountain",           "MST7MDT,M3.2.0,M11.1.0" },
  { "Arizona (no DST)",   "MST7" },
  { "Pacific",            "PST8PDT,M3.2.0,M11.1.0" },
  { "Alaska",             "AKST9AKDT,M3.2.0,M11.1.0" },
  { "Hawaii",             "HST10" },
};
static constexpr int ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

enum RunMode : uint8_t { MODE_SCHEDULE = 0, MODE_ALWAYS_ON = 1, MODE_ALWAYS_OFF = 2 };

struct Window { bool enabled; uint16_t onMin, offMin; };   // minutes after midnight

struct Settings {
  String ssid, pass;
  String hotspotPass = DEFAULT_HOTSPOT_PASSWORD;
  uint8_t zone = 1;                        // Central
  uint8_t mode = MODE_ALWAYS_ON;           // until a schedule is saved
  uint8_t days = 0x7F;                     // bit 0 = Sunday ... bit 6 = Saturday
  Window evening = { true, 18 * 60, 23 * 60 };
  Window morning = { false, 6 * 60 + 30, 8 * 60 };
  uint8_t style = 0;                       // art style index
  int8_t lx = 0, ly = 0, rx = 0, ry = 0;   // per-eye nudge, pixels
  int8_t lrot = 0, rrot = 0;               // per-eye rotation, degrees
  uint8_t pupilShape = 0;                  // 0 style default, 1 slit, 2 round
  int8_t lidSet = -1;                      // -1 style default, 0..n a set, 100 none
  uint8_t travel = 52;                     // gaze travel, pixels from centre
  uint8_t bright = 100;                    // brightness %
  uint8_t pale = 0;                        // paleness % (toward grey/white)
  int8_t warm = 0;                         // -50 colder ... +50 warmer
  uint8_t glow = 100;                      // glow strength %
  bool rinv = false;                       // flip radar left/right
  uint16_t rnear = 750;                    // ignore targets closer than this (mm)
  bool fancy = true;                     // foreshortening, tilt, stretched tissue
                                           // (on by default where there is room for it)
  uint8_t rscale = 1;                      // raw radar units -> mm (1 = mm, 10 = cm)
  uint8_t rfull = 22;                      // angle that gives a full side look
  bool movingOnly = true;                  // ignore stationary echoes (furniture, walls)
  uint8_t stillHold = 8;                   // keep following this long after they stop (s)
  uint8_t spiMhz = 40;                     // display clock; 60/80 are worth trying
  uint8_t trackResp = 8;                   // 1 smooth ... 10 snap
  bool sleepMode = false;                  // eyes shut until the radar sees movement
  uint8_t wakeHold = 12;                   // stay awake this long after they leave (s)
  bool rstatic = true;                     // ignore targets that never move
} cfg;

static uint32_t sweepUntilMs = 0;
static bool learnReq = false, forgetReq = false;
static int clutterCount = 0;
static bool clutterLearning = false;

bool webTakeLearnRequest() { bool r = learnReq; learnReq = false; return r; }
bool webTakeForgetRequest() { bool r = forgetReq; forgetReq = false; return r; }
void webReportClutter(int count, bool learning) { clutterCount = count; clutterLearning = learning; }

bool webTestSweep() { return sweepUntilMs && (int32_t)(millis() - sweepUntilMs) < 0; }

static MapTarget mapTargets[3];
static int mapCount = 0;
static int16_t mapClutter[32];
static int mapClutterCount = 0;
static float mapGaze = 0;
static int mapNearCut = 750;

void webReportMap(const MapTarget *targets, int count, const int16_t *clutterXY, int clutterCount,
                  float gazeDeg, int nearCutMm) {
  mapCount = count > 3 ? 3 : count;
  for (int i = 0; i < mapCount; ++i) mapTargets[i] = targets[i];
  mapClutterCount = clutterCount > 16 ? 16 : clutterCount;
  for (int i = 0; i < mapClutterCount * 2; ++i) mapClutter[i] = clutterXY[i];
  mapGaze = gazeDeg;
  mapNearCut = nearCutMm;
}

static bool radarSeen = false;
static float radarRange = 0, radarAngle = 0;
static unsigned long radarFrameCount = 0;

bool webRadarInvert() { return cfg.rinv; }

int webRadarNearCutoffMm() { return cfg.rnear; }

bool webFancyTurn() { return cfg.fancy; }

int webRadarScale() { return cfg.rscale == 10 ? 10 : 1; }

int webRadarFullGazeDeg() { return cfg.rfull < 10 ? 10 : cfg.rfull; }

bool webFollowMovingOnly() { return cfg.movingOnly; }

int webSpiMhz() { return cfg.spiMhz == 80 ? 80 : (cfg.spiMhz == 60 ? 60 : 40); }

int webTrackResponse() { return cfg.trackResp < 1 ? 1 : (cfg.trackResp > 10 ? 10 : cfg.trackResp); }

bool webSleepUntilMoved() { return cfg.sleepMode; }

int webWakeHoldSecs() { return cfg.wakeHold; }

int webStillHoldSecs() { return cfg.stillHold; }

bool webIgnoreStatic() { return cfg.rstatic; }

void webReportRadar(bool hasTarget, float range_mm, float angle_deg, unsigned long frames) {
  radarSeen = hasTarget; radarRange = range_mm; radarAngle = angle_deg; radarFrameCount = frames;
}

static uint32_t lookVersion = 1;

uint32_t webLookVersion() { return lookVersion; }

void webGetColorTrim(int &brightPct, int &palePct, int &warm, int &glowPct) {
  brightPct = cfg.bright; palePct = cfg.pale; warm = cfg.warm; glowPct = cfg.glow;
}

static const char *const *styleNames = nullptr;
static int styleCount = 0;
static const char *const *lidNames = nullptr;
static int lidCount = 0;

void webSetLidNames(const char *const *names, int count) { lidNames = names; lidCount = count; }

int webLidSet() { return cfg.lidSet; }

void webSetStyleNames(const char *const *names, int count) {
  styleNames = names; styleCount = count;
}

int webArtStyle() { return cfg.style < styleCount ? cfg.style : 0; }

static void loadSettings();

int webStyleFromFlash() {
  loadSettings();           // safe to call early; webSetup() calls it again
  return cfg.style;
}

void webGetEyeNudge(int &leftX, int &leftY, int &rightX, int &rightY) {
  leftX = cfg.lx; leftY = cfg.ly; rightX = cfg.rx; rightY = cfg.ry;
}

void webGetEyeRotation(float &leftDeg, float &rightDeg) {
  leftDeg = cfg.lrot; rightDeg = cfg.rrot;
}

int webGazeTravelPx() { return cfg.travel; }

int webPupilShape() { return cfg.pupilShape > 2 ? 0 : cfg.pupilShape; }

static Preferences prefs;
static WebServer server(80);
static DNSServer dns;
static bool wasConnected = false;
static bool mdnsStarted = false;
static bool hotspotOn = false;
static uint32_t lastConnectedMs = 0;     // or boot time
static uint32_t connectedSinceMs = 0;
static constexpr uint32_t HOTSPOT_AFTER_MS = 45000;   // no home Wi-Fi this long -> hotspot on
static constexpr uint32_t HOTSPOT_OFF_AFTER_MS = 20000; // home Wi-Fi stable this long -> hotspot off

// Stats from the eyes
static float statFps = 0;
static String statMood = "";
static bool statDma = false;
static uint32_t statArtKB = 0;
static bool statPowerSwitch = false;

void webReportStats(float fps, const char *mood, bool dma, uint32_t artInRamKB, bool powerSwitch) {
  statFps = fps; statMood = mood; statDma = dma; statArtKB = artInRamKB; statPowerSwitch = powerSwitch;
}

// ---------------------------------------------------------------------
// Settings storage
// ---------------------------------------------------------------------
static void loadSettings() {
  prefs.begin(SETTINGS_NAMESPACE, true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.hotspotPass = prefs.getString("appass", DEFAULT_HOTSPOT_PASSWORD);
  cfg.zone = prefs.getUChar("zone", 1);
  cfg.mode = prefs.getUChar("mode", MODE_ALWAYS_ON);
  cfg.days = prefs.getUChar("days", 0x7F);
  cfg.evening.enabled = prefs.getBool("e_en", true);
  cfg.evening.onMin = prefs.getUShort("e_on", 18 * 60);
  cfg.evening.offMin = prefs.getUShort("e_off", 23 * 60);
  cfg.morning.enabled = prefs.getBool("m_en", false);
  cfg.morning.onMin = prefs.getUShort("m_on", 6 * 60 + 30);
  cfg.morning.offMin = prefs.getUShort("m_off", 8 * 60);
  cfg.style = prefs.getUChar("style", 0);
  cfg.lx = prefs.getChar("lx", 0); cfg.ly = prefs.getChar("ly", 0);
  cfg.rx = prefs.getChar("rx", 0); cfg.ry = prefs.getChar("ry", 0);
  cfg.lrot = prefs.getChar("lrot", 0); cfg.rrot = prefs.getChar("rrot", 0);
  cfg.travel = prefs.getUChar("travel", 52);
  cfg.pupilShape = prefs.getUChar("pupsh", 0);
  cfg.lidSet = prefs.getChar("lidset", -1);
  cfg.bright = prefs.getUChar("bright", 100);
  cfg.pale = prefs.getUChar("pale", 0);
  cfg.warm = prefs.getChar("warm", 0);
  cfg.glow = prefs.getUChar("glow", 100);
  cfg.rinv = prefs.getBool("rinv", false);
  cfg.rnear = prefs.getUShort("rnear", 750);
  cfg.fancy = prefs.getBool("fancy", true);
  cfg.rscale = prefs.getUChar("rscale", 1);
  cfg.rfull = prefs.getUChar("rfull", 22);
  cfg.movingOnly = prefs.getBool("movonly", true);
  cfg.stillHold = prefs.getUChar("still", 8);
  cfg.spiMhz = prefs.getUChar("spimhz", 40);
  cfg.trackResp = prefs.getUChar("tresp", 8);
  cfg.sleepMode = prefs.getBool("sleepm", false);
  cfg.wakeHold = prefs.getUChar("wakeh", 12);
  cfg.rstatic = prefs.getBool("rstatic", true);
  prefs.end();
  if (cfg.zone >= ZONE_COUNT) cfg.zone = 1;
}

static void saveSettings() {
  prefs.begin(SETTINGS_NAMESPACE, false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("appass", cfg.hotspotPass);
  prefs.putUChar("zone", cfg.zone);
  prefs.putUChar("mode", cfg.mode);
  prefs.putUChar("days", cfg.days);
  prefs.putBool("e_en", cfg.evening.enabled);
  prefs.putUShort("e_on", cfg.evening.onMin);
  prefs.putUShort("e_off", cfg.evening.offMin);
  prefs.putBool("m_en", cfg.morning.enabled);
  prefs.putUShort("m_on", cfg.morning.onMin);
  prefs.putUShort("m_off", cfg.morning.offMin);
  prefs.putUChar("style", cfg.style);
  prefs.putChar("lx", cfg.lx); prefs.putChar("ly", cfg.ly);
  prefs.putChar("rx", cfg.rx); prefs.putChar("ry", cfg.ry);
  prefs.putChar("lrot", cfg.lrot); prefs.putChar("rrot", cfg.rrot);
  prefs.putUChar("travel", cfg.travel);
  prefs.putUChar("pupsh", cfg.pupilShape);
  prefs.putChar("lidset", cfg.lidSet);
  prefs.putUChar("bright", cfg.bright);
  prefs.putUChar("pale", cfg.pale);
  prefs.putChar("warm", cfg.warm);
  prefs.putUChar("glow", cfg.glow);
  prefs.putBool("rinv", cfg.rinv);
  prefs.putUShort("rnear", cfg.rnear);
  prefs.putBool("fancy", cfg.fancy);
  prefs.putUChar("rscale", cfg.rscale);
  prefs.putUChar("rfull", cfg.rfull);
  prefs.putBool("movonly", cfg.movingOnly);
  prefs.putUChar("still", cfg.stillHold);
  prefs.putUChar("spimhz", cfg.spiMhz);
  prefs.putUChar("tresp", cfg.trackResp);
  prefs.putBool("sleepm", cfg.sleepMode);
  prefs.putUChar("wakeh", cfg.wakeHold);
  prefs.putBool("rstatic", cfg.rstatic);
  prefs.end();
}

// ---------------------------------------------------------------------
// Clock and schedule
// ---------------------------------------------------------------------
static bool clockValid() { return time(nullptr) > 1700000000; }

static void applyTimeZone() {
  setenv("TZ", ZONES[cfg.zone].posix, 1);
  tzset();
}

static bool inWindow(const Window &w, int nowMin, int wday) {
  if (!w.enabled || w.onMin == w.offMin) return false;
  if (w.onMin < w.offMin) {
    return nowMin >= w.onMin && nowMin < w.offMin && (cfg.days >> wday & 1);
  }
  // Window crosses midnight (e.g. 7 PM to 1 AM): the day it *started* counts.
  if (nowMin >= w.onMin) return cfg.days >> wday & 1;
  if (nowMin < w.offMin) return cfg.days >> ((wday + 6) % 7) & 1;
  return false;
}

static const char *whyText = "";

bool webEyesShouldBeOn() {
  if (cfg.mode == MODE_ALWAYS_ON)  { whyText = "Always on"; return true; }
  if (cfg.mode == MODE_ALWAYS_OFF) { whyText = "Always off"; return false; }
  if (!clockValid()) { whyText = "Schedule (clock not set yet, staying on)"; return true; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int m = t.tm_hour * 60 + t.tm_min;
  bool on = inWindow(cfg.evening, m, t.tm_wday) || inWindow(cfg.morning, m, t.tm_wday);
  whyText = on ? "Schedule (inside an on-time)" : "Schedule (outside on-times)";
  return on;
}

// ---------------------------------------------------------------------
// Page helpers
// ---------------------------------------------------------------------
static String esc(const String &s) {
  String o;
  for (char c : s) {
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}

static String hhmm(uint16_t m) {
  char b[12];
  snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(m / 60) % 24, (unsigned)(m % 60));
  return b;
}

static uint16_t parseHHMM(const String &s, uint16_t fallback) {
  int c = s.indexOf(':');
  if (c < 1) return fallback;
  int h = s.substring(0, c).toInt(), mi = s.substring(c + 1).toInt();
  if (h < 0 || h > 23 || mi < 0 || mi > 59) return fallback;
  return (uint16_t)(h * 60 + mi);
}

static String nowText() {
  if (!clockValid()) return "not set yet";
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char b[40];
  strftime(b, sizeof(b), "%a %b %d, %I:%M:%S %p", &t);
  return b;
}

static String wifiText() {
  if (cfg.ssid.isEmpty()) return "No home network set (hotspot only)";
  if (WiFi.status() == WL_CONNECTED)
    return "Connected to " + esc(cfg.ssid) + " &mdash; " + WiFi.localIP().toString();
  return "Trying to connect to " + esc(cfg.ssid) + "&hellip;";
}

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta charset="utf-8"><title>Skull Eyes</title>
<style>
body{font-family:system-ui,sans-serif;background:#141210;color:#eee;margin:0;padding:16px;max-width:560px}
h1{color:#ff8a1f;margin:4px 0 12px}h2{color:#ffb35c;font-size:1.05em;margin:0 0 10px}
.card{background:#211d19;border:1px solid #3a322a;border-radius:12px;padding:14px;margin:0 0 14px}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin:6px 0}
label{min-width:88px}input[type=time],input[type=text],input[type=password],select{
background:#0f0d0b;color:#eee;border:1px solid #4a4036;border-radius:8px;padding:8px;font-size:1em}
button,input[type=submit]{background:#ff8a1f;color:#1a1208;border:0;border-radius:8px;padding:10px 14px;
font-weight:600;font-size:1em}.sel{outline:3px solid #fff}
.days label{min-width:0;display:inline-flex;gap:3px;margin-right:6px}
.muted{color:#a99;font-size:.9em}b.on{color:#7dff7d}b.off{color:#ff7d7d}
</style></head><body><h1>&#128128; Skull Eyes</h1>
)HTML";

static const char PAGE_SCRIPT[] PROGMEM = R"HTML(
<script>
const cv=document.getElementById('map');
// Top-down: radar at the bottom centre, +x right, +y away.
function drawMap(m){
  if(!cv) return;
  const g=cv.getContext('2d'), W=cv.width, H=cv.height, ox=W/2, oy=H-14, MAX=6000, sc=(H-30)/MAX;
  const px=(x,y)=>[ox+x*sc, oy-y*sc];
  const pol=(deg,r)=>px(Math.sin(deg*Math.PI/180)*r, Math.cos(deg*Math.PI/180)*r);
  g.clearRect(0,0,W,H);
  g.strokeStyle='#332b23'; g.fillStyle='#7a6b5a'; g.font='11px system-ui';
  for(let r=1;r<=6;r++){                                  // range rings every metre
    g.beginPath();
    for(let a=-60;a<=60;a+=4){ const [x,y]=pol(a,r*1000); a===-60?g.moveTo(x,y):g.lineTo(x,y); }
    g.stroke();
    const [lx,ly]=pol(0,r*1000); g.fillText(r+' m', lx+4, ly+12);
  }
  for(const a of [-60,-30,0,30,60]){                      // the radar's field of view
    const [x,y]=pol(a,MAX); g.beginPath(); g.moveTo(ox,oy); g.lineTo(x,y);
    g.strokeStyle=(a===0)?'#4a4036':'#2b241d'; g.stroke();
  }
  g.setLineDash([4,4]); g.strokeStyle='#8a5a2a'; g.beginPath();   // "ignore closer than"
  for(let a=-60;a<=60;a+=4){ const [x,y]=pol(a,m.near); a===-60?g.moveTo(x,y):g.lineTo(x,y); }
  g.stroke(); g.setLineDash([]);
  g.strokeStyle='#6b6258';                                // learned furniture
  for(let i=0;i<m.clutter.length;i+=2){ const [x,y]=px(m.clutter[i],m.clutter[i+1]);
    g.beginPath(); g.moveTo(x-5,y-5); g.lineTo(x+5,y+5); g.moveTo(x+5,y-5); g.lineTo(x-5,y+5); g.stroke(); }
  const col=['#7dff7d','#ffb020','#7a4030','#6b6258','#5a5a70','#40404a'];
  for(const t of m.targets){ const [x,y]=px(t.x,t.y);
    g.fillStyle=col[t.s]||'#888'; g.beginPath(); g.arc(x,y,t.s===0?7:5,0,7); g.fill();
    if(t.s===0){ g.fillStyle='#cfc';
      g.fillText((Math.hypot(t.x,t.y)/1000).toFixed(1)+' m  '+t.v+' cm/s', x+11, y+4); } }
  const [gx,gy]=pol(m.gaze,1100);                         // where the eyes are pointing
  g.strokeStyle='#9fd0ff'; g.lineWidth=2;
  g.beginPath(); g.moveTo(ox,oy); g.lineTo(gx,gy); g.stroke(); g.lineWidth=1;
  g.fillStyle='#ff8a1f'; g.beginPath(); g.arc(ox,oy,4,0,7); g.fill();
}
setInterval(()=>fetch('/map.json').then(r=>r.json()).then(drawMap),250);

// If it doesn't know the time yet, give it this phone's time.
fetch('/status').then(r=>r.json()).then(s=>{
  if(!s.clock){fetch('/settime',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'epoch='+Math.floor(Date.now()/1000)}).then(()=>setTimeout(()=>location.reload(),500));}
});
setInterval(()=>fetch('/status').then(r=>r.json()).then(s=>{
  document.getElementById('clk').textContent=s.time;
  document.getElementById('fps').textContent=s.fps;
  document.getElementById('mood').textContent=s.mood;
  const e=document.getElementById('eyes');e.textContent=s.on?'ON':'OFF';e.className=s.on?'on':'off';
  document.getElementById('why').textContent=s.why;
}),3000);
</script></body></html>
)HTML";

static void sendPage() {
  bool on = webEyesShouldBeOn();
  String p;
  p.reserve(7000);
  p += FPSTR(PAGE_HEAD);

  // Status
  p += "<div class='card'><h2>Status</h2>";
  p += "<div>Eyes: <b id='eyes' class='" + String(on ? "on'>ON" : "off'>OFF") + "</b> &mdash; <span id='why'>" + whyText + "</span></div>";
  p += "<div>Time: <span id='clk'>" + nowText() + "</span> (" + ZONES[cfg.zone].name + ")</div>";
  p += "<div>Wi-Fi: " + wifiText() + (hotspotOn ? " &mdash; hotspot on" : "") + "</div>";
  p += "<div>Radar: " + String(radarFrameCount ? "" : "no data &mdash; check TX wire, 47 &ohm; resistor, 5 V and ground") +
       (radarFrameCount && radarSeen ? "tracking someone at " + String(radarRange / 1000.0, 1) + " m, " +
        String(radarAngle, 0) + "&deg;" : (radarFrameCount ? "working, nobody in view" : "")) + "</div>";
  p += "<div>At off-times: " + String(statPowerSwitch ? "screens and radar are switched off" :
       "screens go black (no power switch installed)") + "</div>";
  p += "<div class='muted'>Animation: <span id='fps'>" + String(statFps, 1) + "</span> fps, mood <span id='mood'>" + esc(statMood) +
       "</span>, " + (statDma ? "DMA" : "fallback") + " drawing, " + String(statArtKB) + " KB art in RAM, " +
       String(ESP.getFreeHeap() / 1024) + " KB RAM free</div></div>";

  // Live radar map
  p += "<div class='card'><h2>What the radar sees</h2>";
  p += "<canvas id='map' width='340' height='300' style='width:100%;max-width:340px;background:#0f0d0b;"
       "border:1px solid #4a4036;border-radius:10px'></canvas>";
  p += "<div class='muted'>Top-down view, the radar at the bottom centre. Green is whoever the eyes are "
       "following, amber is seen but not chosen, grey X is learned furniture, dotted arc is the "
       "\"ignore closer than\" limit. The line shows where the eyes are pointing.</div></div>";

  // Room learning
  p += "<div class='card'><h2>The room</h2>";
  p += "<div>" + String(clutterLearning ? "Learning the room now &mdash; stay out of the radar's view"
                                        : String(clutterCount) + " fixed echoes are being ignored") + "</div>";
  p += "<form method='post' action='/room' class='row'>";
  p += "<button name='r' value='learn'>Learn the room (20 s)</button>";
  p += "<button name='r' value='forget'>Forget</button></form>";
  p += "<div class='muted'>Walk out of the radar's view, press Learn, and stay away for 20 seconds. "
       "Everything it can still see is furniture, and it will be ignored from then on. Saved, so it "
       "survives a power cut. Do it again if you move the prop or rearrange the room.</div></div>";

  // Test sweep
  p += "<div class='card'><h2>Test</h2><form method='post' action='/sweep' class='row'>";
  p += "<button name='s' value='1'>Sweep eyes side to side (60 s)</button>";
  p += "<button name='s' value='0'>Stop</button></form>";
  p += "<div class='muted'>Ignores the radar and swings both eyes their full travel, left to right. "
       "If you can't see this from where you stand, the eyes are moving less than you can see and the "
       "travel setting below needs raising &mdash; it isn't a tracking problem.</div></div>";

  // Mode
  p += "<div class='card'><h2>Mode</h2><form method='post' action='/mode' class='row'>";
  const char *names[] = { "Follow schedule", "Always on", "Always off" };
  const char *vals[] = { "schedule", "on", "off" };
  for (int i = 0; i < 3; ++i) {
    p += "<button name='m' value='" + String(vals[i]) + "'" + (cfg.mode == i ? " class='sel'" : "") + ">" + names[i] + "</button>";
  }
  p += "</form></div>";

  // Schedule
  p += "<div class='card'><h2>Schedule</h2><form method='post' action='/schedule'>";
  const char *dn[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  p += "<div class='row days'>";
  for (int d = 0; d < 7; ++d) {
    p += "<label><input type='checkbox' name='d" + String(d) + "'" + ((cfg.days >> d & 1) ? " checked" : "") + ">" + dn[d] + "</label>";
  }
  p += "</div>";
  auto win = [&](const char *title, const char *k, const Window &w) {
    p += "<div class='row'><label><input type='checkbox' name='" + String(k) + "en'" + (w.enabled ? " checked" : "") + "> " + title + "</label>";
    p += "on <input type='time' name='" + String(k) + "on' value='" + hhmm(w.onMin) + "'>";
    p += "off <input type='time' name='" + String(k) + "off' value='" + hhmm(w.offMin) + "'></div>";
  };
  win("Evening", "e", cfg.evening);
  win("Morning", "m", cfg.morning);
  p += "<div class='row'><label>Time zone</label><select name='zone'>";
  for (int z = 0; z < ZONE_COUNT; ++z) {
    p += "<option value='" + String(z) + "'" + (cfg.zone == z ? " selected" : "") + ">" + ZONES[z].name + "</option>";
  }
  p += "</select></div><div class='muted'>Saving the schedule also switches Mode to &ldquo;Follow schedule&rdquo;. "
       "An on-time can run past midnight (e.g. 7:00 PM to 1:00 AM).</div>"
       "<div class='row'><input type='submit' value='Save schedule'></div></form></div>";

  // Look: art style and per-eye alignment
  p += "<div class='card'><h2>Look</h2><form method='post' action='/look'>";
  p += "<div class='row'><label>Eye style</label><select name='style'>";
  for (int i = 0; i < styleCount; ++i) {
    p += "<option value='" + String(i) + "'" + (cfg.style == i ? " selected" : "") + ">" + styleNames[i] + "</option>";
  }
  p += "</select></div>";
  p += "<div class='row'><label>Eyelids</label><select name='lidset'>";
  p += String("<option value='-1'") + (cfg.lidSet == -1 ? " selected" : "") + ">as the style intends</option>";
  for (int i = 0; i < lidCount; ++i) {
    p += "<option value='" + String(i) + "'" + (cfg.lidSet == i ? " selected" : "") + ">" +
         lidNames[i] + "</option>";
  }
  p += String("<option value='100'") + (cfg.lidSet == 100 ? " selected" : "") +
       ">none - just darkness</option></select></div>";
  p += "<div class='muted'>A bare skull has no eyelids, so dried flesh or darkness suits it better "
       "than fresh skin. With none, a blink reads as the eye sinking back into the socket. "
       "Changing this restarts the prop.</div>";
  p += "<div class='row'><label>Pupil</label><select name='pupsh'>";
  const char *shapes[] = { "as the style intends", "vertical slit (reptile, cat)", "round (human, demon)" };
  for (int i = 0; i < 3; ++i) {
    p += "<option value='" + String(i) + "'" + (cfg.pupilShape == i ? " selected" : "") + ">" +
         shapes[i] + "</option>";
  }
  p += "</select></div>";
  p += "<div class='muted'>Changing the style restarts it (it reloads the artwork).</div>";
  auto nudge = [&](const char *title, const char *kx, int vx, const char *ky, int vy,
                   const char *kr, int vr) {
    p += "<div class='row'><label>" + String(title) + "</label>";
    p += "right <input type='number' name='" + String(kx) + "' value='" + String(vx) + "' min='-60' max='60' style='width:4.5em'>";
    p += "down <input type='number' name='" + String(ky) + "' value='" + String(vy) + "' min='-60' max='60' style='width:4.5em'>";
    p += "tilt&deg; <input type='number' name='" + String(kr) + "' value='" + String(vr) + "' min='-45' max='45' style='width:4.5em'></div>";
  };
  nudge("Left eye", "lx", cfg.lx, "ly", cfg.ly, "lrot", cfg.lrot);
  nudge("Right eye", "rx", cfg.rx, "ry", cfg.ry, "rrot", cfg.rrot);
  auto slider = [&](const char *title, const char *key, int val, int lo, int hi, const char *suffix) {
    p += "<div class='row'><label>" + String(title) + "</label>";
    p += "<input type='range' name='" + String(key) + "' value='" + String(val) + "' min='" + String(lo) +
         "' max='" + String(hi) + "' style='flex:1;min-width:8em'> " + String(val) + String(suffix) + "</div>";
  };
  slider("Brightness", "bright", cfg.bright, 30, 150, "%");
  slider("Paleness", "pale", cfg.pale, 0, 100, "%");
  slider("Warm / cool", "warm", cfg.warm, -50, 50, "");
  slider("Glow", "glow", cfg.glow, 30, 200, "%");
  p += "<div class='muted'>Brightness and glow tame a screen that looks blown out; paleness washes the "
       "colour toward white; warm/cool shifts it toward orange or blue. These retint the artwork in memory, "
       "so they apply straight away &mdash; no restart.</div>";
  p += "<div class='row'><label><input type='checkbox' name='rinv'" + String(cfg.rinv ? " checked" : "") +
       "> Flip radar left/right</label></div>";
  p += "<div class='muted'>Tick this if the eyes look away from you instead of at you.</div>";
  p += "<div class='row'><label>Radar distances</label><select name='rscale'>";
  p += String("<option value='1'") + (cfg.rscale != 10 ? " selected" : "") + ">are in millimetres</option>";
  p += String("<option value='10'") + (cfg.rscale == 10 ? " selected" : "") + ">are in centimetres (x10)</option>";
  p += "</select></div>";
  p += "<div class='muted'>Stand a known distance away and watch the Serial Monitor or the radar line "
       "above. If it reads ten times too small, switch to centimetres.</div>";
  p += "<div class='row'><label>Display clock</label><select name='spimhz'>";
  for (int mhz : {40, 60, 80}) {
    p += "<option value='" + String(mhz) + "'" + (cfg.spiMhz == mhz ? " selected" : "") + ">" +
         String(mhz) + " MHz</option>";
  }
  p += "</select></div>";
  p += "<div class='muted'>How fast frames are pushed to the screens, which caps the frame rate: "
       "40 MHz allows about 21 fps, 80 MHz about 40. Higher needs short, tidy wiring - if the eyes "
       "show speckles or garbage, come back down. Takes effect after a restart.</div>";
  p += "<div class='row'><label>Tracking response</label><input type='range' name='tresp' value='" +
       String(cfg.trackResp) + "' min='1' max='10' style='flex:1;min-width:8em'> " + String(cfg.trackResp) + "/10</div>";
  p += "<div class='muted'>1 glides smoothly and lags behind; 10 snaps onto the person and holds lock step. "
       "High settings show the radar's own jitter, which reads as the eyes twitching.</div>";
  p += "<div class='row'><label><input type='checkbox' name='sleepm'" + String(cfg.sleepMode ? " checked" : "") +
       "> Sleep with eyes shut until someone moves</label></div>";
  p += "<div class='row'><label>Stay awake for</label><input type='number' name='wakeh' value='" +
       String(cfg.wakeHold) + "' min='2' max='120' style='width:5em'> s after they leave</div>";
  p += "<div class='muted'>The eyes stay closed, then open and lock straight onto whoever walked up. "
       "Far better as a scare than eyes already darting around, and it draws less power while nobody "
       "is there.</div>";
  p += "<div class='row'><label><input type='checkbox' name='movonly'" + String(cfg.movingOnly ? " checked" : "") +
       "> Only follow things that move</label></div>";
  p += "<div class='row'><label>Keep watching for</label><input type='number' name='still' value='" +
       String(cfg.stillHold) + "' min='0' max='60' style='width:5em'> s after they stop</div>";
  p += "<div class='muted'>Indoors the radar sees appliances, cabinets and walls as targets and will "
       "happily stare at the fridge. With this ticked it only picks up someone who is moving, then keeps "
       "watching them for the time above once they stand still.</div>";
  p += "<div class='row'><label>Full look at</label><input type='number' name='rfull' value='" +
       String(cfg.rfull) + "' min='10' max='60' step='5' style='width:5em'> &deg; off centre</div>";
  p += "<div class='muted'>How far someone has to be off to one side before the eyes look as far as "
       "they go. 30&deg; makes normal movement across a room obvious; 60&deg; matches the radar's full "
       "width but makes the eyes barely twitch for someone walking past.</div>";
  p += "<div class='row'><label><input type='checkbox' name='rstatic'" + String(cfg.rstatic ? " checked" : "") +
       "> Ignore things that never move</label></div>";
  p += "<div class='muted'>Radar reports furniture, appliances and walls as targets. With this on, "
       "anything that stays put for 6 seconds is treated as part of the room and skipped until it "
       "moves again. Someone standing perfectly still will also be dropped after 6 seconds.</div>";
  p += "<div class='row'><label>Ignore closer than</label><input type='number' name='rnear' value='" +
       String(cfg.rnear) + "' min='0' max='3000' step='50' style='width:6em'> mm</div>";
  p += "<div class='muted'>The radar's nearest range step is 750 mm wide, and that first step picks up "
       "the prop itself, nearby wiring and the module's own surroundings as if they were people. "
       "750 throws that whole step away. Raise it if the eyes lock onto something that isn't there; "
       "lower it if you want them to react to someone right at the crate.</div>";
  p += "<div class='row'><label><input type='checkbox' name='fancy'" + String(cfg.fancy ? " checked" : "") +
       "> Foreshorten and stretch when looking aside</label></div>";
  p += "<div class='muted'>The iris narrows as the eye turns away, and the tissue behind it stretches. "
       "Costs a little drawing time; untick it if you ever want the frame rate back.</div>";
  p += "<div class='row'><label>Eye travel</label><input type='number' name='travel' value='" + String(cfg.travel) +
       "' min='10' max='55' style='width:5em'> px side to side</div>";
  p += "<div class='muted'>Travel is how far the iris slides from centre at a full side-glance: 20 is a "
       "gentle look, 40 is a hard glance with the tissue behind the eye stretching, 55 is extreme. "
       "The radar's full &plusmn;60&deg; of coverage maps onto this range. "
       "Tilt rotates everything on that screen, for one glued in crooked.</div>";
  p += "<div class='muted'>Nudges the whole eye inside its screen, in pixels, to line the two up when the screens "
       "aren't mounted level. About 190 pixels per inch: 1/8 in &asymp; 24, 3/16 in &asymp; 35, 1/4 in &asymp; 47. "
       "A screen glued low needs a negative \"down\" value.</div>";
  p += "<div class='row'><input type='submit' value='Save look'></div></form></div>";

  // Wi-Fi
  p += "<div class='card'><h2>Wi-Fi</h2><form method='post' action='/wifi'>";
  p += "<div class='row'><label>Home network</label><input type='text' name='ssid' value='" + esc(cfg.ssid) + "'></div>";
  p += "<div class='row'><label>Password</label><input type='password' name='pass' placeholder='(unchanged)'></div>";
  p += "<div class='row'><label>Hotspot password</label><input type='text' name='appass' value='" + esc(cfg.hotspotPass) + "'></div>";
  p += "<div class='muted'>The prop's own network &ldquo;" + String(HOTSPOT_NAME) +
       "&rdquo; only appears when it can't reach your home Wi-Fi. Hotspot password must be 8+ characters. "
       "Saving restarts it.</div>";
  p += "<div class='row'><input type='submit' value='Save &amp; restart'></div></form></div>";

  // Firmware
  p += "<div class='card'><h2>Firmware update</h2><form method='post' action='/update' enctype='multipart/form-data'>";
  p += "<div class='row'><input type='file' name='fw' accept='.bin'></div>";
  p += "<div class='muted'>Choose SkullEyes.ino.bin. The eyes freeze during the upload, then it restarts.</div>";
  p += "<div class='row'><input type='submit' value='Upload firmware'></div></form></div>";

  p += "<div class='muted'>Addresses: http://" + String(HOSTNAME) + ".local &nbsp; ";
  if (WiFi.status() == WL_CONNECTED) p += "http://" + WiFi.localIP().toString() + " &nbsp; ";
  p += "hotspot: http://192.168.4.1</div>";
  p += FPSTR(PAGE_SCRIPT);
  server.send(200, "text/html", p);
}

static void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

// ---------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------
static void setupRoutes() {
  server.on("/", HTTP_GET, sendPage);

  server.on("/status", HTTP_GET, []() {
    bool on = webEyesShouldBeOn();
    String j = "{\"on\":" + String(on ? "true" : "false") + ",\"why\":\"" + whyText +
               "\",\"clock\":" + (clockValid() ? "true" : "false") + ",\"time\":\"" + nowText() +
               "\",\"fps\":\"" + String(statFps, 1) + "\",\"mood\":\"" + statMood + "\"}";
    server.send(200, "application/json", j);
  });

  server.on("/map.json", HTTP_GET, []() {
    String j = "{\"near\":" + String(mapNearCut) + ",\"gaze\":" + String(mapGaze, 1) + ",\"targets\":[";
    for (int i = 0; i < mapCount; ++i) {
      if (i) j += ",";
      j += "{\"x\":" + String(mapTargets[i].x) + ",\"y\":" + String(mapTargets[i].y) +
           ",\"v\":" + String(mapTargets[i].speed) + ",\"s\":" + String(mapTargets[i].state) + "}";
    }
    j += "],\"clutter\":[";
    for (int i = 0; i < mapClutterCount * 2; ++i) { if (i) j += ","; j += String(mapClutter[i]); }
    j += "]}";
    server.send(200, "application/json", j);
  });

  server.on("/room", HTTP_POST, []() {
    if (server.arg("r") == "learn") learnReq = true; else forgetReq = true;
    redirectHome();
  });

  server.on("/sweep", HTTP_POST, []() {
    sweepUntilMs = server.arg("s") == "1" ? millis() + 60000 : 0;
    Serial.printf("[test] sweep %s\n", server.arg("s") == "1" ? "started (60 s)" : "stopped");
    redirectHome();
  });

  server.on("/mode", HTTP_POST, []() {
    String m = server.arg("m");
    cfg.mode = m == "on" ? MODE_ALWAYS_ON : (m == "off" ? MODE_ALWAYS_OFF : MODE_SCHEDULE);
    saveSettings();
    Serial.printf("[web] mode -> %s\n", m.c_str());
    redirectHome();
  });

  server.on("/schedule", HTTP_POST, []() {
    uint8_t days = 0;
    for (int d = 0; d < 7; ++d) if (server.hasArg("d" + String(d))) days |= 1 << d;
    cfg.days = days;
    cfg.evening.enabled = server.hasArg("een");
    cfg.evening.onMin = parseHHMM(server.arg("eon"), cfg.evening.onMin);
    cfg.evening.offMin = parseHHMM(server.arg("eoff"), cfg.evening.offMin);
    cfg.morning.enabled = server.hasArg("men");
    cfg.morning.onMin = parseHHMM(server.arg("mon"), cfg.morning.onMin);
    cfg.morning.offMin = parseHHMM(server.arg("moff"), cfg.morning.offMin);
    int z = server.arg("zone").toInt();
    if (z >= 0 && z < ZONE_COUNT) cfg.zone = (uint8_t)z;
    cfg.mode = MODE_SCHEDULE;
    saveSettings();
    applyTimeZone();
    Serial.println("[web] schedule saved");
    redirectHome();
  });

  server.on("/look", HTTP_POST, []() {
    auto clamp8 = [](long v) { return (int8_t)(v < -60 ? -60 : (v > 60 ? 60 : v)); };
    cfg.lx = clamp8(server.arg("lx").toInt());
    cfg.ly = clamp8(server.arg("ly").toInt());
    cfg.rx = clamp8(server.arg("rx").toInt());
    cfg.ry = clamp8(server.arg("ry").toInt());
    auto clampRot = [](long v) { return (int8_t)(v < -45 ? -45 : (v > 45 ? 45 : v)); };
    cfg.lrot = clampRot(server.arg("lrot").toInt());
    cfg.rrot = clampRot(server.arg("rrot").toInt());
    long tv = server.arg("travel").toInt();
    if (tv >= 10 && tv <= 55) cfg.travel = (uint8_t)tv;
    auto clampU = [](long v, long lo, long hi) { return (uint8_t)(v < lo ? lo : (v > hi ? hi : v)); };
    if (server.hasArg("bright")) cfg.bright = clampU(server.arg("bright").toInt(), 30, 150);
    if (server.hasArg("pale")) cfg.pale = clampU(server.arg("pale").toInt(), 0, 100);
    if (server.hasArg("glow")) cfg.glow = clampU(server.arg("glow").toInt(), 30, 200);
    cfg.rinv = server.hasArg("rinv");
    cfg.fancy = server.hasArg("fancy");
    if (server.hasArg("rscale")) cfg.rscale = server.arg("rscale").toInt() == 10 ? 10 : 1;
    if (server.hasArg("rfull")) cfg.rfull = clampU(server.arg("rfull").toInt(), 10, 60);
    cfg.movingOnly = server.hasArg("movonly");
    if (server.hasArg("spimhz")) {
      long m = server.arg("spimhz").toInt();
      cfg.spiMhz = (m == 80) ? 80 : (m == 60 ? 60 : 40);
    }
    if (server.hasArg("tresp")) cfg.trackResp = clampU(server.arg("tresp").toInt(), 1, 10);
    cfg.sleepMode = server.hasArg("sleepm");
    if (server.hasArg("wakeh")) cfg.wakeHold = clampU(server.arg("wakeh").toInt(), 2, 120);
    if (server.hasArg("still")) cfg.stillHold = clampU(server.arg("still").toInt(), 0, 60);
    cfg.rstatic = server.hasArg("rstatic");
    if (server.hasArg("rnear")) {
      long rn = server.arg("rnear").toInt();
      cfg.rnear = (uint16_t)(rn < 0 ? 0 : (rn > 3000 ? 3000 : rn));
    }
    if (server.hasArg("warm")) {
      long w = server.arg("warm").toInt();
      cfg.warm = (int8_t)(w < -50 ? -50 : (w > 50 ? 50 : w));
    }
    lookVersion++;
    if (server.hasArg("pupsh")) {
      long v = server.arg("pupsh").toInt();
      cfg.pupilShape = (uint8_t)(v < 0 ? 0 : (v > 2 ? 0 : v));
    }
    int st = server.arg("style").toInt();
    bool restart = (st >= 0 && st < styleCount && st != cfg.style);
    if (server.hasArg("lidset")) {
      long v = server.arg("lidset").toInt();
      const int8_t want = (int8_t)((v == 100) ? 100 : (v < 0 ? -1 : (v >= lidCount ? -1 : v)));
      if (want != cfg.lidSet) restart = true;
      cfg.lidSet = want;
    }
    if (st >= 0 && st < styleCount) cfg.style = (uint8_t)st;
    saveSettings();
    Serial.printf("[web] look saved (style %u, nudges %d/%d %d/%d, tilt %d/%d, travel %u)\n",
                  cfg.style, cfg.lx, cfg.ly, cfg.rx, cfg.ry, cfg.lrot, cfg.rrot, cfg.travel);
    if (restart) {
      server.send(200, "text/html",
                  "<meta name='viewport' content='width=device-width'><body style='font-family:sans-serif;background:#141210;color:#eee'>"
                  "<h2>New eye style loading&hellip;</h2><p>Back in about 10 seconds.</p></body>");
      delay(600);
      ESP.restart();
    }
    redirectHome();
  });

  server.on("/wifi", HTTP_POST, []() {
    cfg.ssid = server.arg("ssid");
    cfg.ssid.trim();
    if (server.arg("pass").length()) cfg.pass = server.arg("pass");
    String ap = server.arg("appass");
    if (ap.length() >= 8) cfg.hotspotPass = ap;
    saveSettings();
    server.send(200, "text/html",
                "<meta name='viewport' content='width=device-width'><body style='font-family:sans-serif;background:#141210;color:#eee'>"
                "<h2>Saved. Restarting&hellip;</h2><p>Reconnect in about 20 seconds at http://skull.local "
                "or on the SkullEyes hotspot at http://192.168.4.1</p></body>");
    delay(800);
    ESP.restart();
  });

  server.on("/settime", HTTP_POST, []() {
    long epoch = server.arg("epoch").toInt();
    if (epoch > 1700000000 && !clockValid()) {
      struct timeval tv = { (time_t)epoch, 0 };
      settimeofday(&tv, nullptr);
      Serial.println("[web] clock set from phone");
    }
    server.send(200, "text/plain", "ok");
  });

  server.on("/update", HTTP_POST,
    []() {
      bool ok = !Update.hasError();
      server.send(200, "text/html", ok
        ? "<body style='font-family:sans-serif;background:#141210;color:#eee'><h2>Update done. Restarting&hellip;</h2></body>"
        : "<body style='font-family:sans-serif;background:#141210;color:#eee'><h2>Update failed.</h2><p>The old firmware is still running.</p></body>");
      delay(800);
      if (ok) ESP.restart();
    },
    []() {
      HTTPUpload &up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[web] firmware upload: %s\n", up.filename.c_str());
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        Update.write(up.buf, up.currentSize);
      } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[web] firmware received, %u bytes\n", (unsigned)up.totalSize);
        else Serial.println("[web] firmware update failed");
      }
    });

  // Phones check a few odd addresses when joining a hotspot; send them all to
  // the settings page so it pops up by itself.
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  });
}

// ---------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------
static void startHotspot() {
  if (hotspotOn) return;
  WiFi.mode(cfg.ssid.isEmpty() ? WIFI_AP : WIFI_AP_STA);
  WiFi.softAP(HOTSPOT_NAME, cfg.hotspotPass.length() >= 8 ? cfg.hotspotPass.c_str() : nullptr);
  dns.start(53, "*", WiFi.softAPIP());
  hotspotOn = true;
  Serial.printf("[wifi] hotspot \"%s\" on - http://192.168.4.1 (password: %s)\n", HOTSPOT_NAME,
                cfg.hotspotPass.length() >= 8 ? cfg.hotspotPass.c_str() : "none");
}

static void stopHotspot() {
  if (!hotspotOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);          // radio naps between beacons: much lower power
  hotspotOn = false;
  Serial.println("[wifi] home Wi-Fi is up, hotspot off (saves power)");
}

void webSetup() {
  loadSettings();
  applyTimeZone();

  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);
  lastConnectedMs = millis();

  if (cfg.ssid.isEmpty()) {
    Serial.println("[wifi] no home network saved yet - use the hotspot to set one");
    startHotspot();
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    configTzTime(ZONES[cfg.zone].posix, "pool.ntp.org", "time.google.com");
    Serial.printf("[wifi] connecting to \"%s\"...\n", cfg.ssid.c_str());
  }

  setupRoutes();
  server.begin();
  mdnsStarted = MDNS.begin(HOSTNAME);
  if (mdnsStarted) MDNS.addService("http", "tcp", 80);
}

void webLoop() {
  if (hotspotOn) dns.processNextRequest();
  server.handleClient();

  const uint32_t now = millis();
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    wasConnected = connected;
    if (connected) {
      connectedSinceMs = now;
      Serial.printf("[wifi] connected to \"%s\" - settings page: http://%s.local or http://%s\n",
                    cfg.ssid.c_str(), HOSTNAME, WiFi.localIP().toString().c_str());
      if (!mdnsStarted) { mdnsStarted = MDNS.begin(HOSTNAME); if (mdnsStarted) MDNS.addService("http", "tcp", 80); }
    } else {
      Serial.println("[wifi] home network lost - retrying");
    }
  }
  if (connected) lastConnectedMs = now;

  // Hotspot only while home Wi-Fi is unavailable.
  if (!hotspotOn && !connected && now - lastConnectedMs > HOTSPOT_AFTER_MS) startHotspot();
  if (hotspotOn && connected && now - connectedSinceMs > HOTSPOT_OFF_AFTER_MS) stopHotspot();

  static bool clockAnnounced = false;
  if (!clockAnnounced && clockValid()) {
    clockAnnounced = true;
    Serial.printf("[clock] time is %s\n", nowText().c_str());
  }
}
