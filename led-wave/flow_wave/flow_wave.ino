/*
 * Flow Wave - soft glowing wave with flowing colours on a 694-LED oval WS2812B panel
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
 *   A thick, soft band of light: brightest along a gently waving centre line
 *   and fading smoothly toward the top and bottom, with brighter patches
 *   drifting left -> right. The colour flows continuously through the sensor
 *   palettes - each new colour arrives from the right and spreads left:
 *     CO2 -> PM2.5 -> Humidity -> Temperature -> (CO2 ...)
 *
 * Serial Monitor (115200 baud):
 *   1 = Humidity   2 = Temperature   3 = PM2.5   4 = CO2   (hold that colour)
 *   a = auto: colours keep flowing (default)
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

// Colour flow
const uint32_t COLOUR_CYCLE_MS = 14000;  // time to flow through all 4 palettes (lower = faster)
const float    COLOUR_SPREAD   = 0.12f;  // how much of the cycle is visible across the panel at once
const float    COLOUR_START    = 0.10f;  // where in the colour loop it starts (0.10 = CO2 gold)
const uint32_t HOLD_FADE_MS    = 1500;   // fade time when you pick / release a colour

// Wave shape and motion (periods: lower = faster)
const uint32_t WAVE_PERIOD_MS   = 9000;   // main wave travelling left -> right
const uint32_t WAVE2_PERIOD_MS  = 14000;  // slower second wave (keeps the shape changing)
const uint32_t THICK_PERIOD_MS  = 11000;  // band getting thicker / thinner along its length
const uint32_t GLOW_PERIOD_MS   = 6000;   // brighter patches drifting along the band
const float    WAVE_HEIGHT      = 1.3f;   // up/down swing of the centre line (rows)
const float    BAND_WIDTH       = 4.6f;   // how far the glow spreads above/below the centre (rows)
const float    BRIGHTNESS_FLOOR = 0.06f;  // below this the LED is off (keeps the edges clean)
const float    CORE_LEVEL       = 0.55f;  // share of the light in the bright core (rest = wide soft glow)

// ---------------------------------------------------------------------------
// PANEL GEOMETRY
// ---------------------------------------------------------------------------
const uint8_t NUM_ROWS = 15;
const uint8_t ROW_LEN[NUM_ROWS] = {43, 44, 45, 46, 47, 48, 49, 50, 49, 48, 47, 46, 45, 44, 43};
const uint8_t MAX_ROW_LEN = 50;
const uint16_t NUM_LEDS = 694;

// ---------------------------------------------------------------------------
// COLOURS
// ---------------------------------------------------------------------------
const uint8_t NUM_PALETTES = 4;
const uint8_t PALETTE_SIZE = 6;
enum { PAL_HUMIDITY, PAL_TEMPERATURE, PAL_PM25, PAL_CO2 };
const char *PALETTE_NAME[NUM_PALETTES] = {"Humidity", "Temperature", "PM2.5", "CO2"};
const uint8_t PALETTES[NUM_PALETTES][PALETTE_SIZE][3] = {
  { // Humidity: Lilac -> Blush
    {0xC4, 0xA3, 0xD6}, {0xC9, 0xA6, 0xD1}, {0xCE, 0xA8, 0xCD},
    {0xD3, 0xAB, 0xC8}, {0xD8, 0xAE, 0xC3}, {0xDD, 0xB0, 0xBE}},
  { // Temperature: Sky -> Periwinkle
    {0xA8, 0xCF, 0xDA}, {0xA9, 0xC7, 0xDE}, {0xAA, 0xBF, 0xE3},
    {0xAA, 0xB7, 0xE7}, {0xAB, 0xAF, 0xEA}, {0xAB, 0xA7, 0xEE}},
  { // PM2.5: Apricot -> Burnt orange
    {0xDC, 0x8E, 0x65}, {0xDB, 0x85, 0x5D}, {0xD9, 0x7D, 0x55},
    {0xD7, 0x74, 0x4D}, {0xD5, 0x6A, 0x45}, {0xD3, 0x61, 0x3D}},
  { // CO2: Amber -> Honey
    {0xED, 0xB4, 0x5E}, {0xEE, 0xB8, 0x5F}, {0xEE, 0xBC, 0x61},
    {0xEF, 0xBF, 0x62}, {0xEF, 0xC3, 0x64}, {0xF0, 0xC7, 0x65}},
};

// The flow runs through one long colour loop built from the palettes, in
// this order: honey -> amber -> burnt orange -> apricot -> blush -> lilac ->
// sky -> periwinkle -> (back to honey).
const uint8_t FLOW_ORDER[NUM_PALETTES]    = {PAL_CO2, PAL_PM25, PAL_HUMIDITY, PAL_TEMPERATURE};
const bool    FLOW_REVERSED[NUM_PALETTES] = {true, true, true, false};
const uint8_t FLOW_SIZE = NUM_PALETTES * PALETTE_SIZE;
uint8_t flowColors[FLOW_SIZE][3];   // gamma-corrected colour loop

// Gamma-corrected palettes (index 0 = first hex code) for holding one colour
uint8_t ledPalette[NUM_PALETTES][PALETTE_SIZE][3];

// ---------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------
Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);
uint16_t rowStart[NUM_ROWS];   // index of the first LED (in wiring order) of each row

int8_t   heldPalette = -1;     // -1 = colours flowing, 0..3 = holding that palette
float    holdAmount = 0.0f;    // 0 = flowing, 1 = fully showing the held palette
int8_t   fadePalette = -1;     // palette shown while fading back to the flow

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

// Colour at position p (any value; wraps) along the flow loop.
static void flowColor(float p, float out[3]) {
  p -= floorf(p);
  float f = p * FLOW_SIZE;
  uint8_t i = (uint8_t)f;
  if (i >= FLOW_SIZE) i = FLOW_SIZE - 1;
  uint8_t j = (i + 1) % FLOW_SIZE;
  f -= i;
  for (uint8_t c = 0; c < 3; c++) out[c] = flowColors[i][c] + (flowColors[j][c] - flowColors[i][c]) * f;
}

// Colour at position p (0..1) of one palette.
static void paletteColor(uint8_t pal, float p, float out[3]) {
  p = clamp01(p) * (PALETTE_SIZE - 1);
  uint8_t i = (uint8_t)p;
  if (i >= PALETTE_SIZE - 1) i = PALETTE_SIZE - 2;
  float f = p - i;
  for (uint8_t c = 0; c < 3; c++) {
    out[c] = ledPalette[pal][i][c] + (ledPalette[pal][i + 1][c] - ledPalette[pal][i][c]) * f;
  }
}

// ---------------------------------------------------------------------------
// SERIAL MONITOR
// ---------------------------------------------------------------------------
void printHelp() {
  Serial.println();
  Serial.println(F("Flow Wave control:"));
  Serial.println(F("  1 = Humidity   2 = Temperature   3 = PM2.5   4 = CO2   (hold colour)"));
  Serial.println(F("  a = auto: colours keep flowing"));
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '4') {
      heldPalette = c - '1';
      Serial.print(F("Hold: "));
      Serial.print(PALETTE_NAME[heldPalette]);
      Serial.println(F("  (type a for auto)"));
    } else if (c == 'a' || c == 'A') {
      if (heldPalette >= 0) fadePalette = heldPalette;
      heldPalette = -1;
      Serial.println(F("Auto: colours flowing"));
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

void renderFrame(uint32_t ms, float dt) {
  // Slide toward / away from a held colour
  float step = dt * 1000.0f / HOLD_FADE_MS;
  if (heldPalette >= 0) holdAmount = clamp01(holdAmount + step);
  else                  holdAmount = clamp01(holdAmount - step);
  int8_t showPalette = heldPalette >= 0 ? heldPalette : fadePalette;

  const float wave1 = cycle(ms, WAVE_PERIOD_MS);
  const float wave2 = cycle(ms, WAVE2_PERIOD_MS);
  const float thick = cycle(ms, THICK_PERIOD_MS);
  const float glowT = cycle(ms, GLOW_PERIOD_MS);
  const float colourPhase = (float)(ms % COLOUR_CYCLE_MS) / COLOUR_CYCLE_MS;
  const float midRow = (NUM_ROWS - 1) * 0.5f;

  uint32_t totalLevel = 0;

  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    float offset = (MAX_ROW_LEN - ROW_LEN[r]) * 0.5f;  // centre each row
    for (uint8_t c = 0; c < ROW_LEN[r]; c++) {
      uint16_t idx = ledIndex(r, c);
      float x = offset + c;
      float u = x / (MAX_ROW_LEN - 1);   // 0 = left edge, 1 = right edge

      // Gently waving centre line: two waves travelling left -> right
      float centre = midRow
                   + WAVE_HEIGHT * sinf(6.2831853f * 1.0f * u - wave1)
                   + 0.4f * sinf(6.2831853f * 0.55f * u - wave2 + 1.3f);

      // Band thickness breathes along its length
      float width = BAND_WIDTH * (0.8f + 0.25f * sinf(6.2831853f * 0.8f * u - thick + 0.7f));

      // Soft glow: a brighter core on the centre line inside a wider, dimmer glow
      float d = (r - centre) / width;
      float d2 = d * d;
      float look = CORE_LEVEL * expf(-d2 * 3.0f) + (1.0f - CORE_LEVEL) * expf(-d2 * 0.8f);

      // Brighter patches drifting left -> right along the band
      look *= 0.72f + 0.28f * sinf(6.2831853f * 1.6f * u - glowT);

      // Keep the very faint outer glow off so the edges stay clean
      look = smoothstep(BRIGHTNESS_FLOOR, BRIGHTNESS_FLOOR + 0.2f, look) * look;
      if (look < 0.02f) {
        strip.setPixelColor(idx, 0);
        continue;
      }

      // Colour: the flow loop, with new colours arriving from the right
      float rgb[3];
      flowColor(COLOUR_START + colourPhase + COLOUR_SPREAD * u, rgb);
      if (showPalette >= 0 && holdAmount > 0.0f) {
        float held[3];
        paletteColor(showPalette, u, held);  // held palette runs left -> right
        for (uint8_t i = 0; i < 3; i++) rgb[i] += (held[i] - rgb[i]) * holdAmount;
      }

      // "look" is perceived brightness; LEDs are linear, so convert (~gamma 2.25)
      float level = look * look * sqrtf(sqrtf(look)) * (MAX_BRIGHTNESS / 255.0f);
      uint8_t out[3], brightest = 0;
      for (uint8_t i = 0; i < 3; i++) {
        out[i] = (uint8_t)(rgb[i] * level + 0.5f);
        if (out[i] > brightest) brightest = out[i];
      }
      if (brightest < 6) {  // too dim to hold its colour (avoids red / blue specks)
        strip.setPixelColor(idx, 0);
        continue;
      }
      totalLevel += out[0] + out[1] + out[2];
      strip.setPixelColor(idx, out[0], out[1], out[2]);
    }
  }

  if (holdAmount <= 0.0f) fadePalette = -1;
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

  for (uint8_t p = 0; p < NUM_PALETTES; p++) {
    for (uint8_t i = 0; i < PALETTE_SIZE; i++) {
      for (uint8_t c = 0; c < 3; c++) {
        ledPalette[p][i][c] = Adafruit_NeoPixel::gamma8(PALETTES[p][i][c]);
      }
    }
  }
  // Build the colour loop
  for (uint8_t k = 0; k < NUM_PALETTES; k++) {
    for (uint8_t i = 0; i < PALETTE_SIZE; i++) {
      uint8_t src = FLOW_REVERSED[k] ? PALETTE_SIZE - 1 - i : i;
      for (uint8_t c = 0; c < 3; c++) {
        flowColors[k * PALETTE_SIZE + i][c] = ledPalette[FLOW_ORDER[k]][src][c];
      }
    }
  }

  strip.begin();
  strip.clear();
  strip.show();
  printHelp();
}

void loop() {
  static uint32_t lastFrame = 0;
  uint32_t now = millis();

  handleSerial();

  if (now - lastFrame < 1000UL / TARGET_FPS) return;
  float dt = (lastFrame == 0) ? 0.0f : (now - lastFrame) / 1000.0f;
  lastFrame = now;

#ifdef WIRING_TEST
  renderWiringTest();
#else
  renderFrame(now, dt);
#endif

  strip.show();
}
