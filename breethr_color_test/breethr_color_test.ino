// BREETHR 694 colour test. Upload, watch the panel, take a photo of each step.
// Each step shows for 6 seconds, then the steps repeat.
//
//  STEP 1  Top third RED, middle GREEN, bottom BLUE   (checks COLOR_ORDER)
//  STEP 2  Apricot #EDAB7A : left = raw hex | right = gamma corrected
//  STEP 3  Lilac   #C4A3D6 : left = raw hex | right = gamma corrected
//  STEP 4  Sky     #A8CFDA : left = raw hex | right = gamma corrected
//  STEP 5  Same as step 2 but at full brightness (255 instead of 96)
#define FASTLED_INTERNAL
#include <FastLED.h>
#include <math.h>

#define LED_DATA_PIN 5
#define COLOR_ORDER GRB   // the setting we are testing

static constexpr uint16_t NUM_LEDS = 694;
static constexpr uint8_t NUM_ROWS = 15;
static constexpr uint8_t ROW_LEN[NUM_ROWS] = {43,44,45,46,47,48,49,50,49,48,47,46,45,44,43};
static constexpr uint16_t ROW_START[NUM_ROWS] = {0,43,87,132,178,225,273,322,372,421,469,516,562,607,651};
CRGB leds[NUM_LEDS];

static int16_t rowColToIndex(uint8_t row, uint8_t col) {
  const uint8_t off = (row & 1) ? col : ROW_LEN[row] - 1 - col;
  return ROW_START[row] + off;
}

static uint8_t gammaByte(uint8_t v) {
  return uint8_t(powf(v / 255.0f, 2.2f) * 255.0f + 0.5f);
}

static CRGB hexColor(uint32_t h, bool gamma) {
  uint8_t r = h >> 16, g = h >> 8, b = h;
  if (gamma) { r = gammaByte(r); g = gammaByte(g); b = gammaByte(b); }
  return CRGB(r, g, b);
}

static void splitRawVsGamma(uint32_t hex) {
  for (uint8_t row = 0; row < NUM_ROWS; ++row)
    for (uint8_t col = 0; col < ROW_LEN[row]; ++col)
      leds[rowColToIndex(row, col)] = hexColor(hex, col >= ROW_LEN[row] / 2);
}

void setup() {
  Serial.begin(115200);
  FastLED.addLeds<WS2812B, LED_DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 3000);
  FastLED.setCorrection(UncorrectedColor);
  FastLED.setDither(false);
}

void loop() {
  const uint8_t step = (millis() / 6000) % 5 + 1;
  FastLED.setBrightness(step == 5 ? 255 : 96);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  if (step == 1) {
    for (uint8_t row = 0; row < NUM_ROWS; ++row) {
      const CRGB c = row < 5 ? CRGB(255, 0, 0) : row < 10 ? CRGB(0, 255, 0) : CRGB(0, 0, 255);
      for (uint8_t col = 0; col < ROW_LEN[row]; ++col) leds[rowColToIndex(row, col)] = c;
    }
  }
  if (step == 2 || step == 5) splitRawVsGamma(0xEDAB7A);
  if (step == 3) splitRawVsGamma(0xC4A3D6);
  if (step == 4) splitRawVsGamma(0xA8CFDA);
  FastLED.show();
  static uint8_t lastStep = 0;
  if (step != lastStep) { lastStep = step; Serial.printf("STEP %u\n", step); }
  delay(30);
}
