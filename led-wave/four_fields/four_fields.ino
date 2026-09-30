/*
 * Four Fields - 4 sensor columns on a 694-LED oval WS2812B panel
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
 * Look ("Perimeter: outside / Centre: inside"):
 *   Four columns, left -> right: Temperature | Humidity | PM2.5 | CO2,
 *   split by dark 1-LED divider lines. Each column has:
 *     - an outer border, 2 LEDs thick (top rows 1-2, bottom rows 14-15 and
 *       the 2 end LEDs of each row), in the darker colour of its palette,
 *       shimmering gently
 *     - a 1-LED dark gap
 *     - a soft centre in the lighter colour of its palette
 *   At start-up a wave flows left -> right and fills the centres one after
 *   another (about 8 s). They then stay full with soft ripples.
 *
 * Serial Monitor (115200 baud):
 *   1 = spotlight Temperature   2 = Humidity   3 = PM2.5   4 = CO2
 *       (that column glows, the others dim)
 *   a = all four equally (default)
 *   r = replay the fill wave
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

const uint8_t  BORDER_LEDS     = 2;     // border thickness (rows 1-2 / 14-15 and 2 LEDs at each row end)
const uint32_t FILL_DELAY_MS   = 500;   // pause before the fill wave starts
const uint32_t FILL_TIME_MS    = 8000;  // time for the wave to fill all four centres
const float    BORDER_LEVEL    = 0.72f; // border brightness (0..1) ...
const float    BORDER_SHIMMER  = 0.28f; // ... plus/minus this much shimmer
const float    CENTRE_LEVEL    = 0.82f; // centre brightness (0..1) ...
const float    CENTRE_RIPPLE   = 0.12f; // ... plus/minus this much ripple
const float    WAVE_EDGE_GLOW  = 0.35f; // extra glow on the wave front while filling
const float    SPOTLIGHT_DIM   = 0.5f;  // brightness of the other columns when one is spotlit
const float    SPOTLIGHT_SPEED = 0.15f; // how quickly the spotlight moves (0..1 per frame)

// ---------------------------------------------------------------------------
// PANEL GEOMETRY
// ---------------------------------------------------------------------------
const uint8_t NUM_ROWS = 15;
const uint8_t ROW_LEN[NUM_ROWS] = {43, 44, 45, 46, 47, 48, 49, 50, 49, 48, 47, 46, 45, 44, 43};
const uint8_t MAX_ROW_LEN = 50;
const uint16_t NUM_LEDS = 694;

// Columns. x = LED position from the left edge (0..49; odd-length rows sit
// half an LED in). Each divider covers [d, d+1), which is exactly 1 LED per row.
const uint8_t NUM_COLUMNS = 4;
const float DIVIDER[NUM_COLUMNS - 1] = {12.25f, 24.75f, 37.25f};
const float COLUMN_LEFT[NUM_COLUMNS]  = {0.0f, 13.25f, 25.75f, 38.25f};
const float COLUMN_RIGHT[NUM_COLUMNS] = {12.25f, 24.75f, 37.25f, 49.0f};

// ---------------------------------------------------------------------------
// COLOURS  (left -> right: Temperature, Humidity, PM2.5, CO2)
// ---------------------------------------------------------------------------
const uint8_t PALETTE_SIZE = 6;
const char *COLUMN_NAME[NUM_COLUMNS] = {"Temperature", "Humidity", "PM2.5", "CO2"};
const uint8_t PALETTES[NUM_COLUMNS][PALETTE_SIZE][3] = {
  { // Temperature: Sky -> Periwinkle
    {0xA8, 0xCF, 0xDA}, {0xA9, 0xC7, 0xDE}, {0xAA, 0xBF, 0xE3},
    {0xAA, 0xB7, 0xE7}, {0xAB, 0xAF, 0xEA}, {0xAB, 0xA7, 0xEE}},
  { // Humidity: Lilac -> Blush
    {0xC4, 0xA3, 0xD6}, {0xC9, 0xA6, 0xD1}, {0xCE, 0xA8, 0xCD},
    {0xD3, 0xAB, 0xC8}, {0xD8, 0xAE, 0xC3}, {0xDD, 0xB0, 0xBE}},
  { // PM2.5: Apricot -> Burnt orange
    {0xDC, 0x8E, 0x65}, {0xDB, 0x85, 0x5D}, {0xD9, 0x7D, 0x55},
    {0xD7, 0x74, 0x4D}, {0xD5, 0x6A, 0x45}, {0xD3, 0x61, 0x3D}},
  { // CO2: Amber -> Honey
    {0xED, 0xB4, 0x5E}, {0xEE, 0xB8, 0x5F}, {0xEE, 0xBC, 0x61},
    {0xEF, 0xBF, 0x62}, {0xEF, 0xC3, 0x64}, {0xF0, 0xC7, 0x65}},
};

// Palettes after gamma correction, reordered so index 0 is the DARKER end
// (used for the border) and index 5 the LIGHTER end (used for the centre).
uint8_t ledPalette[NUM_COLUMNS][PALETTE_SIZE][3];

// ---------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------
Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);
uint16_t rowStart[NUM_ROWS];      // index of the first LED (in wiring order) of each row

int8_t   spotlight = -1;          // -1 = all equal, 0..3 = that column spotlit
float    columnLevel[NUM_COLUMNS] = {1.0f, 1.0f, 1.0f, 1.0f};  // animated spotlight level
uint32_t fillStartedAt = 0;

// ---------------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------------
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

// Angle 0..2*pi repeating every periodMs. Integer maths keeps the animation
// smooth no matter how long the ESP32 has been running.
static inline float cycle(uint32_t ms, uint32_t periodMs) {
  return 6.2831853f * (float)(ms % periodMs) / (float)periodMs;
}

// Which column an LED at position x belongs to, or -1 on a divider line.
static int8_t columnAt(float x) {
  for (uint8_t k = 0; k < NUM_COLUMNS - 1; k++) {
    if (x >= DIVIDER[k] && x < DIVIDER[k] + 1.0f) return -1;
  }
  for (uint8_t k = 0; k < NUM_COLUMNS; k++) {
    if (x < COLUMN_RIGHT[k] || k == NUM_COLUMNS - 1) return k;
  }
  return NUM_COLUMNS - 1;
}

// Colour at position p (0 = darker end .. 1 = lighter end) of a column's palette.
static void columnColor(uint8_t k, float p, float out[3]) {
  p = clamp01(p) * (PALETTE_SIZE - 1);
  uint8_t i = (uint8_t)p;
  if (i >= PALETTE_SIZE - 1) i = PALETTE_SIZE - 2;
  float f = p - i;
  for (uint8_t c = 0; c < 3; c++) {
    out[c] = ledPalette[k][i][c] + (ledPalette[k][i + 1][c] - ledPalette[k][i][c]) * f;
  }
}

static float luminance(const uint8_t c[3]) {
  return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

// ---------------------------------------------------------------------------
// SERIAL MONITOR
// ---------------------------------------------------------------------------
void printHelp() {
  Serial.println();
  Serial.println(F("Four Fields control:"));
  Serial.println(F("  1 = Temperature   2 = Humidity   3 = PM2.5   4 = CO2   (spotlight)"));
  Serial.println(F("  a = all four equally"));
  Serial.println(F("  r = replay the fill wave"));
}

void handleSerial(uint32_t ms) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '4') {
      spotlight = c - '1';
      Serial.print(F("Spotlight: "));
      Serial.print(COLUMN_NAME[spotlight]);
      Serial.println(F("  (type a for all)"));
    } else if (c == 'a' || c == 'A') {
      spotlight = -1;
      Serial.println(F("All four columns"));
    } else if (c == 'r' || c == 'R') {
      fillStartedAt = ms;
      Serial.println(F("Replaying fill"));
    } else if (c != '\n' && c != '\r' && c != ' ') {
      printHelp();
    }
  }
}

// ---------------------------------------------------------------------------
// RENDERING
// ---------------------------------------------------------------------------
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
  // Slide each column's spotlight level toward its target
  for (uint8_t k = 0; k < NUM_COLUMNS; k++) {
    float target = (spotlight < 0 || spotlight == k) ? 1.0f : SPOTLIGHT_DIM;
    columnLevel[k] += (target - columnLevel[k]) * SPOTLIGHT_SPEED;
  }

  // Timing (each wave has its own period so nothing drifts over long uptimes)
  const float shimmerA = cycle(ms, 5712), shimmerB = cycle(ms, 8976), shimmerC = cycle(ms, 3696);
  const float rippleA  = cycle(ms, 3142), rippleB  = cycle(ms, 4833);
  const float frontWob = cycle(ms, 2856);

  // Fill progress 0..1 with a gentle start and finish
  uint32_t since = ms - fillStartedAt;
  float progress = since < FILL_DELAY_MS ? 0.0f
                 : smoothstep(0.0f, 1.0f, (float)(since - FILL_DELAY_MS) / FILL_TIME_MS);
  float p4 = progress * progress; p4 *= p4;

  uint32_t totalLevel = 0;

  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    float offset = (MAX_ROW_LEN - ROW_LEN[r]) * 0.5f;  // centre each row

    for (uint8_t c = 0; c < ROW_LEN[r]; c++) {
      uint16_t idx = ledIndex(r, c);
      float x = offset + c;
      int8_t k = columnAt(x);

      // Depth from the panel edge: 1 = outermost ring
      uint8_t depth = r + 1;
      if (NUM_ROWS - r < depth) depth = NUM_ROWS - r;
      if (c + 1 < depth) depth = c + 1;
      if (ROW_LEN[r] - c < depth) depth = ROW_LEN[r] - c;

      if (k < 0 || depth == BORDER_LEDS + 1) {  // divider line or gap ring
        strip.setPixelColor(idx, 0);
        continue;
      }

      // 0..1 across this column, for a gentle colour shift left -> right
      float across = clamp01((x - COLUMN_LEFT[k]) / (COLUMN_RIGHT[k] - COLUMN_LEFT[k]));
      float look, colorPos;

      if (depth <= BORDER_LEDS) {
        // Border: darker colour, shimmering ripples drifting across it
        float n = 0.5f * sinf(0.35f * x + 0.8f * r - shimmerA)
                + 0.3f * sinf(0.21f * x - 0.5f * r + shimmerB + 1.3f)
                + 0.2f * sinf(0.53f * x + 0.3f * r - shimmerC + 2.1f);
        look = (BORDER_LEVEL + BORDER_SHIMMER * n) * 0.9f;
        colorPos = 0.75f * across;
      } else {
        // Centre: filled by a wave front travelling left -> right
        float front = 3.0f + progress * 47.0f + 1.8f * sinf(0.5585f * r - frontWob);
        float filled = smoothstep(-1.8f, 0.6f, front - x);
        if (filled <= 0.0f) {
          strip.setPixelColor(idx, 0);
          continue;
        }
        float d = (front - x - 0.5f) / 1.3f;
        float edgeGlow = expf(-d * d) * (1.0f - p4);  // bright leading edge while filling
        float ripple = 0.5f * sinf(0.45f * x + 0.9f * r - rippleA)
                     + 0.5f * sinf(0.23f * x - 0.6f * r - rippleB + 1.7f);
        look = filled * (CENTRE_LEVEL + CENTRE_RIPPLE * ripple + WAVE_EDGE_GLOW * edgeGlow);
        colorPos = 0.25f + 0.75f * across;
      }

      look = clamp01(look) * columnLevel[k];

      // "look" is perceived brightness; LEDs are linear, so convert (~gamma 2.25)
      float level = look * look * sqrtf(sqrtf(look)) * (MAX_BRIGHTNESS / 255.0f);
      if (level * 255.0f < 5.0f) {  // too dim to hold its colour
        strip.setPixelColor(idx, 0);
        continue;
      }

      float rgb[3];
      columnColor(k, colorPos, rgb);
      uint8_t out[3];
      for (uint8_t i = 0; i < 3; i++) {
        out[i] = (uint8_t)(rgb[i] * level + 0.5f);
        totalLevel += out[i];
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

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  uint16_t start = 0;
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    rowStart[r] = start;
    start += ROW_LEN[r];
  }

  // Gamma-correct the palettes, darker end first
  for (uint8_t k = 0; k < NUM_COLUMNS; k++) {
    bool darkFirst = luminance(PALETTES[k][0]) < luminance(PALETTES[k][PALETTE_SIZE - 1]);
    for (uint8_t i = 0; i < PALETTE_SIZE; i++) {
      uint8_t src = darkFirst ? i : PALETTE_SIZE - 1 - i;
      for (uint8_t c = 0; c < 3; c++) {
        ledPalette[k][i][c] = Adafruit_NeoPixel::gamma8(PALETTES[k][src][c]);
      }
    }
  }

  strip.begin();
  strip.clear();
  strip.show();

  fillStartedAt = millis();
  printHelp();
}

void loop() {
  static uint32_t lastFrame = 0;
  uint32_t now = millis();

  handleSerial(now);

  if (now - lastFrame < 1000UL / TARGET_FPS) return;
  lastFrame = now;

#ifdef WIRING_TEST
  renderWiringTest();
#else
  renderFrame(now);
#endif

  strip.show();
}
