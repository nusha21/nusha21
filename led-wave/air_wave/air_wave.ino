/*
 * Air Wave - 3D ribbon animation for a 694-LED oval WS2812B panel
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
 * Patterns (both are a 3D ribbon drawn in perspective, with pointed ends):
 *   Silk   - a see-through sheet of silk with one glowing edge, folding
 *            gently; occasional random swells rise through it. Colours run
 *            along its length (left tip = first colour, right tip = last).
 *   Bulges - a lit, solid ribbon. Bulges appear at random places, swell up,
 *            drift left -> right and fade away. Front face = first colour,
 *            back face = last colour.
 *
 * Serial Monitor (115200 baud):
 *   p = switch pattern (Silk <-> Bulges)
 *   1 = Humidity     (Lilac -> Blush)
 *   2 = Temperature  (Sky -> Periwinkle)
 *   3 = PM2.5        (Apricot -> Burnt orange)
 *   4 = CO2          (Amber -> Honey)
 *   a = auto: change theme every 5 seconds (this is the default at start-up)
 */

#include <Adafruit_NeoPixel.h>
#include <math.h>

// ---------------------------------------------------------------------------
// USER SETTINGS
// ---------------------------------------------------------------------------
#define DATA_PIN        5       // ESP32 GPIO connected to the panel DIN
#define MAX_BRIGHTNESS  90      // 0-255 global brightness cap
#define MAX_MILLIAMPS   4000    // power budget for the LEDs (keep below your PSU rating)
#define TARGET_FPS      40      // 694 LEDs take ~21 ms to send, so ~45 fps is the max

// Uncomment to run a wiring check instead of the animation:
// LED #0 blue, left end of every row red, right end green.
// #define WIRING_TEST

const uint32_t THEME_HOLD_MS  = 5000;  // auto mode: time on each colour theme
const uint32_t THEME_FADE_MS  = 1000;  // cross-fade time between themes

// Pattern shown at start-up: 0 = Silk, 1 = Bulges ('p' switches)
#define START_PATTERN   0
const uint32_t PATTERN_FADE_MS = 600;  // fade-in after switching pattern

// ---- Silk pattern ----
const float SILK_START      = 1.0f;    // LED column of the left tip
const float SILK_LENGTH     = 47.0f;   // length in LEDs
const float SILK_WIDTH      = 7.0f;    // half-width of the sheet (rows) - bigger = wider silk
const float SILK_WAVE       = 1.3f;    // height of the main up/down wave (rows)
const uint32_t SILK_WAVE_MS   = 7000;  // main wave (left -> right), lower = faster
const uint32_t SILK_CROSS_MS  = 11000; // second wave going the other way (changing folds)
const uint32_t SILK_RIPPLE_MS = 5300;  // small ripple that twists the sheet
const float SILK_EDGE       = 2.2f;    // brightness of the glowing edge line
const float SILK_BODY       = 0.15f;   // brightness of the faint side of the sheet (0..1)
const float SILK_GLOW       = 3.2f;    // overall brightness of the silk
const float SILK_TILT       = 0.35f;   // radians we look down onto the sheet
const uint32_t SILK_SWELL_EVERY_MS = 2600;  // random swells in the silk, on average this often
const float SILK_SWELL_LIFT = 2.0f;    // how far swells rise / dip (rows)
const float SILK_SWELL_POP  = 3.0f;    // how far swells come toward you

// ---- Bulges pattern ----
// Ribbon shape
const float RIBBON_START    = 2.0f;   // LED column where the ribbon's left tip is
const float RIBBON_LENGTH   = 45.0f;  // length in LEDs (panel is 50 wide, so the ends stay dark)
const float RIBBON_WIDTH    = 5.6f;   // half-width of a full-size bulge (rows)
const float RIBBON_THIN     = 0.24f;  // thickness between bulges, as a fraction of RIBBON_WIDTH

// Random bulges
const uint32_t BULGE_EVERY_MS = 1600;  // on average a new bulge this often
const float BULGE_LIFE_MIN  = 3.0f;    // seconds a bulge lives (random between min and max)
const float BULGE_LIFE_MAX  = 6.0f;
const float BULGE_SIZE_MIN  = 0.45f;   // random bulge size (0..1)
const float BULGE_SIZE_MAX  = 1.0f;
const float BULGE_SPEED_MIN = 0.06f;   // drift to the right, ribbon lengths per second
const float BULGE_SPEED_MAX = 0.18f;
const float BULGE_LIFT      = 3.2f;    // how far bulges rise up / dip down (rows)
const float BULGE_POP       = 5.5f;    // how far bulges swell toward you (3D depth)

// Background motion
const uint32_t SWELL_PERIOD_MS = 6000; // gentle wave under the bulges (lower = faster)
const uint32_t ROLL_PERIOD_MS  = 8000; // slow twist of the ribbon
const float SWELL_HEIGHT    = 1.1f;    // rows

// 3D look
const float CAMERA_DIST     = 38.0f;   // lower = stronger perspective
const float CAMERA_TILT     = 0.31f;   // radians we look down onto the ribbon (~18 degrees)
const float BULGE_GLOW      = 5.5f;    // overall brightness of the bulge ribbon

// Both patterns
const float GLOW            = 1.1f;    // how quickly dense / folded areas saturate

// ---------------------------------------------------------------------------
// PANEL GEOMETRY
// ---------------------------------------------------------------------------
const uint8_t NUM_ROWS = 15;
const uint8_t ROW_LEN[NUM_ROWS] = {43, 44, 45, 46, 47, 48, 49, 50, 49, 48, 47, 46, 45, 44, 43};
const uint8_t MAX_ROW_LEN = 50;
const uint16_t NUM_LEDS = 694;
const uint8_t GRID_W = 2 * (MAX_ROW_LEN - 1) + 1;  // half-LED columns (rows are offset by 0.5)

// ---------------------------------------------------------------------------
// COLOUR THEMES  (front face -> back face)
// ---------------------------------------------------------------------------
const uint8_t NUM_THEMES = 4;
const uint8_t PALETTE_SIZE = 6;
const char *THEME_NAME[NUM_THEMES] = {"Humidity", "Temperature", "PM2.5", "CO2"};
const uint8_t PALETTES[NUM_THEMES][PALETTE_SIZE][3] = {
  { // 1 Humidity: Lilac -> Blush
    {0xC4, 0xA3, 0xD6}, {0xC9, 0xA6, 0xD1}, {0xCE, 0xA8, 0xCD},
    {0xD3, 0xAB, 0xC8}, {0xD8, 0xAE, 0xC3}, {0xDD, 0xB0, 0xBE}},
  { // 2 Temperature: Sky -> Periwinkle
    {0xA8, 0xCF, 0xDA}, {0xA9, 0xC7, 0xDE}, {0xAA, 0xBF, 0xE3},
    {0xAA, 0xB7, 0xE7}, {0xAB, 0xAF, 0xEA}, {0xAB, 0xA7, 0xEE}},
  { // 3 PM2.5: Apricot -> Burnt orange
    {0xDC, 0x8E, 0x65}, {0xDB, 0x85, 0x5D}, {0xD9, 0x7D, 0x55},
    {0xD7, 0x74, 0x4D}, {0xD5, 0x6A, 0x45}, {0xD3, 0x61, 0x3D}},
  { // 4 CO2: Amber -> Honey
    {0xED, 0xB4, 0x5E}, {0xEE, 0xB8, 0x5F}, {0xEE, 0xBC, 0x61},
    {0xEF, 0xBF, 0x62}, {0xEF, 0xC3, 0x64}, {0xF0, 0xC7, 0x65}},
};

// Palettes after gamma correction. The hex codes are screen colours;
// LEDs are linear, so without this the pastels look washed-out.
uint8_t ledPalette[NUM_THEMES][PALETTE_SIZE][3];

// Theme state
uint8_t  themeNow = 0;        // index into PALETTES
uint8_t  themePrev = 0;       // theme we are fading from
uint32_t themeChangedAt = 0;  // millis() of the last change
bool     autoCycle = true;

// Pattern state
const uint8_t PATTERN_SILK = 0, PATTERN_BULGES = 1;
const char *PATTERN_NAME[2] = {"Silk", "Bulges"};
uint8_t  pattern = START_PATTERN;
uint32_t patternChangedAt = 0;

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 3D RIBBON DATA
// (Types live up here, before any function: the Arduino IDE auto-inserts
// function prototypes above the first function, and they need these types.)
// ---------------------------------------------------------------------------
// The ribbon is sampled as a grid: NUM_SLICES along its length, SLICE_POINTS
// across. Each point is projected with perspective and "splatted" onto a
// half-LED grid, adding light. Folds and edge-on parts pile up more surface
// per LED, so they glow brighter - that is what makes it read as 3D.
const uint16_t NUM_SLICES   = 160;
const uint8_t  SLICE_POINTS_SILK   = 26;  // points across the ribbon
const uint8_t  SLICE_POINTS_BULGES = 20;

struct Slice {
  float x, y, z;     // centre line
  float ay, az;      // half cross-section vector (already tilted toward the camera)
};
Slice slices[NUM_SLICES];

struct Bulge {
  float pos, sigma, amount, sign;
};
const uint8_t MAX_BULGES = 8;

float glowGrid[NUM_ROWS][GRID_W];   // accumulated light
float colorGrid[NUM_ROWS][GRID_W];  // accumulated colour position (weighted)

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

// Phase helper: 0..1 repeating every periodMs. Integer maths keeps the
// animation smooth no matter how long the ESP32 has been running.
static inline float phase(uint32_t ms, uint32_t periodMs) {
  return (float)(ms % periodMs) / (float)periodMs;
}

// Repeatable pseudo-random number 0..1 for bulge k, property j.
static float rnd(uint32_t k, uint32_t j) {
  uint32_t h = k * 0x9E3779B1u + j * 0x85EBCA77u + 0x27d4eb2fu;
  h ^= h >> 16; h *= 0x7feb352du;
  h ^= h >> 15; h *= 0x846ca68bu;
  h ^= h >> 16;
  return (h >> 8) * (1.0f / 16777216.0f);
}

// Colour for position p (0 = front colour .. 1 = back colour) in a theme.
static void themeColor(uint8_t theme, float p, float out[3]) {
  p = clamp01(p) * (PALETTE_SIZE - 1);
  uint8_t i = (uint8_t)p;
  if (i >= PALETTE_SIZE - 1) i = PALETTE_SIZE - 2;
  float f = p - i;
  for (uint8_t k = 0; k < 3; k++) {
    out[k] = ledPalette[theme][i][k] + (ledPalette[theme][i + 1][k] - ledPalette[theme][i][k]) * f;
  }
}

// ---------------------------------------------------------------------------
// THEME CONTROL (auto cycle + Serial Monitor)
// ---------------------------------------------------------------------------
void setTheme(uint8_t theme, uint32_t ms) {
  if (theme == themeNow) return;
  themePrev = themeNow;
  themeNow = theme;
  themeChangedAt = ms;
}

void printHelp() {
  Serial.println();
  Serial.println(F("Air Wave control:"));
  Serial.println(F("  1 = Humidity   2 = Temperature   3 = PM2.5   4 = CO2"));
  Serial.println(F("  a = auto colours (change every 5 s)"));
  Serial.println(F("  p = switch pattern (Silk <-> Bulges)"));
}

void handleSerial(uint32_t ms) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '4') {
      autoCycle = false;
      setTheme(c - '1', ms);
      Serial.print(F("Colour: "));
      Serial.print(THEME_NAME[themeNow]);
      Serial.println(F("  (type a for auto)"));
    } else if (c == 'p' || c == 'P') {
      pattern = (pattern == PATTERN_SILK) ? PATTERN_BULGES : PATTERN_SILK;
      patternChangedAt = ms;
      Serial.print(F("Pattern: "));
      Serial.println(PATTERN_NAME[pattern]);
    } else if (c == 'a' || c == 'A') {
      autoCycle = true;
      themeChangedAt = ms;  // hold the current colour for a full 5 s first
      Serial.println(F("Auto: colour changes every 5 s"));
    } else if (c != '\n' && c != '\r' && c != ' ') {
      printHelp();
    }
  }
}

void updateTheme(uint32_t ms) {
  if (autoCycle && ms - themeChangedAt >= THEME_HOLD_MS) {
    setTheme((themeNow + 1) % NUM_THEMES, ms);
    Serial.print(F("Auto colour: "));
    Serial.println(THEME_NAME[themeNow]);
  }
}

// ---------------------------------------------------------------------------
// 3D RIBBON
// ---------------------------------------------------------------------------
// Find the bulges alive at time ms. A new one starts about every everyMs.
uint8_t activeBulges(uint32_t ms, uint32_t everyMs, Bulge *out) {
  uint8_t n = 0;
  uint32_t kNow = ms / everyMs;
  uint32_t lookBack = (uint32_t)(BULGE_LIFE_MAX * 1000.0f / everyMs) + 2;

  for (uint32_t i = 0; i <= lookBack && n < MAX_BULGES; i++) {
    if (kNow < i) break;
    uint32_t k = kNow - i;
    // age in seconds, from integer ms so it stays precise
    float age = (int32_t)(ms - k * everyMs) / 1000.0f
              - 0.8f * rnd(k, 0) * (everyMs / 1000.0f);
    float life = BULGE_LIFE_MIN + (BULGE_LIFE_MAX - BULGE_LIFE_MIN) * rnd(k, 1);
    if (age <= 0.0f || age >= life) continue;

    float start = 0.02f + 0.7f * rnd(k, 2);  // born anywhere, the start included
    float speed = BULGE_SPEED_MIN + (BULGE_SPEED_MAX - BULGE_SPEED_MIN) * rnd(k, 3);
    float size  = BULGE_SIZE_MIN + (BULGE_SIZE_MAX - BULGE_SIZE_MIN) * rnd(k, 4);
    float env   = sinf(3.14159265f * age / life);  // swell up, then fade away

    out[n].pos    = start + speed * age;
    out[n].sigma  = 0.07f + 0.08f * rnd(k, 5);
    out[n].amount = size * env * env;
    out[n].sign   = rnd(k, 6) > 0.5f ? 1.0f : -1.0f;  // rise up or dip down
    n++;
  }
  return n;
}

// Sum the bulges at position u along the ribbon.
static void bulgesAt(float u, const Bulge *bulges, uint8_t numBulges, float &size, float &lift) {
  size = 0.0f; lift = 0.0f;
  for (uint8_t b = 0; b < numBulges; b++) {
    float d = (u - bulges[b].pos) / bulges[b].sigma;
    if (d > 3.0f || d < -3.0f) continue;
    float g = bulges[b].amount * expf(-d * d);
    size += g;
    lift += bulges[b].sign * g;
  }
  if (size > 1.3f) size = 1.3f;
}

// Silk: a wide sheet folding gently. Two waves travel in opposite directions
// so the folds keep changing, and random swells rise through it.
void computeSilkSlices(uint32_t ms) {
  const float TWO_PI_F = 6.2831853f;
  const float wavePh   = phase(ms, SILK_WAVE_MS);
  const float crossPh  = phase(ms, SILK_CROSS_MS);
  const float ripplePh = phase(ms, SILK_RIPPLE_MS);
  const float cosTilt = cosf(SILK_TILT), sinTilt = sinf(SILK_TILT);

  Bulge bulges[MAX_BULGES];
  uint8_t numBulges = activeBulges(ms, SILK_SWELL_EVERY_MS, bulges);

  for (uint16_t i = 0; i < NUM_SLICES; i++) {
    float u = (float)i / (NUM_SLICES - 1);  // 0 = left tip, 1 = right tip
    float size, lift;
    bulgesAt(u, bulges, numBulges, size, lift);

    float taper = powf(sinf(3.14159265f * u), 0.7f);   // narrow at both tips
    float ph1 = TWO_PI_F * (1.2f * u - wavePh);         // travels left -> right
    float ph2 = TWO_PI_F * (0.7f * u + crossPh);        // travels right -> left
    float ph3 = TWO_PI_F * (2.1f * u - ripplePh);

    Slice &s = slices[i];
    s.x = SILK_START + u * SILK_LENGTH;
    s.y = SILK_WAVE * sinf(ph1) + 0.6f * sinf(ph2) + SILK_SWELL_LIFT * lift;
    s.z = 2.5f * cosf(ph1) + SILK_SWELL_POP * size;

    // The sheet lies mostly flat (going into the panel) and tips toward you;
    // where it turns edge-on it folds into a bright line.
    float theta = 0.9f + 0.8f * sinf(ph2) + 0.5f * sinf(ph3) + 0.8f * lift;
    float halfW = SILK_WIDTH * taper * (0.7f + 0.5f * size);
    float cy = halfW * sinf(theta), cz = halfW * cosf(theta);
    s.ay = cy * cosTilt - cz * sinTilt;
    s.az = cy * sinTilt + cz * cosTilt;
  }
}

// Bulges: a solid ribbon with random bulges that swell, drift and fade.
void computeBulgeSlices(uint32_t ms) {
  const float TWO_PI_F = 6.2831853f;
  const float swellPh = phase(ms, SWELL_PERIOD_MS);
  const float rollPh  = phase(ms, ROLL_PERIOD_MS);
  const float cosTilt = cosf(CAMERA_TILT), sinTilt = sinf(CAMERA_TILT);

  Bulge bulges[MAX_BULGES];
  uint8_t numBulges = activeBulges(ms, BULGE_EVERY_MS, bulges);

  for (uint16_t i = 0; i < NUM_SLICES; i++) {
    float u = (float)i / (NUM_SLICES - 1);  // 0 = left tip, 1 = right tip
    float size, lift;
    bulgesAt(u, bulges, numBulges, size, lift);

    float taper = smoothstep(0.0f, 0.1f, u) * (1.0f - smoothstep(0.88f, 1.0f, u));  // pointed tips
    float ph = TWO_PI_F * (1.1f * u - swellPh);  // gentle swell travelling left -> right

    Slice &s = slices[i];
    s.x = RIBBON_START + u * RIBBON_LENGTH;
    s.y = SWELL_HEIGHT * sinf(ph) + BULGE_LIFT * lift;
    s.z = 1.5f * cosf(ph) + BULGE_POP * size;  // bulges swell toward you

    float theta = TWO_PI_F * (0.4f * u - rollPh) + 1.3f * lift;  // twist
    float halfW = RIBBON_WIDTH * taper * (RIBBON_THIN + 0.8f * size);
    float cy = halfW * cosf(theta), cz = halfW * sinf(theta);
    // Tilt only the cross-section, so we look down onto the ribbon's face
    // while the ribbon stays vertically centred.
    s.ay = cy * cosTilt - cz * sinTilt;
    s.az = cy * sinTilt + cz * cosTilt;
  }
}

// Add one point's light into the half-LED grid (bilinear, anti-aliased).
static inline void splat(float gx, float gy, float light, float colorPos) {
  int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
  float fx = gx - x0, fy = gy - y0;
  for (uint8_t dy = 0; dy < 2; dy++) {
    int yi = y0 + dy;
    if (yi < 0 || yi >= NUM_ROWS) continue;
    float wy = dy ? fy : 1.0f - fy;
    for (uint8_t dx = 0; dx < 2; dx++) {
      int xi = x0 + dx;
      if (xi < 0 || xi >= GRID_W) continue;
      float w = (dx ? fx : 1.0f - fx) * wy * light;
      glowGrid[yi][xi] += w;
      colorGrid[yi][xi] += w * colorPos;
    }
  }
}

void renderRibbon() {
  // Light comes from the upper left, in front
  const float LX = -0.3546f, LY = -0.6079f, LZ = -0.7296f;
  const float CX = (MAX_ROW_LEN - 1) * 0.5f, CY = (NUM_ROWS - 1) * 0.5f;
  const bool silk = (pattern == PATTERN_SILK);
  const uint8_t points = silk ? SLICE_POINTS_SILK : SLICE_POINTS_BULGES;
  const float vStep = 2.0f / (points - 1);

  memset(glowGrid, 0, sizeof(glowGrid));
  memset(colorGrid, 0, sizeof(colorGrid));

  for (uint16_t i = 0; i < NUM_SLICES; i++) {
    const Slice &s = slices[i];
    const Slice &n = slices[i < NUM_SLICES - 1 ? i + 1 : i];
    const Slice &p = slices[i > 0 ? i - 1 : i];
    float span = (i > 0 && i < NUM_SLICES - 1) ? 0.5f : 1.0f;

    float u = (float)i / (NUM_SLICES - 1);

    for (uint8_t j = 0; j < points; j++) {
      float v = -1.0f + j * vStep;

      float X = s.x, Y = s.y + v * s.ay, Z = s.z + v * s.az;

      // Surface tangents: along the ribbon (ds) and across it (dv)
      float dsx = (n.x - p.x) * span;
      float dsy = ((n.y + v * n.ay) - (p.y + v * p.ay)) * span;
      float dsz = ((n.z + v * n.az) - (p.z + v * p.az)) * span;
      float dvy = s.ay * vStep, dvz = s.az * vStep;

      // Normal = ds x dv (dv has no x part); its length = surface area of this patch
      float nx = dsy * dvz - dsz * dvy;
      float ny = -dsx * dvz;
      float nz = dsx * dvy;
      float area = sqrtf(nx * nx + ny * ny + nz * nz);
      if (area < 1e-6f) continue;
      nx /= area; ny /= area; nz /= area;

      float light, colorPos;
      if (silk) {
        // See-through silk: one glowing edge (v = +1), the body fading away
        // from it, brighter where the sheet turns edge-on to you.
        float e = (v + 1.0f) * 0.5f;
        float body = SILK_BODY + (1.0f - SILK_BODY) * e * e;
        float r = (1.0f - v) / 0.12f;
        float edge = SILK_EDGE * expf(-r * r);
        float edgeOn = 0.3f + 0.7f * (1.0f - fabsf(nz));
        float tips = smoothstep(0.0f, 0.12f, u) * (1.0f - smoothstep(0.88f, 1.0f, u));
        light = (body + edge) * edgeOn * tips * SILK_GLOW;
        colorPos = u;  // colours run along the length
      } else {
        // Solid ribbon: diffuse + a glossy highlight
        float ndl = fabsf(nx * LX + ny * LY + nz * LZ);
        float n2 = ndl * ndl, n4 = n2 * n2, n8 = n4 * n4;
        float spec = n8 * n4;  // ndl^12
        float depth = clamp01(0.5f - 0.5f * Z / 9.0f);  // 1 = near, 0 = far
        light = (0.2f + 0.8f * ndl + 0.6f * spec) * (0.35f + 0.65f * depth) * BULGE_GLOW;
        // Front face (normal toward the camera) = first colour, back = last colour
        colorPos = (nz < 0.0f ? 0.0f : 0.85f) + 0.15f * (1.0f - depth);
      }

      // Perspective projection
      float f = CAMERA_DIST / (CAMERA_DIST + Z);
      float sx = CX + (X - CX) * f;
      float sy = CY + Y * f;

      splat(sx * 2.0f, sy, light * area, colorPos);
    }
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

void renderFrame(uint32_t ms) {
  if (pattern == PATTERN_SILK) computeSilkSlices(ms);
  else                         computeBulgeSlices(ms);
  renderRibbon();

  float fade = clamp01((float)(ms - themeChangedAt) / THEME_FADE_MS);
  float patternFade = smoothstep(0.0f, 1.0f, (float)(ms - patternChangedAt) / PATTERN_FADE_MS);
  uint32_t totalLevel = 0;

  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    uint8_t slotOffset = MAX_ROW_LEN - ROW_LEN[r];  // centring offset in half-LEDs

    for (uint8_t c = 0; c < ROW_LEN[r]; c++) {
      uint8_t x = slotOffset + 2 * c;
      uint16_t idx = ledIndex(r, c);

      // Each LED gathers its own half-slot plus half of each neighbour.
      float glow = glowGrid[r][x] * 0.5f, col = colorGrid[r][x] * 0.5f;
      if (x > 0)          { glow += glowGrid[r][x - 1] * 0.25f; col += colorGrid[r][x - 1] * 0.25f; }
      if (x < GRID_W - 1) { glow += glowGrid[r][x + 1] * 0.25f; col += colorGrid[r][x + 1] * 0.25f; }

      // Soft saturation: dense folds glow brighter but never clip harshly
      float look = 1.0f - expf(-glow * 2.0f * GLOW);
      float colorPos = glow > 1e-6f ? col / glow : 0.0f;

      // "look" is perceived brightness; LEDs are linear, so convert (~gamma 2.25)
      float level = look * look * sqrtf(sqrtf(look)) * patternFade * (MAX_BRIGHTNESS / 255.0f);
      if (level * 255.0f < 5.0f) {  // too dim to hold its colour
        strip.setPixelColor(idx, 0);
        continue;
      }

      float rgbNew[3], rgbOld[3];
      themeColor(themeNow, colorPos, rgbNew);
      themeColor(themePrev, colorPos, rgbOld);

      uint8_t out[3];
      for (uint8_t k = 0; k < 3; k++) {
        float v = rgbOld[k] + (rgbNew[k] - rgbOld[k]) * fade;
        out[k] = (uint8_t)(v * level + 0.5f);
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
  Serial.begin(115200);

  uint16_t start = 0;
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    rowStart[r] = start;
    start += ROW_LEN[r];
  }

  for (uint8_t t = 0; t < NUM_THEMES; t++) {
    for (uint8_t i = 0; i < PALETTE_SIZE; i++) {
      for (uint8_t k = 0; k < 3; k++) {
        ledPalette[t][i][k] = Adafruit_NeoPixel::gamma8(PALETTES[t][i][k]);
      }
    }
  }

  strip.begin();
  strip.clear();
  strip.show();

  themeChangedAt = millis();
  patternChangedAt = millis();
  printHelp();
  Serial.print(F("Pattern: "));
  Serial.println(PATTERN_NAME[pattern]);
  Serial.print(F("Colour: "));
  Serial.println(THEME_NAME[themeNow]);
}

void loop() {
  static uint32_t lastFrame = 0;
  uint32_t now = millis();

  handleSerial(now);
  updateTheme(now);

  if (now - lastFrame < 1000UL / TARGET_FPS) return;
  lastFrame = now;

#ifdef WIRING_TEST
  renderWiringTest();
#else
  renderFrame(now);
#endif

  strip.show();
}
