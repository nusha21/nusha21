#define FASTLED_ALLOW_INTERRUPTS 0
#define FASTLED_INTERNAL
#ifndef CORE_DEBUG_LEVEL
#define CORE_DEBUG_LEVEL 0
#endif
#ifndef ARDUINO_USB_CDC_ON_BOOT
#define ARDUINO_USB_CDC_ON_BOOT 0
#endif

#include <DNSServer.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

// BREETHR 694 - play once, replay from the web.
//
// - On boot the curved-wave animation plays ONCE, then the panel goes black.
// - Wi-Fi: tries the built-in network, then the one saved from the setup page.
//   Only if neither connects does the panel open its own hotspot
//   "Breethr ABCDE" (last 5 digits of its MAC) with a setup page.
// - Once online it asks the Vercel web app every few seconds whether the
//   PLAY button was pressed; every press restarts the animation once.
//
// The animation (shape, timing, colours) is the same as
// breethr_694_curved_wave_v3.ino.

// ================= USER SETTINGS =================
// Built-in Wi-Fi, tried first. Leave both "" to only use the setup page.
const char WIFI_SSID[] = "Breethr Tata 2.4 Ghz";
const char WIFI_PASSWORD[] = "breethr@321";

// Address of the Vercel web app (no "/" at the end).
const char PLAY_SERVER_URL[] = "https://breethr-play.vercel.app";

#define ENABLE_USB_SERIAL 1

// ================= LOCKED HARDWARE =================
#define LED_DATA_PIN 5
// BOOT button on most ESP32 boards. Hold it 5 s to forget the saved Wi-Fi
// and open the setup hotspot.
#define SETUP_BUTTON_PIN 0

#define LED_TYPE WS2812B
// WS2812B LEDs normally use GRB byte order. With RGB the red and green
// channels swap and Apricot/oranges show up GREEN. If colours look wrong,
// try the other value: GRB <-> RGB.
#define COLOR_ORDER GRB

// ================= 694-LED PANEL =================
static constexpr uint16_t NUM_LEDS = 694;
static constexpr uint8_t NUM_ROWS = 15;
static constexpr uint8_t GRID_X2 = 99; // Half-pitch x coordinate, 0..98.
static constexpr uint8_t BRIGHTNESS = 220;  // 0..255.
// LEDs are linear, screens are not: without this, pastel hex codes look
// washed-out / whitish on LEDs. 1.0 = off, 2.2 = like a screen, 2.8 = richer.
static constexpr float COLOR_GAMMA = 2.4f;
// WS2812B green LEDs are much brighter than red/blue, which gives oranges and
// yellows a green tint. This scales each channel (R, G, B); lower the middle
// byte for less green. 0xFFFFFF = no correction.
static constexpr uint32_t LED_CORRECTION = 0xFFB0F0;
static constexpr uint16_t MAX_MILLIAMPS = 3000;
static constexpr float TAU = 6.283185307179586f;

// Visual row order: TOP to BOTTOM.
static constexpr uint8_t ROW_LEN[NUM_ROWS] = {
  43, 44, 45, 46, 47, 48, 49, 50,
  49, 48, 47, 46, 45, 44, 43
};
static constexpr uint16_t ROW_START[NUM_ROWS] = {
    0,  43,  87, 132, 178, 225, 273, 322,
  372, 421, 469, 516, 562, 607, 651
};
static_assert(651 + 43 == NUM_LEDS, "The rows must total 694 LEDs");

// ================= TIMING =================
static constexpr uint16_t MAIN_FRAME_MS = 33;

// ================= ANIMATION SPEED / DELAYS (EDIT HERE) =================
// All values are in milliseconds (1000 = 1 second). Bigger = slower / longer.
//
// Wave movement speed (how fast the ribbons travel left -> right).
static constexpr uint16_t WAVE_A_PERIOD_MS = 9900;  // Lower = faster.
static constexpr uint16_t WAVE_B_PERIOD_MS = 12300; // Lower = faster.
//
// Every step below starts from a BLACK screen and the wave fills in from the
// left, except Lilac which fills in from the right over the outdoor orange.
//
// 1) Outdoor HIGH - orange (setting names still say APRICOT).
static constexpr uint32_t APRICOT_FILL_MS = 10000;   // Outdoor orange fills in from the left.
static constexpr uint32_t APRICOT_HOLD_MS = 0;       // Extra time on full outdoor orange.
//
// 2) Outdoor LOW - Lilac.
static constexpr uint32_t LILAC_FILL_MS = 10000;     // Lilac pushes in from the right.
static constexpr uint32_t LILAC_HOLD_MS = 10000;     // Hold full Lilac.
//
// 3) Black screen between outdoor and indoor.
static constexpr uint32_t BLACK_FADE_MS = 2000;      // Lilac fades out to black.
static constexpr uint32_t BLACK_HOLD_MS = 2000;      // Stay black.
//
// 4) Indoor: Temperature -> Humidity -> PM 2.5 -> CO2.
//    Temperature fills in from black; each next colour flows in over the last.
static constexpr uint32_t INDOOR_FILL_MS = 10000;    // Each colour fills in from the left.
static constexpr uint32_t INDOOR_HOLD_MS = 0;        // Extra time on each full colour.
//
// 5) Fade to black after CO2. The animation then stops until the next PLAY.
static constexpr uint32_t END_FADE_MS = 2000;        // CO2 fades out to black.
static constexpr uint32_t END_BLACK_MS = 2000;       // Stay black.
//
// Softness of the edge between two colours (0.05 = sharp, 0.25 = very soft).
static constexpr float COLOR_EDGE = 0.12f;

// ================= NETWORK TIMING =================
// How often the panel asks the web app whether PLAY was pressed. The web app
// caches the answer, so a short interval stays inside Vercel's free plan.
// Don't go below 3000 (free plan: 1 million requests per month).
static constexpr uint32_t PLAY_POLL_INTERVAL_MS = 3000UL;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000UL;
// Connection lost for this long -> open the setup hotspot.
static constexpr uint32_t WIFI_LOST_PORTAL_MS = 60000UL;
// While the hotspot is open (and nobody is using it), retry known Wi-Fi.
static constexpr uint32_t PORTAL_RETRY_INTERVAL_MS = 60000UL;
static constexpr uint32_t SETUP_BUTTON_HOLD_MS = 5000UL;
static constexpr uint16_t HTTP_CONNECT_TIMEOUT_MS = 5000;
static constexpr uint16_t HTTP_READ_TIMEOUT_MS = 5000;

CRGB leds[NUM_LEDS];
uint8_t gammaTable[256];

// ================= WAVE COLORS =================
static constexpr uint32_t COLOR_OUTDOOR_HIGH = 0xD68354; // Light orange (#D68354)
static constexpr uint32_t COLOR_OUTDOOR_LOW = 0xC4A3D6;  // Lilac

// Indoor colours fade from LEFT colour to RIGHT colour across the panel.
static constexpr uint8_t NUM_INDOOR = 4;
static constexpr uint32_t INDOOR_LEFT[NUM_INDOOR] = {
  0xA8CFDA, // Temperature: Sky
  0xD3ABC8, // Humidity:    Blush
  0xEDAB7A, // PM 2.5:      Apricot
  0xF5E8AE  // CO2:         Light yellow (#F5E8AE)
};
static constexpr uint32_t INDOOR_RIGHT[NUM_INDOOR] = {
  0xABA7EE, // Temperature: Periwinkle
  0xDDB0BE, // Humidity:    Blush
  0xEDAB7A, // PM 2.5:      Apricot
  0xF5E8AE  // CO2:         Light yellow (#F5E8AE)
};

static constexpr uint32_t INDOOR_PARAM_MS = INDOOR_FILL_MS + INDOOR_HOLD_MS;
static constexpr uint32_t ANIMATION_CYCLE_MS =
  APRICOT_FILL_MS + APRICOT_HOLD_MS +
  LILAC_FILL_MS + LILAC_HOLD_MS +
  BLACK_FADE_MS + BLACK_HOLD_MS +
  NUM_INDOOR * INDOOR_PARAM_MS +
  END_FADE_MS + END_BLACK_MS;

// A "paint" is a colour that may fade from left to right across the panel.
struct Paint {
  uint32_t left;
  uint32_t right;
};

// How the `top` paint replaces the `base` paint.
enum Motion : uint8_t {
  FROM_LEFT,  // edge travels left -> right
  FROM_RIGHT, // edge travels right -> left
  FADE        // whole panel cross-fades at once
};

// One frame of the colour sequence: `top` covers `base` as progress goes 0..1.
struct Scene {
  Paint base;
  Paint top;
  float progress;
  Motion motion;
};

float curveA[GRID_X2];
float curveB[GRID_X2];
uint32_t animationStartMs = 0;
uint32_t lastMainFrameMs = 0;
bool animationPlaying = false;

// Written by the network task (core 0), read by loop() (core 1).
// Each PLAY press adds one; loop() replays when it changes.
volatile uint32_t playRequestCount = 0;
uint32_t handledPlayCount = 0;

// ================= LED MAPPING =================
// colFromLeft is a visual coordinate. Data enters at the TOP RIGHT.
static int16_t rowColToIndex(int8_t row, int8_t colFromLeft) {
  if (row < 0 || row >= NUM_ROWS ||
      colFromLeft < 0 || colFromLeft >= ROW_LEN[row]) return -1;
  const uint8_t stripOffset = (row & 1)
                            ? colFromLeft
                            : ROW_LEN[row] - 1 - colFromLeft;
  return ROW_START[row] + stripOffset;
}

static void clearPanel() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
}

static CRGB colorFromHex(uint32_t hex) {
  return CRGB((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

static constexpr Paint BLACK_PAINT = {0x000000, 0x000000};
static constexpr Paint APRICOT_PAINT = {COLOR_OUTDOOR_HIGH, COLOR_OUTDOOR_HIGH};
static constexpr Paint LILAC_PAINT = {COLOR_OUTDOOR_LOW, COLOR_OUTDOOR_LOW};

static Paint indoorPaint(uint8_t i) {
  return {INDOOR_LEFT[i], INDOOR_RIGHT[i]};
}

static float clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float smoothStep(float edge0, float edge1, float x) {
  const float t = clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0f - 2.0f * t);
}

static CRGB mixColor(const CRGB &a, const CRGB &b, float k) {
  return CRGB(uint8_t(a.r + (b.r - a.r) * k + 0.5f),
              uint8_t(a.g + (b.g - a.g) * k + 0.5f),
              uint8_t(a.b + (b.b - a.b) * k + 0.5f));
}

static CRGB paintAt(const Paint &paint, float x) {
  return mixColor(colorFromHex(paint.left), colorFromHex(paint.right), x);
}

static float progressOf(uint32_t t, uint32_t duration) {
  return duration == 0 ? 1.0f : clamp01(float(t) / duration);
}

static Scene sceneAt(uint32_t now) {
  uint32_t t = (now - animationStartMs) % ANIMATION_CYCLE_MS;

  // 1) Outdoor orange fills in from the left, starting from black.
  if (t < APRICOT_FILL_MS + APRICOT_HOLD_MS) {
    return {BLACK_PAINT, APRICOT_PAINT, progressOf(t, APRICOT_FILL_MS), FROM_LEFT};
  }
  t -= APRICOT_FILL_MS + APRICOT_HOLD_MS;

  // 2) Lilac pushes in from the right over the outdoor orange, then holds.
  if (t < LILAC_FILL_MS + LILAC_HOLD_MS) {
    return {APRICOT_PAINT, LILAC_PAINT, progressOf(t, LILAC_FILL_MS), FROM_RIGHT};
  }
  t -= LILAC_FILL_MS + LILAC_HOLD_MS;

  // 3) Fade to a black screen.
  if (t < BLACK_FADE_MS + BLACK_HOLD_MS) {
    return {LILAC_PAINT, BLACK_PAINT, progressOf(t, BLACK_FADE_MS), FADE};
  }
  t -= BLACK_FADE_MS + BLACK_HOLD_MS;

  // 4) Indoor colours: Temperature fills in from black, then each next
  //    colour flows in from the left over the previous one.
  if (t < NUM_INDOOR * INDOOR_PARAM_MS) {
    const uint8_t i = t / INDOOR_PARAM_MS;
    const uint32_t tp = t % INDOOR_PARAM_MS;
    const Paint before = (i == 0) ? BLACK_PAINT : indoorPaint(i - 1);
    return {before, indoorPaint(i), progressOf(tp, INDOOR_FILL_MS), FROM_LEFT};
  }
  t -= NUM_INDOOR * INDOOR_PARAM_MS;

  // 5) Fade to black at the end.
  return {indoorPaint(NUM_INDOOR - 1), BLACK_PAINT, progressOf(t, END_FADE_MS), FADE};
}

// How much of the `top` paint covers position x (0 = left, 1 = right).
static float coverAt(const Scene &scene, float x) {
  if (scene.motion == FADE) return scene.progress;
  const float travel = 1.0f + 2.0f * COLOR_EDGE;
  if (scene.motion == FROM_RIGHT) {
    const float front = 1.0f + COLOR_EDGE - travel * scene.progress;
    return smoothStep(front - COLOR_EDGE, front + COLOR_EDGE, x);
  }
  const float front = -COLOR_EDGE + travel * scene.progress;
  return 1.0f - smoothStep(front - COLOR_EDGE, front + COLOR_EDGE, x);
}

static void updateCurves(uint32_t now) {
  const float timeA = float(now % WAVE_A_PERIOD_MS) / WAVE_A_PERIOD_MS;
  const float timeB = float(now % WAVE_B_PERIOD_MS) / WAVE_B_PERIOD_MS;
  for (uint8_t x2 = 0; x2 < GRID_X2; ++x2) {
    const float x = float(x2) / (GRID_X2 - 1);
    const float phaseA = TAU * (1.12f * x - timeA);
    const float phaseB = TAU * (0.96f * x - timeB) + 1.9f;
    curveA[x2] = 0.5f + 0.23f * sinf(phaseA)
                       + 0.035f * sinf(2.0f * phaseA + 0.7f);
    curveB[x2] = 0.5f + 0.19f * sinf(phaseB);
  }
}

static float softBand(float distance, float width) {
  const float z = distance / width;
  if (z < -4.5f || z > 4.5f) return 0.0f;
  return expf(-0.5f * z * z);
}

static void renderCurvedWave(uint32_t now) {
  updateCurves(now);
  clearPanel();
  const Scene scene = sceneAt(now);

  for (uint8_t row = 0; row < NUM_ROWS; ++row) {
    const float y = float(row) / (NUM_ROWS - 1);
    const uint8_t leftX2 = 50 - ROW_LEN[row];
    for (uint8_t col = 0; col < ROW_LEN[row]; ++col) {
      const uint8_t x2 = leftX2 + 2 * col;
      const float x = float(x2) / (GRID_X2 - 1);
      const float ribbonA = softBand(y - curveA[x2], 0.088f);
      const float ribbonB = softBand(y - curveB[x2], 0.10f);
      const float halo = softBand(y - curveA[x2], 0.18f);
      float light = 0.66f * ribbonA + 0.48f * ribbonB + 0.08f * halo;
      if (light > 1.0f) light = 1.0f;

      // LEDs outside the two ribbons remain exactly RGB(0,0,0).
      if (light < 0.025f) continue;
      CRGB color = mixColor(paintAt(scene.base, x), paintAt(scene.top, x),
                            coverAt(scene, x));
      color = CRGB(gammaTable[color.r], gammaTable[color.g], gammaTable[color.b]);
      color.nscale8(uint8_t(light * 255.0f + 0.5f));
      leds[rowColToIndex(row, col)] = color;
    }
  }
  FastLED.show();
}

// Starts the animation from the beginning. It plays once, then goes black.
static void startAnimation(uint32_t now) {
  animationStartMs = now;
  animationPlaying = true;
  lastMainFrameMs = now - MAIN_FRAME_MS; // draw the first frame right away
}

static void updateAnimation(uint32_t now) {
  if (!animationPlaying || now - lastMainFrameMs < MAIN_FRAME_MS) return;
  lastMainFrameMs = now;
  if (now - animationStartMs >= ANIMATION_CYCLE_MS) {
    animationPlaying = false;
    clearPanel();
    FastLED.show();
    return;
  }
  renderCurvedWave(now);
}

// =====================================================================
// ================= NETWORK (runs on its own task, core 0) =============
// =====================================================================

#if ENABLE_USB_SERIAL
#define LOG(...) Serial.printf(__VA_ARGS__)
#else
#define LOG(...) do {} while (0)
#endif

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

Preferences wifiPrefs;
WebServer portalServer(80);
DNSServer dnsServer;
WiFiClientSecure tlsClient;
HTTPClient playHttp;

char hotspotName[20];  // "Breethr ABCDE"
String savedSsid;
String savedPassword;

bool portalActive = false;
uint32_t portalRetryMs = 0;
uint32_t wifiLostSinceMs = 0;
uint32_t lastPlayPollMs = 0;
bool havePlayBaseline = false;
long long lastPlaySeq = 0;

// Set by the setup page, handled by the network loop.
bool connectRequested = false;
String requestedSsid;
String requestedPassword;

enum PortalState : uint8_t { PORTAL_IDLE, PORTAL_CONNECTING, PORTAL_CONNECTED, PORTAL_FAILED };
volatile PortalState portalState = PORTAL_IDLE;

// Scan results for the setup page.
String scanJson = "[]";
uint32_t scanDoneMs = 0;
bool scanRunning = false;

// Breethr logomark, also used on the web app.
static const char LOGO_SVG[] PROGMEM = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 430 728" fill="#073D4D" aria-label="Breethr"><g transform="translate(0,728) scale(0.1,-0.1)"><path d="M1420 7269 c-178 -15 -339 -71 -490 -170 -488 -320 -798 -1057 -895 -2129 -19 -206 -31 -749 -20 -904 l7 -109 69 133 c227 437 620 877 1114 1251 621 470 1358 779 1857 779 364 0 644 -184 738 -485 31 -99 31 -257 0 -380 -83 -331 -304 -681 -644 -1021 -134 -133 -299 -278 -333 -291 -6 -3 -51 18 -98 47 -333 196 -692 330 -1051 392 -170 29 -524 32 -676 5 -381 -67 -602 -237 -680 -526 -28 -101 -30 -315 -5 -411 81 -305 315 -485 712 -547 137 -21 423 -21 584 1 390 53 777 192 1157 416 l52 30 83 -67 c110 -88 321 -295 419 -408 388 -452 559 -892 474 -1219 -60 -229 -254 -412 -514 -483 -101 -28 -380 -25 -519 6 -940 203 -2219 1184 -2691 2063 l-48 90 -7 -97 c-10 -128 0 -756 14 -910 119 -1299 551 -2109 1215 -2279 257 -67 510 -36 712 85 176 106 356 320 465 554 20 41 36 64 44 61 35 -13 252 -56 337 -67 142 -17 438 -7 553 20 373 84 639 272 794 562 193 358 170 834 -63 1324 -170 356 -470 736 -784 989 -40 33 -71 62 -70 66 2 4 47 47 102 95 820 729 1151 1583 857 2211 -123 262 -347 468 -624 572 -174 65 -246 76 -492 76 -229 1 -329 -11 -535 -64 -41 -10 -76 -17 -78 -15 -3 2 -19 41 -38 86 -61 150 -187 328 -305 430 -218 191 -423 260 -699 238z m284 -443 c108 -53 221 -177 296 -326 25 -49 29 -65 19 -69 -35 -11 -386 -195 -489 -256 -341 -203 -634 -426 -867 -663 -59 -59 -103 -97 -103 -88 0 28 39 195 77 326 90 317 221 601 359 782 109 141 257 258 377 297 93 30 95 30 188 27 71 -2 96 -7 143 -30z m-88 -2925 c201 -37 417 -107 636 -207 48 -21 86 -43 85 -49 -5 -14 -278 -129 -417 -174 -376 -125 -793 -139 -1005 -34 -117 58 -168 149 -145 259 25 119 150 198 358 224 108 14 363 4 488 -19z m-772 -2260 c274 -235 600 -448 969 -634 97 -49 177 -93 177 -98 0 -6 -12 -33 -26 -62 -102 -201 -261 -339 -409 -353 -125 -12 -292 46 -415 145 -242 194 -451 596 -574 1104 -58 239 -65 228 60 102 60 -61 158 -153 218 -204z"/></g></svg>)SVG";

static const char PORTAL_HTML_HEAD[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Breethr Wi-Fi setup</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#FDFAF7;color:#141414;font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
main{max-width:420px;margin:0 auto;padding:32px 20px 40px}
.logo{width:44px;margin:0 auto 18px}
.logo svg{display:block;width:100%;height:auto}
h1{font-size:22px;font-weight:600;text-align:center;margin:0 0 4px;color:#073D4D}
.sub{text-align:center;color:#5c5c5c;margin:0 0 24px;font-size:14px}
.card{background:#fff;border:1px solid #ece6dc;border-radius:16px;padding:18px}
label{display:block;font-size:13px;font-weight:600;margin:0 0 6px;color:#073D4D}
input{width:100%;font:inherit;padding:12px 14px;border:1px solid #d9d2c6;border-radius:10px;background:#FDFAF7;color:#141414}
input:focus{outline:2px solid #5497A7;outline-offset:1px;border-color:#5497A7}
.row{margin-bottom:14px}
.nets{list-style:none;margin:0 0 16px;padding:0;max-height:220px;overflow:auto;border:1px solid #ece6dc;border-radius:10px}
.nets li{display:flex;justify-content:space-between;gap:8px;padding:11px 14px;border-bottom:1px solid #f1ece4;cursor:pointer}
.nets li:last-child{border-bottom:0}
.nets li:hover,.nets li.sel{background:#F9F5EE}
.nets .ssid{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.nets .meta{color:#7a7a7a;font-size:13px;white-space:nowrap}
.muted{color:#7a7a7a;font-size:14px;padding:11px 14px}
.pw{position:relative}
.pw button{position:absolute;right:8px;top:50%;transform:translateY(-50%);border:0;background:none;color:#5497A7;font:inherit;font-size:13px;cursor:pointer;padding:6px}
.go{width:100%;border:0;border-radius:999px;background:#073D4D;color:#FDFAF7;font:inherit;font-weight:600;padding:14px;cursor:pointer;margin-top:4px}
.go:disabled{opacity:.55;cursor:default}
.link{background:none;border:0;color:#5497A7;font:inherit;font-size:14px;cursor:pointer;padding:0;margin:0 0 12px}
.msg{margin-top:16px;padding:12px 14px;border-radius:10px;font-size:14px;display:none}
.msg.show{display:block}
.msg.info{background:#D0F1F8;color:#073D4D}
.msg.ok{background:#e4f1d6;color:#254835}
.msg.err{background:#f8e1d2;color:#7a3a10}
.foot{text-align:center;color:#9a9a9a;font-size:12px;margin-top:20px}
</style></head><body><main>
)HTML";

static const char PORTAL_HTML_BODY[] PROGMEM = R"HTML(
<h1>Connect your Breethr</h1>
<p class="sub">Choose the Wi-Fi network this panel should use.</p>
<div class="card">
<form id="f" autocomplete="off">
<label>Nearby networks</label>
<ul class="nets" id="nets"><li class="muted">Searching&hellip;</li></ul>
<button type="button" class="link" id="rescan">Search again</button>
<div class="row"><label for="ssid">Network name</label>
<input id="ssid" name="ssid" maxlength="32" required autocapitalize="none" spellcheck="false"></div>
<div class="row"><label for="pass">Password</label>
<div class="pw"><input id="pass" name="pass" type="password" maxlength="64">
<button type="button" id="show">Show</button></div></div>
<button class="go" id="go" type="submit">Connect</button>
</form>
<div class="msg" id="msg"></div>
</div>
<p class="foot" id="foot"></p>
</main>
<script>
var $=function(i){return document.getElementById(i)};
function esc(s){return s.replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function msg(t,c){var m=$('msg');m.className='msg show '+c;m.textContent=t}
function bars(r){return r>-60?'Strong':r>-72?'Good':'Weak'}
function scan(){
 fetch('/scan').then(function(r){return r.json()}).then(function(d){
  if(d.scanning){setTimeout(scan,1500);return}
  var ul=$('nets');
  if(!d.networks.length){ul.innerHTML='<li class="muted">No networks found</li>';return}
  ul.innerHTML='';
  d.networks.forEach(function(n){
   var li=document.createElement('li');
   li.innerHTML='<span class="ssid">'+esc(n.ssid)+'</span><span class="meta">'+bars(n.rssi)+(n.lock?' &#128274;':'')+'</span>';
   li.onclick=function(){
    [].forEach.call(ul.children,function(c){c.classList.remove('sel')});
    li.classList.add('sel');$('ssid').value=n.ssid;$('pass').value='';
    if(n.lock)$('pass').focus();
   };
   ul.appendChild(li);
  });
 }).catch(function(){setTimeout(scan,2000)});
}
$('rescan').onclick=function(){$('nets').innerHTML='<li class="muted">Searching&hellip;</li>';fetch('/scan?fresh=1').then(function(){setTimeout(scan,1500)})};
$('show').onclick=function(){var p=$('pass');p.type=p.type==='password'?'text':'password';this.textContent=p.type==='password'?'Show':'Hide'};
var fails=0,ssidSent='';
function poll(){
 fetch('/status').then(function(r){return r.json()}).then(function(s){
  fails=0;
  if(s.state==='connecting'){setTimeout(poll,1000);return}
  if(s.state==='connected'){
   msg('Connected to '+ssidSent+'. The panel is online and this hotspot will now close. You can close this page.','ok');return}
  msg('Could not connect to '+ssidSent+'. Check the password and try again.','err');$('go').disabled=false;
 }).catch(function(){
  if(++fails>8){msg('The hotspot closed. If the panel connected, it is online now. If this hotspot appears again, the connection failed. Join it and try again.','info');return}
  setTimeout(poll,1500);
 });
}
$('f').onsubmit=function(e){
 e.preventDefault();
 ssidSent=$('ssid').value.trim();if(!ssidSent)return;
 $('go').disabled=true;msg('Connecting to '+ssidSent+'… this takes up to 20 seconds.','info');
 var b=new URLSearchParams();b.append('ssid',ssidSent);b.append('pass',$('pass').value);
 fetch('/connect',{method:'POST',body:b}).then(function(){setTimeout(poll,1500)})
 .catch(function(){msg('Lost contact with the panel. Rejoin its hotspot and try again.','err');$('go').disabled=false});
};
scan();
</script></body></html>
)HTML";

// Last 5 hex digits of the Wi-Fi MAC, e.g. 24:6F:28:AB:CD:EF -> "BCDEF".
static void makeHotspotName() {
  const uint64_t efuse = ESP.getEfuseMac(); // byte 0 is the first MAC byte
  uint8_t mac[6];
  for (uint8_t i = 0; i < 6; ++i) mac[i] = (efuse >> (8 * i)) & 0xFF;
  char hex[13];
  snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(hotspotName, sizeof(hotspotName), "Breethr %s", hex + 7);
}

static void appendJsonString(String &out, const String &s) {
  out += '"';
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
      out += buf;
    } else {
      out += c;
    }
  }
  out += '"';
}

static void loadSavedWifi() {
  savedSsid = wifiPrefs.getString("ssid", "");
  savedPassword = wifiPrefs.getString("pass", "");
}

static void saveWifi(const String &ssid, const String &password) {
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", password);
  savedSsid = ssid;
  savedPassword = password;
}

// ---------- Setup page (captive portal) ----------
static void servicePortal() {
  if (!portalActive) return;
  dnsServer.processNextRequest();
  portalServer.handleClient();

  if (scanRunning) {
    const int16_t n = WiFi.scanComplete();
    if (n >= 0) {
      String json = "[";
      int added = 0;
      for (int16_t i = 0; i < n && added < 25; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        bool duplicate = false;
        for (int16_t j = 0; j < i; ++j) {
          if (WiFi.SSID(j) == ssid) { duplicate = true; break; }
        }
        if (duplicate) continue;
        if (added++) json += ',';
        json += "{\"ssid\":";
        appendJsonString(json, ssid);
        json += ",\"rssi\":";
        json += WiFi.RSSI(i);
        json += ",\"lock\":";
        json += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "false" : "true";
        json += '}';
      }
      json += ']';
      scanJson = json;
      WiFi.scanDelete();
      scanRunning = false;
      scanDoneMs = millis();
    } else if (n == WIFI_SCAN_FAILED) {
      scanRunning = false;
    }
  }
}

static void startScan() {
  if (scanRunning) return;
  WiFi.scanDelete();
  scanRunning = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING;
}

static void handlePortalRoot() {
  String page;
  page.reserve(9000);
  page += FPSTR(PORTAL_HTML_HEAD);
  page += "<div class=\"logo\">";
  page += FPSTR(LOGO_SVG);
  page += "</div>";
  page += FPSTR(PORTAL_HTML_BODY);
  page.replace("<p class=\"foot\" id=\"foot\"></p>",
               String("<p class=\"foot\">") + hotspotName + "</p>");
  portalServer.sendHeader("Cache-Control", "no-store");
  portalServer.send(200, "text/html; charset=utf-8", page);
}

static void handlePortalScan() {
  const bool stale = scanDoneMs == 0 || millis() - scanDoneMs > 20000UL;
  if (portalServer.hasArg("fresh") || stale) startScan();
  String json;
  if (scanRunning && (stale || portalServer.hasArg("fresh"))) {
    json = "{\"scanning\":true}";
  } else {
    json = "{\"scanning\":false,\"networks\":" + scanJson + "}";
  }
  portalServer.sendHeader("Cache-Control", "no-store");
  portalServer.send(200, "application/json", json);
}

static void handlePortalConnect() {
  const String ssid = portalServer.arg("ssid");
  if (ssid.length() == 0 || ssid.length() > 32 ||
      portalServer.arg("pass").length() > 64) {
    portalServer.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  requestedSsid = ssid;
  requestedPassword = portalServer.arg("pass");
  connectRequested = true;
  portalState = PORTAL_CONNECTING;
  portalServer.send(200, "application/json", "{\"ok\":true}");
}

static void handlePortalStatus() {
  const char *state = "idle";
  switch (portalState) {
    case PORTAL_CONNECTING: state = "connecting"; break;
    case PORTAL_CONNECTED:  state = "connected"; break;
    case PORTAL_FAILED:     state = "failed"; break;
    default: break;
  }
  String json = String("{\"state\":\"") + state + "\"}";
  portalServer.sendHeader("Cache-Control", "no-store");
  portalServer.send(200, "application/json", json);
}

// Any other address (phone "is there internet?" checks included) is sent to
// the setup page, which makes phones pop it up automatically.
static void handlePortalRedirect() {
  portalServer.sendHeader("Location", "http://192.168.4.1/", true);
  portalServer.send(302, "text/plain", "");
}

static void startPortal() {
  if (portalActive) return;
  LOG("[wifi] Opening setup hotspot \"%s\"\n", hotspotName);
  // Stop background reconnects so the radio stays on the hotspot channel.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
  WiFi.softAP(hotspotName);
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", AP_IP);
  static bool routesAdded = false;
  if (!routesAdded) {
    portalServer.on("/", HTTP_GET, handlePortalRoot);
    portalServer.on("/scan", HTTP_GET, handlePortalScan);
    portalServer.on("/connect", HTTP_POST, handlePortalConnect);
    portalServer.on("/status", HTTP_GET, handlePortalStatus);
    portalServer.onNotFound(handlePortalRedirect);
    routesAdded = true;
  }
  portalServer.begin();
  portalActive = true;
  portalState = PORTAL_IDLE;
  portalRetryMs = millis();
  scanDoneMs = 0;
  startScan();
}

static void stopPortal() {
  if (!portalActive) return;
  LOG("[wifi] Closing setup hotspot\n");
  portalServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  portalActive = false;
}

// Waits for a Wi-Fi connection while keeping the setup page responsive.
static bool tryConnect(const String &ssid, const String &password) {
  if (ssid.length() == 0) return false;
  LOG("[wifi] Connecting to \"%s\"...\n", ssid.c_str());
  if (scanRunning) {
    WiFi.scanDelete();
    scanRunning = false;
  }
  WiFi.disconnect(false, false);
  vTaskDelay(pdMS_TO_TICKS(100));
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t start = millis();
  while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      LOG("[wifi] Connected, IP %s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    servicePortal();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  LOG("[wifi] Could not connect to \"%s\"\n", ssid.c_str());
  WiFi.disconnect(false, false);
  return false;
}

static bool ssidVisible(const String &ssid, int16_t count) {
  for (int16_t i = 0; i < count; ++i) {
    if (WiFi.SSID(i) == ssid) return true;
  }
  return false;
}

// Built-in network first, then the one saved from the setup page.
// Networks that are not in range are skipped, unless none of them are
// (a hidden network does not show up in a scan).
static bool connectKnownWifi() {
  const String builtInSsid = WIFI_SSID;
  const String builtInPassword = WIFI_PASSWORD;
  const bool haveBuiltIn = builtInSsid.length() > 0;
  const bool haveSaved = savedSsid.length() > 0 && savedSsid != builtInSsid;
  if (!haveBuiltIn && !haveSaved) return false;

  if (scanRunning) {
    WiFi.scanDelete();
    scanRunning = false;
  }
  const int16_t count = WiFi.scanNetworks(false);
  const bool builtInSeen = haveBuiltIn && ssidVisible(builtInSsid, count);
  const bool savedSeen = haveSaved && ssidVisible(savedSsid, count);
  WiFi.scanDelete();
  const bool tryAll = !builtInSeen && !savedSeen;

  if (haveBuiltIn && (builtInSeen || tryAll) &&
      tryConnect(builtInSsid, builtInPassword)) return true;
  if (haveSaved && (savedSeen || tryAll) &&
      tryConnect(savedSsid, savedPassword)) return true;
  return false;
}

// ---------- Talking to the web app ----------
// GET /api/play returns {"seq":N}. N goes up by one on every PLAY press.
static void pollPlayServer() {
  char url[200];
  snprintf(url, sizeof(url), "%s/api/play", PLAY_SERVER_URL);
  playHttp.setReuse(true);
  playHttp.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  playHttp.setTimeout(HTTP_READ_TIMEOUT_MS);
  if (!playHttp.begin(tlsClient, url)) return;
  const int code = playHttp.GET();
  if (code == HTTP_CODE_OK) {
    const String body = playHttp.getString();
    const int key = body.indexOf("\"seq\"");
    const int colon = key >= 0 ? body.indexOf(':', key) : -1;
    if (colon >= 0) {
      const long long seq = strtoll(body.c_str() + colon + 1, nullptr, 10);
      if (!havePlayBaseline) {
        // The boot animation already plays; just remember where we are.
        havePlayBaseline = true;
        lastPlaySeq = seq;
        LOG("[play] Web app reachable (seq %lld)\n", seq);
      } else if (seq != lastPlaySeq) {
        lastPlaySeq = seq;
        LOG("[play] PLAY pressed (seq %lld)\n", seq);
        playRequestCount = playRequestCount + 1;
      }
    }
  } else {
    LOG("[play] Web app answered %d\n", code);
  }
  playHttp.end();
}

// Hold BOOT for 5 s: forget the saved Wi-Fi and open the setup hotspot.
static bool setupButtonHeld() {
  static uint32_t pressedSinceMs = 0;
  static bool fired = false;
  if (digitalRead(SETUP_BUTTON_PIN) == LOW) {
    if (pressedSinceMs == 0) pressedSinceMs = millis();
    if (!fired && millis() - pressedSinceMs >= SETUP_BUTTON_HOLD_MS) {
      fired = true;
      return true;
    }
  } else {
    pressedSinceMs = 0;
    fired = false;
  }
  return false;
}

static void networkTask(void *) {
  wifiPrefs.begin("breethr-wifi", false);
  loadSavedWifi();
  tlsClient.setInsecure();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  LOG("[wifi] Hotspot name if setup is needed: \"%s\"\n", hotspotName);

  if (!connectKnownWifi()) startPortal();

  for (;;) {
    const uint32_t now = millis();

    if (setupButtonHeld()) {
      LOG("[wifi] BOOT held: forgetting saved Wi-Fi\n");
      wifiPrefs.remove("ssid");
      wifiPrefs.remove("pass");
      loadSavedWifi();
      havePlayBaseline = false;
      startPortal();
    }

    if (portalActive) {
      servicePortal();

      if (connectRequested) {
        connectRequested = false;
        const String ssid = requestedSsid;
        const String password = requestedPassword;
        if (tryConnect(ssid, password)) {
          saveWifi(ssid, password);
          portalState = PORTAL_CONNECTED;
          // Give the phone a few seconds to show "Connected".
          const uint32_t shownAt = millis();
          while (millis() - shownAt < 5000UL) {
            servicePortal();
            vTaskDelay(pdMS_TO_TICKS(20));
          }
          stopPortal();
          wifiLostSinceMs = 0;
        } else {
          portalState = PORTAL_FAILED;
        }
      } else if (now - portalRetryMs >= PORTAL_RETRY_INTERVAL_MS &&
                 WiFi.softAPgetStationNum() == 0) {
        // Nobody is on the setup page: check if a known network is back.
        portalRetryMs = now;
        if (connectKnownWifi()) {
          stopPortal();
          wifiLostSinceMs = 0;
        }
        portalRetryMs = millis();
      }
    } else if (WiFi.status() == WL_CONNECTED) {
      wifiLostSinceMs = 0;
      if (now - lastPlayPollMs >= PLAY_POLL_INTERVAL_MS) {
        lastPlayPollMs = now;
        pollPlayServer();
      }
    } else {
      // Lost Wi-Fi: let it reconnect on its own for a while, then open setup.
      if (wifiLostSinceMs == 0) {
        wifiLostSinceMs = now;
        LOG("[wifi] Connection lost, retrying...\n");
      } else if (now - wifiLostSinceMs >= WIFI_LOST_PORTAL_MS) {
        startPortal();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(portalActive ? 5 : 20));
  }
}

// ================= SETUP / LOOP =================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

#if ENABLE_USB_SERIAL
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" BREETHR 694 - play once, replay from web");
  Serial.println("========================================");
#endif

  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);

  FastLED.addLeds<LED_TYPE, LED_DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  for (uint16_t i = 0; i < 256; ++i) {
    gammaTable[i] = uint8_t(powf(i / 255.0f, COLOR_GAMMA) * 255.0f + 0.5f);
  }
  // Tone down the over-bright green channel (see LED_CORRECTION).
  FastLED.setCorrection(CRGB(LED_CORRECTION));
  FastLED.setDither(false);
  clearPanel();
  FastLED.show();

  makeHotspotName();

  // Wi-Fi, setup page and web-app checks run on core 0 so the animation on
  // core 1 never stutters.
  xTaskCreatePinnedToCore(networkTask, "network", 12288, nullptr, 1, nullptr, 0);

  // Boot: play the animation once.
  startAnimation(millis());
}

void loop() {
  const uint32_t now = millis();

  const uint32_t requests = playRequestCount;
  if (requests != handledPlayCount) {
    handledPlayCount = requests;
    startAnimation(now);
  }

  updateAnimation(now);
  delay(1);
}
