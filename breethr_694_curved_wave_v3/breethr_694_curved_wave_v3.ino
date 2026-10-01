#define FASTLED_ALLOW_INTERRUPTS 0
#define FASTLED_INTERNAL
#ifndef CORE_DEBUG_LEVEL
#define CORE_DEBUG_LEVEL 0
#endif
#ifndef ARDUINO_USB_CDC_ON_BOOT
#define ARDUINO_USB_CDC_ON_BOOT 0
#endif

#include <ArduinoJson.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

// BREETHR network code is retained from breethr_288_final.ino.
// The SCD4x (CO2) and SPS30 (dust) sensor code has been removed.
// The 694-LED curved wave shape is unchanged; only its colour sequence is new.

// ================= USER SETTINGS =================
const char WIFI_SSID[] = "Breethr Tata 2.4 Ghz";
const char WIFI_PASSWORD[] = "breethr@321";
const char SERVER_BASE_URL[] = "https://project-57vve.vercel.app";
const char DEVICE_ID[] = "breethr-esp32";

#define ENABLE_USB_SERIAL 1

// ================= LOCKED HARDWARE =================
#define LED_DATA_PIN 5
#define RELAY_PIN 32

#define LED_TYPE WS2812B
// WS2812B LEDs normally use GRB byte order. With RGB the red and green
// channels swap and Apricot/oranges show up GREEN. If colours look wrong,
// try the other value: GRB <-> RGB.
#define COLOR_ORDER GRB

// ================= ORIGINAL SENSOR LIMITS =================
#define Lower_Limit_CO2_SCD      400
#define Upper_Limit_CO2          5000
#define Upper_Limit_Temperature  50
#define Upper_Limit_Humidity     95
#define Upper_Limit_PM           999

// ================= ORIGINAL GOOD/BAD CUT-OFFS =================
// These are copied exactly from airSignature() in breethr_288_final.ino.
// At or below the number is GOOD; above it is BAD.
static constexpr uint16_t GOOD_PM25_MAX = 30;       // ug/m3
static constexpr uint16_t GOOD_CO2_MAX = 1000;      // ppm
static constexpr int16_t GOOD_TEMPERATURE_MAX = 25; // deg C
static constexpr uint16_t GOOD_HUMIDITY_MAX = 65;   // %RH

// ================= 694-LED PANEL =================
static constexpr uint16_t NUM_LEDS = 694;
static constexpr uint8_t NUM_ROWS = 15;
static constexpr uint8_t GRID_X2 = 99; // Half-pitch x coordinate, 0..98.
static constexpr uint8_t BRIGHTNESS = 220;  // 0..255. Was 96 (too dull).
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
// 1) Outdoor HIGH - Dark orange (setting names still say APRICOT).
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
// 5) Black screen after CO2, before outdoor orange starts again.
static constexpr uint32_t END_FADE_MS = 2000;        // CO2 fades out to black.
static constexpr uint32_t END_BLACK_MS = 2000;       // Stay black.
//
// Softness of the edge between two colours (0.05 = sharp, 0.25 = very soft).
static constexpr float COLOR_EDGE = 0.12f;
static constexpr uint32_t POST_INTERVAL_MS = 10000UL;
static constexpr uint32_t SERVER_POLL_INTERVAL_MS = 10000UL;
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000UL;
static constexpr uint32_t PERSIST_INTERVAL_MS = 120000UL;
static constexpr uint16_t HTTP_CONNECT_TIMEOUT_MS = 5000;
static constexpr uint16_t HTTP_READ_TIMEOUT_MS = 5000;

CRGB leds[NUM_LEDS];
uint8_t gammaTable[256];
Preferences preferences;
WiFiClientSecure secureClient;

struct __attribute__((packed)) AirValues {
  uint16_t pm25;
  uint16_t pm10;
  uint16_t co2;
  int16_t temperature;
  uint16_t humidity;
};

AirValues current = {17, 18, Lower_Limit_CO2_SCD, 25, 52};
AirValues outdoor = {76, 120, 550, 28, 68};

bool freshAirOn = false;
bool coolingOn = false;
bool persistentStateDirty = false;
int16_t outdoorAqi = -1;

uint32_t lastMainFrameMs = 0;
uint32_t lastPostMs = 0;
uint32_t lastServerPollMs = 0;
uint32_t lastWifiRetryMs = 0;
uint32_t lastPersistentSaveMs = 0;

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

// ================= PERSISTED FALLBACK STATE =================
// Layout is kept compatible with the original saved state.
struct __attribute__((packed)) StoredState {
  uint32_t magic;
  AirValues indoor;
  AirValues outdoor;
  int16_t outdoorAqi;
  uint8_t flags;
  uint8_t cleanSignature;
};

static constexpr uint32_t STORED_STATE_MAGIC = 0x42525448UL;

static uint16_t clampU16(long value, uint16_t minValue, uint16_t maxValue) {
  if (value < (long)minValue) return minValue;
  if (value > (long)maxValue) return maxValue;
  return (uint16_t)value;
}

static int16_t clampS16(long value, int16_t minValue, int16_t maxValue) {
  if (value < (long)minValue) return minValue;
  if (value > (long)maxValue) return maxValue;
  return (int16_t)value;
}

static uint16_t roundedClampedU16(float value, uint16_t minValue, uint16_t maxValue) {
  return clampU16((long)round(value), minValue, maxValue);
}

static int16_t roundedClampedS16(float value, int16_t minValue, int16_t maxValue) {
  return clampS16((long)round(value), minValue, maxValue);
}

static void sanitizeIndoor(AirValues *air) {
  air->pm25 = clampU16(air->pm25, 0, Upper_Limit_PM);
  air->pm10 = clampU16(air->pm10, 0, Upper_Limit_PM);
  air->co2 = clampU16(air->co2, Lower_Limit_CO2_SCD, Upper_Limit_CO2);
  air->temperature = clampS16(air->temperature, -40, Upper_Limit_Temperature);
  air->humidity = clampU16(air->humidity, 0, Upper_Limit_Humidity);
}

static void sanitizeOutdoor(AirValues *air) {
  air->pm25 = clampU16(air->pm25, 0, Upper_Limit_PM);
  air->pm10 = clampU16(air->pm10, 0, Upper_Limit_PM);
  air->co2 = 550;
  air->temperature = clampS16(air->temperature, -40, Upper_Limit_Temperature);
  air->humidity = clampU16(air->humidity, 0, Upper_Limit_Humidity);
}

static bool airValuesDiffer(const AirValues &a, const AirValues &b) {
  return a.pm25 != b.pm25 || a.pm10 != b.pm10 || a.co2 != b.co2 ||
         a.temperature != b.temperature || a.humidity != b.humidity;
}

static void requestPersistentSave() {
  persistentStateDirty = true;
}

static uint8_t airSignature(const AirValues &air) {
  uint8_t signature = 0;
  if (air.pm25 > GOOD_PM25_MAX) signature |= 0x01;
  if (air.co2 > GOOD_CO2_MAX) signature |= 0x02;
  if (air.temperature > GOOD_TEMPERATURE_MAX) signature |= 0x04;
  if (air.humidity > GOOD_HUMIDITY_MAX) signature |= 0x08;
  return signature;
}

static void loadPersistentState() {
  StoredState stored;
  const size_t readBytes = preferences.getBytes("state", &stored, sizeof(stored));
  if (readBytes != sizeof(stored) || stored.magic != STORED_STATE_MAGIC) return;

  current = stored.indoor;
  outdoor = stored.outdoor;
  sanitizeIndoor(&current);
  sanitizeOutdoor(&outdoor);
  outdoorAqi = stored.outdoorAqi;
  freshAirOn = (stored.flags & 0x01) != 0;
  coolingOn = (stored.flags & 0x02) != 0;
}

static void savePersistentState(bool force = false) {
  const uint32_t now = millis();
  if (!persistentStateDirty) return;
  if (!force && lastPersistentSaveMs != 0 &&
      now - lastPersistentSaveMs < PERSIST_INTERVAL_MS) return;

  StoredState stored;
  stored.magic = STORED_STATE_MAGIC;
  stored.indoor = current;
  stored.outdoor = outdoor;
  stored.outdoorAqi = outdoorAqi;
  stored.flags = (freshAirOn ? 0x01 : 0x00) | (coolingOn ? 0x02 : 0x00);
  stored.cleanSignature = airSignature(current);
  preferences.putBytes("state", &stored, sizeof(stored));
  lastPersistentSaveMs = now;
  persistentStateDirty = false;
}

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

  // 5) Fade to black before the cycle starts again with Apricot.
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

// ================= WIFI AND SERVER =================
static void startWiFiInBackground() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void maintainWiFi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) return;
  if (now - lastWifiRetryMs >= WIFI_RETRY_INTERVAL_MS) {
    lastWifiRetryMs = now;
    WiFi.disconnect(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

static void buildServerUrl(const char *path, char *url, size_t urlSize) {
  snprintf(url, urlSize, "%s%s", SERVER_BASE_URL, path);
}

static void pollServerState() {
  if (WiFi.status() != WL_CONNECTED) return;
  char url[256];
  buildServerUrl("/api/state", url, sizeof(url));

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.begin(secureClient, url);
  const int code = http.GET();

  if (code == HTTP_CODE_OK) {
    StaticJsonDocument<160> filter;
    filter["flags"]["freshAir"] = true;
    filter["flags"]["cooling"] = true;
    filter["outdoor"]["aqi"] = true;
    filter["outdoor"]["pm25"] = true;
    filter["outdoor"]["pm10"] = true;
    filter["outdoor"]["temperature"] = true;
    filter["outdoor"]["humidity"] = true;

    StaticJsonDocument<320> doc;
    const DeserializationError error =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));

    if (!error) {
      const bool nextFreshAir = doc["flags"]["freshAir"] | freshAirOn;
      const bool nextCooling = doc["flags"]["cooling"] | coolingOn;
      const int16_t nextOutdoorAqi = doc["outdoor"]["aqi"] | outdoorAqi;
      AirValues nextOutdoor = outdoor;
      JsonObject outdoorDoc = doc["outdoor"].as<JsonObject>();
      if (!outdoorDoc.isNull()) {
        nextOutdoor.pm25 = clampU16(outdoorDoc["pm25"] | outdoor.pm25, 0, Upper_Limit_PM);
        nextOutdoor.pm10 = clampU16(outdoorDoc["pm10"] | outdoor.pm10, 0, Upper_Limit_PM);
        nextOutdoor.co2 = 550;
        nextOutdoor.temperature =
          clampS16(outdoorDoc["temperature"] | outdoor.temperature,
                   -40, Upper_Limit_Temperature);
        nextOutdoor.humidity =
          clampU16(outdoorDoc["humidity"] | outdoor.humidity,
                   0, Upper_Limit_Humidity);
      }
      sanitizeOutdoor(&nextOutdoor);

      if (nextFreshAir != freshAirOn || nextCooling != coolingOn ||
          nextOutdoorAqi != outdoorAqi || airValuesDiffer(nextOutdoor, outdoor)) {
        freshAirOn = nextFreshAir;
        coolingOn = nextCooling;
        outdoorAqi = nextOutdoorAqi;
        outdoor = nextOutdoor;
        requestPersistentSave();
      }
    }
  }
  http.end();
}

static void postSensorReading() {
  if (WiFi.status() != WL_CONNECTED) return;
  char url[256];
  buildServerUrl("/api/readings", url, sizeof(url));

  StaticJsonDocument<192> doc;
  doc["deviceId"] = DEVICE_ID;
  doc["co2"] = current.co2;
  doc["pm10"] = current.pm10;
  doc["pm25"] = current.pm25;
  doc["temperature"] = current.temperature;
  doc["humidity"] = current.humidity;

  char payload[160];
  const size_t payloadLength = serializeJson(doc, payload, sizeof(payload));
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  http.POST((uint8_t *)payload, payloadLength);
  http.end();
}

// ================= RELAY =================
static void applyFreshAirRelay() {
  digitalWrite(RELAY_PIN, freshAirOn ? LOW : HIGH);
}

// ================= SETUP / LOOP =================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

#if ENABLE_USB_SERIAL
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" BREETHR 694 + CURVED WAVE (no sensors)");
  Serial.println("========================================");
#endif

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  secureClient.setInsecure();

  preferences.begin("breethr", false);
  loadPersistentState();
  applyFreshAirRelay();

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

  startWiFiInBackground();
  lastWifiRetryMs = millis();
  lastMainFrameMs = millis();
  animationStartMs = millis();
}

void loop() {
  const uint32_t now = millis();

  if (now - lastMainFrameMs >= MAIN_FRAME_MS) {
    lastMainFrameMs = now;
    renderCurvedWave(now);
  }

  maintainWiFi(now);
  applyFreshAirRelay();

  if (now - lastServerPollMs >= SERVER_POLL_INTERVAL_MS) {
    lastServerPollMs = now;
    pollServerState();
    applyFreshAirRelay();
  }
  if (now - lastPostMs >= POST_INTERVAL_MS) {
    lastPostMs = now;
    postSensorReading();
  }
  savePersistentState(false);
}
