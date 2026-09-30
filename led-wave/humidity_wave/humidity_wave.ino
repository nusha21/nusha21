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
 *   A glowing, dotted-mesh ribbon rides an ocean swell left -> right: uneven
 *   wave heights, sharp crests, flat troughs, and a slow roll as it goes.
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

// Ocean swell. The periods don't divide into each other, so the exact
// pattern practically never repeats.
const uint32_t SWELL_PERIOD_MS = 7000;   // main swell crossing the panel (lower = faster)
const uint32_t CHOP_PERIOD_MS  = 4300;   // shorter waves riding on the swell
const uint32_t SET_PERIOD_MS   = 17000;  // "sets" of bigger waves rolling through
const uint32_t ROLL_PERIOD_MS  = 9000;   // slow roll of the ribbon (front <-> back face)
const uint32_t MESH_PERIOD_MS  = 900;    // mesh lines travelling with the water
const float SWELL_AMPLITUDE = 3.6f;  // height of the main swell (rows)
const float CHOP_AMPLITUDE  = 0.7f;  // height of the small waves (rows)
const float CREST_SHARPNESS = 0.28f; // 0 = plain sine, higher = peakier crests, flatter troughs
const float RIBBON_HALF_W   = 3.4f;  // half-width of the ribbon when seen face-on (rows)
const float RIBBON_MIN_W    = 1.9f;  // half-width when seen edge-on (keeps a solid band)
const float EDGE_SOFTNESS   = 0.9f;  // rows over which the ribbon edge fades out (inward)
const float HALO            = 0.18f; // faint glow just outside the ribbon
const float MESH_STRENGTH   = 0.5f;  // 0 = solid ribbon, 1 = only mesh dots visible
const float MESH_SPACING    = 4.0f;  // LEDs between the cross lines of the mesh

// How "alive" the water feels: uneven speed, wandering edges, drifting light.
// 0 = perfectly regular wave, 1 = default, up to ~1.5 for a rougher sea.
const float ORGANIC         = 1.0f;

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

// Per-column values of the ribbon. Rows are centred, so a column sits on a
// half-LED grid: slot = 2 * x, where x = 0 .. MAX_ROW_LEN - 1.
struct Column {
  float yCentre;  // row of the ribbon centre line
  float halfTop;  // visible half-thickness above the centre line (rows)
  float halfBot;  // ... and below it; they differ so the ribbon isn't mirrored
  float cosT;     // ribbon roll: +1 front face, -1 back face, 0 edge-on
  float sinT;
  float across;   // 0..1 brightness of the cross-mesh line at this column
  float crest;    // 0..1, 1 on top of a wave crest
  float shimmer;  // brightness multiplier: slow patches of light drifting across
};
const uint8_t NUM_SLOTS = 2 * (MAX_ROW_LEN - 1) + 1;
Column columns[NUM_SLOTS];

// Smooth pseudo-random wobble in about -1..1: three sines at unrelated
// frequencies, some drifting left and some right, so it never looks periodic.
static inline float wobble(float u, float p1, float p2, float p3, float seed) {
  const float TWO_PI_F = 6.2831853f;
  return 0.5f  * sinf(TWO_PI_F * (1.3f * u + p1) + seed)
       + 0.3f  * sinf(TWO_PI_F * (2.9f * u - p2) + 2.1f * seed)
       + 0.2f  * sinf(TWO_PI_F * (5.1f * u + p3) + 3.7f * seed);
}

// Second-order (Stokes) wave: sharper crests and flatter troughs than a sine.
// Returns the height (up is positive) and its slope with respect to theta.
static inline float stokes(float theta, float amp, float *slope) {
  float s1 = sinf(theta), c1 = cosf(theta);
  float s2 = 2.0f * s1 * c1, c2 = c1 * c1 - s1 * s1;
  *slope = -amp * (s1 + 2.0f * CREST_SHARPNESS * s2);
  return amp * (c1 + CREST_SHARPNESS * c2);
}

void computeColumns(uint32_t ms) {
  const float TWO_PI_F = 6.2831853f;
  const float midRow = (NUM_ROWS - 1) * 0.5f;
  // Real water doesn't move at a constant speed: nudge the phases back and
  // forth slowly so the swell surges and eases (about +/-25% speed).
  const float surge = ORGANIC * sinf(TWO_PI_F * phase(ms, 11300));
  const float ease  = ORGANIC * sinf(TWO_PI_F * phase(ms, 15700));
  const float swellPh = phase(ms, SWELL_PERIOD_MS) + 0.04f * surge;
  const float chopPh  = phase(ms, CHOP_PERIOD_MS) + 0.03f * ease;
  const float setPh   = phase(ms, SET_PERIOD_MS);
  const float rollPh  = phase(ms, ROLL_PERIOD_MS) + 0.05f * ease;
  const float n1 = phase(ms, 5300), n2 = phase(ms, 6700), n3 = phase(ms, 3900);
  const float meshPh  = phase(ms, MESH_PERIOD_MS);
  const float meshLines = (MAX_ROW_LEN - 1) / MESH_SPACING;

  for (uint8_t slot = 0; slot < NUM_SLOTS; slot++) {
    float u = (slot * 0.5f) / (MAX_ROW_LEN - 1);  // 0 = left edge, 1 = right edge
    Column &col = columns[slot];

    // Bigger and smaller waves arrive in groups, like an ocean swell.
    float setEnv = 0.62f + 0.38f * sinf(TWO_PI_F * (0.45f * u - setPh));

    // (k * u - phase) moves every crest from left to right.
    float slope1, slope2;
    float h1 = stokes(TWO_PI_F * (1.0f * u - swellPh), SWELL_AMPLITUDE * setEnv, &slope1);
    float h2 = stokes(TWO_PI_F * (2.2f * u - chopPh) + 0.7f, CHOP_AMPLITUDE, &slope2);
    float height = h1 + h2;
    height += ORGANIC * 0.5f * wobble(u, n1, n2, n3, 0.0f);
    col.yCentre = midRow - height;  // rows count downward, so up = smaller row

    // Steepness of the water surface in rows per LED column
    float slope = (slope1 * 1.0f + slope2 * 2.2f) * TWO_PI_F / (MAX_ROW_LEN - 1);

    // The ribbon slowly rolls, and tips further over on the steep face of a wave.
    float theta = TWO_PI_F * (0.45f * u - rollPh) + 1.4f * slope;
    col.cosT = cosf(theta);
    col.sinT = sinf(theta);
    float halfW = RIBBON_MIN_W + RIBBON_HALF_W * fabsf(col.cosT);
    // Top and bottom edges wander on their own, so the thickness breathes.
    col.halfTop = halfW * (1.0f + ORGANIC * 0.16f * wobble(u, n2, n3, n1, 1.0f));
    col.halfBot = halfW * (1.0f + ORGANIC * 0.16f * wobble(u, n3, n1, n2, 2.0f));
    col.shimmer = 1.0f + ORGANIC * 0.14f * wobble(u, n1, n3, n2, 3.0f);

    // Cross lines of the mesh drift right with the water.
    float a = 0.5f + 0.5f * cosf(TWO_PI_F * (u * meshLines - meshPh));
    col.across = a * a * a;  // cubed = thinner lines, clearer dots

    col.crest = clamp01(h1 / (SWELL_AMPLITUDE * (1.0f + CREST_SHARPNESS)));
  }
}

void renderWave(uint32_t ms) {
  const float TWO_PI_F = 6.2831853f;
  uint32_t totalLevel = 0;  // sum of all channel values, for the power limiter

  computeColumns(ms);

  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    uint8_t slotOffset = MAX_ROW_LEN - ROW_LEN[r];  // centring offset in half-LEDs

    for (uint8_t c = 0; c < ROW_LEN[r]; c++) {
      const Column &col = columns[slotOffset + 2 * c];
      uint16_t idx = ledIndex(r, c);

      // Distance (rows) outside the ribbon edge; negative = inside.
      float halfW = (r < col.yCentre) ? col.halfTop : col.halfBot;
      float dist = fabsf(r - col.yCentre) - halfW;
      float cover = 1.0f - smoothstep(-EDGE_SOFTNESS, 0.4f, dist);
      float glow = dist > 0.0f ? HALO / (1.0f + 4.0f * dist * dist) : HALO;
      if (cover <= 0.0f && glow < 0.03f) {
        strip.setPixelColor(idx, 0);
        continue;
      }

      float s = (r - col.yCentre) / halfW;  // -1..1 across the ribbon

      // Mesh: lines along the ribbon (fade out when seen edge-on, where they would
      // squash together) crossed by lines that travel with the wave. Dots where they meet.
      float faceOn = fabsf(col.cosT);
      float alongLine = 0.5f + 0.5f * cosf(TWO_PI_F * 2.0f * s);
      float along = 1.0f + (alongLine - 1.0f) * faceOn;
      float mesh = (1.0f - MESH_STRENGTH) + MESH_STRENGTH * 0.5f * (col.across + along);

      // 3D shading: the side tilted toward you is brighter, edge-on folds
      // catch a highlight, and wave crests glint.
      float depth = 0.5f + 0.5f * s * col.sinT;  // 0 = far, 1 = near
      float light = 0.5f + 0.3f * depth + 0.15f * (1.0f - faceOn) + 0.2f * col.crest;
      // Everything above is "how bright it looks". LEDs are linear, so convert
      // (gamma 2.2) - this keeps fades smooth and lets the mesh gaps go dark.
      float look = clamp01(cover * mesh * light * col.shimmer + glow);
      float level = look * look * sqrtf(sqrtf(look)) * (MAX_BRIGHTNESS / 255.0f);  // ~look^2.25

      // Front face (cos > 0) = Lilac, back face (cos < 0) = Blush
      float rgb[3];
      paletteColor((1.0f - col.cosT) * 0.5f, rgb);

      // Too dim to show the colour (only one channel would light, giving
      // stray red/blue specks) - switch the LED off instead.
      if (level * 255.0f < 3.0f) {
        strip.setPixelColor(idx, 0);
        continue;
      }

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
