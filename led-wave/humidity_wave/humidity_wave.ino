/*
 * Humidity Wave - twisting ribbon animation for a 694-LED oval WS2812B panel
 * Board   : ESP32
 * Library : Adafruit NeoPixel
 *
 * Panel layout (row lengths, top to bottom):
 *   43 44 45 46 47 48 49 50 49 48 47 46 45 44 43   = 694 LEDs
 * Rows are centred, giving the oval shape.
 *
 * Wiring (serpentine / zig-zag):
 *   Data enters at the RIGHT end of the TOP row (LED #0).
 *   Row 1 runs right -> left, row 2 left -> right, row 3 right -> left, ...
 *
 * Animation:
 *   A glowing ribbon travels left -> right as a wave and twists as it goes.
 *   Front face of the ribbon = Lilac (#C4A3D6), back face = Blush (#DDB0BE),
 *   with the other Humidity shades blended in between while it turns.
 */

#include <Adafruit_NeoPixel.h>
#include <math.h>

// ---------------------------------------------------------------------------
// USER SETTINGS - change these to match your hardware / taste
// ---------------------------------------------------------------------------
#define DATA_PIN        5       // ESP32 GPIO connected to the panel DIN
#define MAX_BRIGHTNESS  90      // 0-255 global brightness cap
#define MAX_MILLIAMPS   4000    // power budget for the LEDs (keep below your PSU rating)
#define TARGET_FPS      40      // 694 LEDs take ~21 ms to send, so ~45 fps is the max

// Uncomment to run a wiring check instead of the animation:
// lights the first LED of every row red and the last LED green.
// #define WIRING_TEST

const uint32_t WAVE_PERIOD_MS   = 6000;  // time for one wave to cross the panel (lower = faster)
const uint32_t TWIST_PERIOD_MS  = 4000;  // time for one full twist to pass a point
const uint32_t RIPPLE_PERIOD_MS = 4200;  // time for the small secondary ripple
const float WAVE_AMPLITUDE = 3.2f;  // how far the ribbon swings up/down (in rows)
const float RIBBON_HALF_W  = 4.2f;  // half-width of the ribbon when seen face-on (rows)
const float RIBBON_MIN_W   = 0.9f;  // half-width when seen edge-on (keeps a thin line)

// ---------------------------------------------------------------------------
// PANEL GEOMETRY
// ---------------------------------------------------------------------------
const uint8_t NUM_ROWS = 15;
const uint8_t ROW_LEN[NUM_ROWS] = {43, 44, 45, 46, 47, 48, 49, 50, 49, 48, 47, 46, 45, 44, 43};
const uint8_t MAX_ROW_LEN = 50;
const uint16_t NUM_LEDS = 694;

// ---------------------------------------------------------------------------
// HUMIDITY PALETTE  (Lilac -> Blush)
// ---------------------------------------------------------------------------
const uint8_t PALETTE_SIZE = 6;
const uint8_t PALETTE[PALETTE_SIZE][3] = {
  {0xC4, 0xA3, 0xD6},  // #C4A3D6  front face
  {0xC9, 0xA6, 0xD1},  // #C9A6D1
  {0xCE, 0xA8, 0xCD},  // #CEA8CD
  {0xD3, 0xAB, 0xC8},  // #D3ABC8
  {0xD8, 0xAE, 0xC3},  // #D8AEC3
  {0xDD, 0xB0, 0xBE},  // #DDB0BE  back face
};

Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);

uint16_t rowStart[NUM_ROWS];  // index of the first LED (in wiring order) of each row

// Convert (row, column-from-left) to the LED index along the wire.
uint16_t ledIndex(uint8_t row, uint8_t col) {
  if (row % 2 == 0) {
    return rowStart[row] + (ROW_LEN[row] - 1 - col);  // even rows: right -> left
  }
  return rowStart[row] + col;                         // odd rows: left -> right
}

static inline float clamp01(float x) {
  return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static inline float smoothstep(float e0, float e1, float x) {
  float t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3.0f - 2.0f * t);
}

// Palette after gamma correction. The hex codes are screen (sRGB) colours;
// LEDs are linear, so without this the pastels look washed-out / white.
uint8_t ledPalette[PALETTE_SIZE][3];

// Blend through the palette, p = 0 (Lilac) .. 1 (Blush)
static void paletteColor(float p, float out[3]) {
  p = clamp01(p) * (PALETTE_SIZE - 1);
  uint8_t i = (uint8_t)p;
  if (i >= PALETTE_SIZE - 1) i = PALETTE_SIZE - 2;
  float f = p - i;
  for (uint8_t k = 0; k < 3; k++) {
    out[k] = ledPalette[i][k] + (ledPalette[i + 1][k] - ledPalette[i][k]) * f;
  }
}

// Scale the frame down if it would draw more than MAX_MILLIAMPS.
// WS2812B: ~20 mA per channel at full (255), ~1 mA idle per LED.
void limitPower(uint32_t totalLevel) {
  uint32_t milliamps = (totalLevel * 20UL) / 255UL + NUM_LEDS;
  if (milliamps <= MAX_MILLIAMPS) return;

  float scale = (float)(MAX_MILLIAMPS - NUM_LEDS) / (float)(milliamps - NUM_LEDS);
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    uint32_t col = strip.getPixelColor(i);
    uint8_t r = (uint8_t)(((col >> 16) & 0xFF) * scale);
    uint8_t g = (uint8_t)(((col >> 8) & 0xFF) * scale);
    uint8_t b = (uint8_t)((col & 0xFF) * scale);
    strip.setPixelColor(i, r, g, b);
  }
}

// Phase helper: 0..1 repeating every periodMs. Uses integer maths so the
// animation stays smooth no matter how long the ESP32 has been running.
static inline float phase(uint32_t ms, uint32_t periodMs) {
  return (float)(ms % periodMs) / (float)periodMs;
}

void renderWave(uint32_t ms) {
  const float TWO_PI_F = 6.2831853f;
  const float midRow = (NUM_ROWS - 1) * 0.5f;
  const float wavePh   = phase(ms, WAVE_PERIOD_MS);
  const float twistPh  = phase(ms, TWIST_PERIOD_MS);
  const float ripplePh = phase(ms, RIPPLE_PERIOD_MS);
  uint32_t totalLevel = 0;  // sum of all channel values, for the power limiter

  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    float offset = (MAX_ROW_LEN - ROW_LEN[r]) * 0.5f;  // centre each row

    for (uint8_t c = 0; c < ROW_LEN[r]; c++) {
      float u = (offset + c) / (MAX_ROW_LEN - 1);  // 0 = left edge, 1 = right edge

      // Ribbon centre line: main wave plus a smaller, faster wave for an organic feel.
      // Using (u - phase) moves every crest from left to right.
      float yCentre = midRow
                    + WAVE_AMPLITUDE * sinf(TWO_PI_F * (u - wavePh))
                    + 0.8f * sinf(TWO_PI_F * (2.3f * u - ripplePh) + 1.3f);

      // Twist angle of the ribbon at this x position
      float theta = TWO_PI_F * (0.8f * u - twistPh);
      float cosT = cosf(theta);
      float sinT = sinf(theta);

      // Visible (projected) half-thickness: wide face-on, thin edge-on
      float halfW = RIBBON_MIN_W + RIBBON_HALF_W * fabsf(cosT);
      float s = (r - yCentre) / halfW;  // -1..1 across the ribbon
      float cover = 1.0f - smoothstep(0.75f, 1.15f, fabsf(s));

      uint16_t idx = ledIndex(r, c);
      if (cover <= 0.0f) {
        strip.setPixelColor(idx, 0);
        continue;
      }

      // 3D shading: the side of the ribbon tilted toward you is brighter,
      // and edge-on folds get a highlight like in the reference video.
      float depth = 0.5f + 0.5f * s * sinT;            // 0 = far, 1 = near
      float light = 0.55f + 0.30f * depth + 0.15f * (1.0f - fabsf(cosT));
      float level = cover * light * (MAX_BRIGHTNESS / 255.0f);

      // Front face (cos > 0) = Lilac, back face (cos < 0) = Blush
      float rgb[3];
      paletteColor((1.0f - cosT) * 0.5f, rgb);

      uint8_t out[3];
      for (uint8_t k = 0; k < 3; k++) {
        out[k] = (uint8_t)(rgb[k] * level + 0.5f);
        totalLevel += out[k];
      }
      strip.setPixelColor(idx, out[0], out[1], out[2]);
    }
  }

  limitPower(totalLevel);
}

void renderWiringTest() {
  strip.clear();
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    strip.setPixelColor(ledIndex(r, 0), 40, 0, 0);                // left end = red
    strip.setPixelColor(ledIndex(r, ROW_LEN[r] - 1), 0, 40, 0);   // right end = green
  }
  strip.setPixelColor(0, 0, 0, 60);  // LED #0 = blue (should be top-right)
}

void setup() {
  uint16_t start = 0;
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    rowStart[r] = start;
    start += ROW_LEN[r];
  }

  for (uint8_t i = 0; i < PALETTE_SIZE; i++) {
    for (uint8_t k = 0; k < 3; k++) {
      ledPalette[i][k] = Adafruit_NeoPixel::gamma8(PALETTE[i][k]);
    }
  }

  strip.begin();
  strip.clear();
  strip.show();
}

void loop() {
  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (now - lastFrame < 1000UL / TARGET_FPS) return;
  lastFrame = now;

#ifdef WIRING_TEST
  renderWiringTest();
#else
  renderWave(now);
#endif

  strip.show();
}
