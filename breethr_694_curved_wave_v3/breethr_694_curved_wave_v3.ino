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
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2cSps30.h>
#include <math.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

// BREETHR sensor/network code is retained from breethr_288_final.ino.
// Only its LED mapping, animation, and colors have been replaced by the
// approved 694-LED curved wave.

// ================= USER SETTINGS =================
const char WIFI_SSID[] = "Breethr Tata 2.4 Ghz";
const char WIFI_PASSWORD[] = "breethr@321";
const char SERVER_BASE_URL[] = "https://project-57vve.vercel.app";
const char DEVICE_ID[] = "breethr-esp32";

#define ENABLE_USB_SERIAL 1

// ================= LOCKED HARDWARE =================
#define LED_DATA_PIN 5
#define RELAY_PIN 32
#define SDA_PIN 21
#define SCL_PIN 22
#define SCD4X_ADDRESS 0x62
#define SPS30_ADDRESS 0x69
#define ABC 0

#define LED_TYPE WS2812B
// This 694-LED panel is wired for RGB byte order. Using GRB swaps the red and
// green channels and makes the supplied yellows/oranges appear green.
#define COLOR_ORDER RGB

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
static constexpr uint8_t BRIGHTNESS = 96;
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
static constexpr uint16_t WAVE_A_PERIOD_MS = 6600; // Lower = faster.
static constexpr uint16_t WAVE_B_PERIOD_MS = 8200; // Lower = faster.
static constexpr uint32_t SENSOR_INTERVAL_MS = 5000UL;
static constexpr uint32_t POST_INTERVAL_MS = 10000UL;
static constexpr uint32_t SERVER_POLL_INTERVAL_MS = 10000UL;
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000UL;
static constexpr uint32_t PERSIST_INTERVAL_MS = 120000UL;
static constexpr uint32_t SCD_RETRY_INTERVAL_MS = 30000UL;
static constexpr uint32_t SPS_RETRY_INTERVAL_MS = 30000UL;
static constexpr uint16_t I2C_TIMEOUT_MS = 25;
static constexpr uint16_t HTTP_CONNECT_TIMEOUT_MS = 5000;
static constexpr uint16_t HTTP_READ_TIMEOUT_MS = 5000;

CRGB leds[NUM_LEDS];
SensirionI2cScd4x scd4x;
SensirionI2cSps30 sps30;
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
uint16_t previousCo2 = Lower_Limit_CO2_SCD;

bool freshAirOn = false;
bool coolingOn = false;
bool persistentStateDirty = false;
bool scdAvailable = false;
bool spsAvailable = false;
uint8_t scdMisses = 0;
uint8_t spsMisses = 0;
int16_t outdoorAqi = -1;

uint32_t lastMainFrameMs = 0;
uint32_t lastSensorReadMs = 0;
uint32_t lastPostMs = 0;
uint32_t lastServerPollMs = 0;
uint32_t lastWifiRetryMs = 0;
uint32_t lastPersistentSaveMs = 0;
uint32_t lastScdRetryMs = 0;
uint32_t lastSpsRetryMs = 0;

// ================= FIXED ATTACHED COLORS =================
// These four values are the exact swatches requested. There are no gradients,
// hue shifts, saturation boosts, or additional palette colors.
static constexpr uint32_t COLOR_UNDER_25 = 0xD3613D; // orange
static constexpr uint32_t COLOR_UNDER_50 = 0xEBB89A; // peach
static constexpr uint32_t COLOR_UNDER_75 = 0xD3ABC8; // lilac
static constexpr uint32_t COLOR_75_PLUS = 0xABAFEA;  // supplied blue

float curveA[GRID_X2];
float curveB[GRID_X2];
float targetQuality = 1.0f; // 0 = all bad; 1 = all good.

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
  previousCo2 = current.co2;
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

// Four original binary states contribute equally. PM10 is still collected and
// uploaded, but the original condition signature did not use it.
static float combinedQuality(const AirValues &air) {
  uint8_t goodCount = 0;
  if (air.pm25 <= GOOD_PM25_MAX) ++goodCount;
  if (air.co2 <= GOOD_CO2_MAX) ++goodCount;
  if (air.temperature <= GOOD_TEMPERATURE_MAX) ++goodCount;
  if (air.humidity <= GOOD_HUMIDITY_MAX) ++goodCount;
  return goodCount * 0.25f;
}

static CRGB fixedColorForQuality(float quality) {
  const float percent = quality * 100.0f;
  if (percent < 25.0f) return colorFromHex(COLOR_UNDER_25);
  if (percent < 50.0f) return colorFromHex(COLOR_UNDER_50);
  if (percent < 75.0f) return colorFromHex(COLOR_UNDER_75);
  // The score can be exactly 75%, so 75% and 100% both use the blue state.
  return colorFromHex(COLOR_75_PLUS);
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
  const CRGB waveColor = fixedColorForQuality(targetQuality);

  for (uint8_t row = 0; row < NUM_ROWS; ++row) {
    const float y = float(row) / (NUM_ROWS - 1);
    const uint8_t leftX2 = 50 - ROW_LEN[row];
    for (uint8_t col = 0; col < ROW_LEN[row]; ++col) {
      const uint8_t x2 = leftX2 + 2 * col;
      const float ribbonA = softBand(y - curveA[x2], 0.088f);
      const float ribbonB = softBand(y - curveB[x2], 0.10f);
      const float halo = softBand(y - curveA[x2], 0.18f);
      float light = 0.66f * ribbonA + 0.48f * ribbonB + 0.08f * halo;
      if (light > 1.0f) light = 1.0f;

      // LEDs outside the two ribbons remain exactly RGB(0,0,0).
      if (light < 0.025f) continue;
      CRGB color = waveColor;
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

// ================= SCD4x + SPS30 =================
static void initScdSensor(uint32_t now) {
  if (now - lastScdRetryMs < SCD_RETRY_INTERVAL_MS && lastScdRetryMs != 0) return;
  lastScdRetryMs = now;
  scd4x.begin(Wire, SCD4X_ADDRESS);
  scd4x.stopPeriodicMeasurement();
  delay(500);
  scd4x.setAutomaticSelfCalibrationEnabled(ABC ? 1 : 0);
  scdAvailable = (scd4x.startPeriodicMeasurement() == 0);
  scdMisses = 0;
}

static void initSpsSensor(uint32_t now) {
  if (now - lastSpsRetryMs < SPS_RETRY_INTERVAL_MS && lastSpsRetryMs != 0) return;
  lastSpsRetryMs = now;
  sps30.begin(Wire, SPS30_ADDRESS);
  sps30.stopMeasurement();
  delay(100);
  const int16_t spsError =
    sps30.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT);
  spsAvailable = (spsError == 0);
  spsMisses = 0;

#if ENABLE_USB_SERIAL
  if (spsError) {
    Serial.print("SPS30 START ERROR: ");
    Serial.println(spsError);
  } else {
    Serial.println("SPS30 initialized OK");
  }
#endif
}

static bool dust(uint16_t *p25, uint16_t *p10) {
  if (!spsAvailable) {
    initSpsSensor(millis());
    return false;
  }

  float pm1 = 0.0f, pm25 = 0.0f, pm4 = 0.0f, pm10 = 0.0f;
  float nc05 = 0.0f, nc1 = 0.0f, nc25 = 0.0f;
  float nc4 = 0.0f, nc10 = 0.0f, typicalParticleSize = 0.0f;
  uint16_t dataReady = 0;
  int16_t error = sps30.readDataReadyFlag(dataReady);

  if (error == 0 && dataReady) {
    error = sps30.readMeasurementValuesFloat(
      pm1, pm25, pm4, pm10, nc05, nc1, nc25, nc4, nc10,
      typicalParticleSize);
  }

  if (error == 0 && dataReady) {
    *p25 = roundedClampedU16(pm25, 0, Upper_Limit_PM);
    *p10 = roundedClampedU16(pm10, 0, Upper_Limit_PM);
    spsMisses = 0;
#if ENABLE_USB_SERIAL
    Serial.print("PM2.5       : "); Serial.print(pm25, 2); Serial.println(" ug/m3");
    Serial.print("PM10        : "); Serial.print(pm10, 2); Serial.println(" ug/m3");
#endif
    return true;
  }

  if (error == 0 && !dataReady) {
#if ENABLE_USB_SERIAL
    Serial.println("SPS30       : Data not ready");
#endif
    return false;
  }

#if ENABLE_USB_SERIAL
  Serial.print("SPS30 ERROR : "); Serial.println(error);
#endif
  if (++spsMisses >= 3) {
    spsAvailable = false;
    spsMisses = 0;
    lastSpsRetryMs = 0;
  }
  return false;
}

static void readCO2() {
  if (!scdAvailable) {
    initScdSensor(millis());
    current.co2 = previousCo2;
    return;
  }

  bool dataReady = false;
  if (scd4x.getDataReadyStatus(dataReady) != 0) {
    current.co2 = previousCo2;
    if (++scdMisses >= 3) {
      scdAvailable = false;
      scdMisses = 0;
    }
    return;
  }
  if (!dataReady) {
    current.co2 = previousCo2;
    return;
  }

  uint16_t rawCo2 = 0;
  float rawTemperature = 0.0f;
  float rawHumidity = 0.0f;
  if (scd4x.readMeasurement(rawCo2, rawTemperature, rawHumidity) != 0) {
    current.co2 = previousCo2;
    if (++scdMisses >= 3) {
      scdAvailable = false;
      scdMisses = 0;
    }
    return;
  }

  current.co2 = clampU16(rawCo2, Lower_Limit_CO2_SCD, Upper_Limit_CO2);
  current.temperature = roundedClampedS16(rawTemperature, -40, Upper_Limit_Temperature);
  current.humidity = roundedClampedU16(rawHumidity, 0, Upper_Limit_Humidity);
  previousCo2 = current.co2;
  scdMisses = 0;

#if ENABLE_USB_SERIAL
  Serial.print("CO2         : "); Serial.print(rawCo2); Serial.println(" ppm");
  Serial.print("Temperature : "); Serial.print(rawTemperature, 2); Serial.println(" C");
  Serial.print("Humidity    : "); Serial.print(rawHumidity, 2); Serial.println(" %");
#endif
}

static void readSensors() {
  const AirValues previous = current;
  uint16_t pm25 = current.pm25;
  uint16_t pm10 = current.pm10;

#if ENABLE_USB_SERIAL
  Serial.println("----------------------------------------");
#endif
  readCO2();
  if (dust(&pm25, &pm10)) {
    current.pm25 = pm25;
    current.pm10 = pm10;
  }
  sanitizeIndoor(&current);
  if (airValuesDiffer(previous, current)) requestPersistentSave();

#if ENABLE_USB_SERIAL
  const float score = combinedQuality(current) * 100.0f;
  Serial.print("Combined good: "); Serial.print(score, 0); Serial.println(" %");
  Serial.println("----------------------------------------");
#endif
}

// ================= SETUP / LOOP =================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

#if ENABLE_USB_SERIAL
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" BREETHR 694 + CURVED AIR-QUALITY WAVE");
  Serial.println(" SCD4x: CO2 / temperature / humidity");
  Serial.println(" SPS30: PM2.5 / PM10");
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
  // Do not apply FastLED color correction: send the supplied palette values
  // directly, apart from the wave's intentional brightness scaling.
  FastLED.setCorrection(UncorrectedColor);
  FastLED.setDither(false);
  clearPanel();
  FastLED.show();
  targetQuality = combinedQuality(current);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  delay(500);
  initScdSensor(millis());
  initSpsSensor(millis());

  startWiFiInBackground();
  lastWifiRetryMs = millis();
  lastMainFrameMs = millis();
  lastSensorReadMs = millis();
}

void loop() {
  const uint32_t now = millis();

  if (now - lastMainFrameMs >= MAIN_FRAME_MS) {
    lastMainFrameMs = now;
    targetQuality = combinedQuality(current);
    renderCurvedWave(now);
  }

  maintainWiFi(now);
  applyFreshAirRelay();

  if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = now;
    readSensors();
  }
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
