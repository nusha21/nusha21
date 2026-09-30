/*
 * Air Show - looping LED show for a 694-LED oval WS2812B panel
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
 * The show (loops, with a smooth cross-fade between steps):
 *   Step 1  Dotted wave   20 s  a 3D landscape of dots with a wave rising in
 *                               the centre; colour changes every 5 s:
 *                               Humidity -> Temperature -> PM2.5 -> CO2
 *   Step 2  Four Fields   10 s  Temperature | Humidity | PM2.5 | CO2 columns,
 *                               each with a border, a gap and a centre that a
 *                               wave fills left -> right
 *   Step 3  Orange wave    8 s  the dotted wave in dark / light orange
 *
 * Serial Monitor (115200 baud):
 *   1 = hold step 1 (dotted wave, colours keep changing)
 *   2 = hold step 2 (Four Fields)
 *   3 = hold step 3 (orange wave)
 *   a = auto: restart the whole show from the beginning (default)
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

// Uncomment to run a wiring check instead of the show:
// LED #0 blue, left end of every row red, right end green.
// #define WIRING_TEST

// Show timing
const uint32_t STEP_MS[3]      = {20000, 10000, 8000};  // dotted wave, Four Fields, orange wave
const uint32_t CROSSFADE_MS    = 1000;  // blend between steps
const uint32_t WAVE_COLOUR_MS  = 5000;  // dotted wave: time on each colour
const uint32_t WAVE_FADE_MS    = 1000;  // dotted wave: colour fade time

// Dotted wave
const float WAVE_HUMP       = 3.2f;   // height of the ridge rising in the centre
const float WAVE_SWELL      = 1.8f;   // extra swell height in the centre
const float WAVE_GLOW       = 2.4f;   // brightness / density of the dots

// Four Fields
const uint8_t  BORDER_LEDS    = 2;     // border thickness
const uint32_t FILL_DELAY_MS  = 500;   // pause before the fill wave starts
const uint32_t FILL_TIME_MS   = 8000;  // time for the wave to fill all four centres
const float    BORDER_LEVEL   = 0.72f;
const float    BORDER_SHIMMER = 0.28f;
const float    CENTRE_LEVEL   = 0.82f;
const float    CENTRE_RIPPLE  = 0.12f;
const float    WAVE_EDGE_GLOW = 0.35f;

// ---------------------------------------------------------------------------
// PANEL GEOMETRY
// ---------------------------------------------------------------------------
const uint8_t NUM_ROWS = 15;
const uint8_t ROW_LEN[NUM_ROWS] = {43, 44, 45, 46, 47, 48, 49, 50, 49, 48, 47, 46, 45, 44, 43};
const uint8_t MAX_ROW_LEN = 50;
const uint16_t NUM_LEDS = 694;
const uint8_t GRID_W = 2 * (MAX_ROW_LEN - 1) + 1;  // half-LED columns (rows are offset by 0.5)

// Four Fields columns. x = LED position from the left edge (0..49).
// Each divider covers [d, d+1), which is exactly 1 LED per row.
const uint8_t NUM_COLUMNS = 4;
const float DIVIDER[NUM_COLUMNS - 1] = {12.25f, 24.75f, 37.25f};
const float COLUMN_LEFT[NUM_COLUMNS]  = {0.0f, 13.25f, 25.75f, 38.25f};
const float COLUMN_RIGHT[NUM_COLUMNS] = {12.25f, 24.75f, 37.25f, 49.0f};

// ---------------------------------------------------------------------------
// COLOURS
// ---------------------------------------------------------------------------
const uint8_t PALETTE_SIZE = 6;
const uint8_t NUM_PALETTES = 5;
enum { PAL_HUMIDITY, PAL_TEMPERATURE, PAL_PM25, PAL_CO2, PAL_ORANGE };
const char *PALETTE_NAME[NUM_PALETTES] = {"Humidity", "Temperature", "PM2.5", "CO2", "Orange"};
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
  { // Orange: dark orange -> light orange (step 3)
    {0xC8, 0x50, 0x14}, {0xD8, 0x66, 0x22}, {0xE6, 0x7E, 0x34},
    {0xF0, 0x96, 0x4A}, {0xF6, 0xAE, 0x68}, {0xF9, 0xC4, 0x8A}},
};
// Four Fields column order, left -> right
const uint8_t COLUMN_PALETTE[NUM_COLUMNS] = {PAL_TEMPERATURE, PAL_HUMIDITY, PAL_PM25, PAL_CO2};
// Dotted wave colour order in step 1
const uint8_t WAVE_ORDER[4] = {PAL_HUMIDITY, PAL_TEMPERATURE, PAL_PM25, PAL_CO2};

// Palettes after gamma correction, reordered so index 0 is the DARKER end.
uint8_t ledPalette[NUM_PALETTES][PALETTE_SIZE][3];

// ---------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------
Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);
uint16_t rowStart[NUM_ROWS];      // index of the first LED (in wiring order) of each row

const uint8_t NUM_STEPS = 3;
const char *STEP_NAME[NUM_STEPS] = {"Dotted wave", "Four Fields", "Orange wave"};
bool     autoShow = true;
uint8_t  step = 0, prevStep = 2;
uint32_t stepStartedAt = 0;
uint32_t prevStepStartedAt = 0;
bool     prevWasAuto = true;      // the step we are fading out of was part of the auto show
bool     firstStep = true;        // no cross-fade on the very first step after power-on

// Frame buffers: final LED values (0..255 floats) for the current and the
// previous step, so steps can cross-fade.
float frameA[NUM_LEDS][3];
float frameB[NUM_LEDS][3];

// Dotted wave light buffers (half-LED grid)
float glowGrid[NUM_ROWS][GRID_W];
float heightGrid[NUM_ROWS][GRID_W];

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

// Colour at position p (0 = darker end .. 1 = lighter end) of a palette.
static void paletteColor(uint8_t pal, float p, float out[3]) {
  p = clamp01(p) * (PALETTE_SIZE - 1);
  uint8_t i = (uint8_t)p;
  if (i >= PALETTE_SIZE - 1) i = PALETTE_SIZE - 2;
  float f = p - i;
  for (uint8_t c = 0; c < 3; c++) {
    out[c] = ledPalette[pal][i][c] + (ledPalette[pal][i + 1][c] - ledPalette[pal][i][c]) * f;
  }
}

static float luminance(const uint8_t c[3]) {
  return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

// "look" is perceived brightness 0..1; LEDs are linear, so convert (~gamma 2.25).
static inline float ledLevel(float look) {
  return look * look * sqrtf(sqrtf(look)) * (MAX_BRIGHTNESS / 255.0f);
}

// ---------------------------------------------------------------------------
// STEP 1 & 3: DOTTED WAVE
// ---------------------------------------------------------------------------
// A grid of dots on a 3D wavy surface, seen from above in perspective.
// Only the far part of the surface is drawn: there the dots are dense enough
// to form clean lines on 15 rows.
const uint8_t DOTS_ACROSS = 52;
const uint8_t DOTS_DEEP   = 13;

// Writes the frame into out[][3]. Colour fades from palette palFrom to palTo by fade.
void renderDottedWave(uint32_t ms, uint8_t palFrom, uint8_t palTo, float fade, float out[][3]) {
  memset(glowGrid, 0, sizeof(glowGrid));
  memset(heightGrid, 0, sizeof(heightGrid));

  const float a1 = cycle(ms, 5712);   // main swell, left -> right
  const float a2 = cycle(ms, 10472);  // slower cross swell
  const float a3 = cycle(ms, 15708);  // ridge drifting forward / back
  const float a4 = cycle(ms, 8976);   // ridge swelling up / down
  const float ridgeDepth = 15.0f + 2.0f * sinf(a3);
  const float ridgeSize  = 0.75f + 0.25f * sinf(a4);

  for (uint8_t j = 0; j < DOTS_DEEP; j++) {
    float w = 26.0f * j / (DOTS_DEEP - 1);           // 0 = near, 26 = far
    float nearFade = smoothstep(8.0f, 13.0f, w);     // near rows are too sparse -> hidden
    if (nearFade <= 0.0f) continue;
    float rw = (w - ridgeDepth) / 5.0f;
    float ridge = expf(-rw * rw);
    float z = w + 7.0f, f = 16.0f / z;

    for (uint8_t i = 0; i < DOTS_ACROSS; i++) {
      float u = -38.0f + 76.0f * i / (DOTS_ACROSS - 1);
      float cu = u / 17.0f;
      float centre = expf(-cu * cu);                 // waves rise in the middle of the panel

      float h = (0.8f + WAVE_SWELL * centre) * sinf(0.13f * u + 0.22f * w - a1)
              + 0.7f * sinf(0.17f * u - 0.10f * w - a2 + 1.0f)
              + WAVE_HUMP * ridge * centre * ridgeSize;

      float sx = 24.5f + u * f * 1.25f;
      float sy = 2.6f + (6.5f - h) * f * 1.9f;

      float hn = clamp01((h + 3.0f) / 8.0f);         // 0 = trough, 1 = crest
      float light = (0.4f + 0.6f * powf(hn, 1.3f)) * (0.65f + 0.35f * w / 26.0f)
                  * nearFade * (1.0f - smoothstep(10.5f, 12.5f, sy));
      if (light <= 0.0f) continue;

      // Blend the dot smoothly into the 4 nearest half-LED slots
      float gx = sx * 2.0f, gy = sy;
      int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
      float fx = gx - x0, fy = gy - y0;
      for (uint8_t dy = 0; dy < 2; dy++) {
        int yi = y0 + dy;
        if (yi < 0 || yi >= NUM_ROWS) continue;
        float wy = dy ? fy : 1.0f - fy;
        for (uint8_t dx = 0; dx < 2; dx++) {
          int xi = x0 + dx;
          if (xi < 0 || xi >= GRID_W) continue;
          float wgt = (dx ? fx : 1.0f - fx) * wy * light;
          glowGrid[yi][xi] += wgt;
          heightGrid[yi][xi] += wgt * hn;
        }
      }
    }
  }

  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    uint8_t slotOffset = MAX_ROW_LEN - ROW_LEN[r];
    for (uint8_t c = 0; c < ROW_LEN[r]; c++) {
      uint16_t idx = ledIndex(r, c);
      uint8_t x = slotOffset + 2 * c;
      float g = glowGrid[r][x] * 0.5f, hsum = heightGrid[r][x] * 0.5f;
      if (x > 0)          { g += glowGrid[r][x - 1] * 0.25f; hsum += heightGrid[r][x - 1] * 0.25f; }
      if (x < GRID_W - 1) { g += glowGrid[r][x + 1] * 0.25f; hsum += heightGrid[r][x + 1] * 0.25f; }

      float look = 1.0f - expf(-g * WAVE_GLOW);           // soft saturation
      float k = clamp01((look - 0.10f) / 0.15f);          // faint light fades out fully
      look *= k * k * (3.0f - 2.0f * k);
      if (look < 0.02f) { out[idx][0] = out[idx][1] = out[idx][2] = 0.0f; continue; }
      look = powf(look, 0.75f);

      float hpos = g > 1e-6f ? hsum / g : 0.0f;           // troughs dark end, crests light end
      float a[3], b[3];
      paletteColor(palFrom, hpos, a);
      paletteColor(palTo, hpos, b);
      float level = ledLevel(look);
      for (uint8_t i = 0; i < 3; i++) out[idx][i] = (a[i] + (b[i] - a[i]) * fade) * level;
    }
  }
}

// ---------------------------------------------------------------------------
// STEP 2: FOUR FIELDS
// ---------------------------------------------------------------------------
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

void renderFourFields(uint32_t ms, uint32_t fillStartedAt, float out[][3]) {
  const float shimmerA = cycle(ms, 5712), shimmerB = cycle(ms, 8976), shimmerC = cycle(ms, 3696);
  const float rippleA  = cycle(ms, 3142), rippleB  = cycle(ms, 4833);
  const float frontWob = cycle(ms, 2856);

  uint32_t since = ms - fillStartedAt;
  float progress = since < FILL_DELAY_MS ? 0.0f
                 : smoothstep(0.0f, 1.0f, (float)(since - FILL_DELAY_MS) / FILL_TIME_MS);
  float p4 = progress * progress; p4 *= p4;

  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    float offset = (MAX_ROW_LEN - ROW_LEN[r]) * 0.5f;
    for (uint8_t c = 0; c < ROW_LEN[r]; c++) {
      uint16_t idx = ledIndex(r, c);
      out[idx][0] = out[idx][1] = out[idx][2] = 0.0f;
      float x = offset + c;
      int8_t k = columnAt(x);

      // Depth from the panel edge: 1 = outermost ring
      uint8_t depth = r + 1;
      if (NUM_ROWS - r < depth) depth = NUM_ROWS - r;
      if (c + 1 < depth) depth = c + 1;
      if (ROW_LEN[r] - c < depth) depth = ROW_LEN[r] - c;
      if (k < 0 || depth == BORDER_LEDS + 1) continue;  // divider line or gap ring

      float across = clamp01((x - COLUMN_LEFT[k]) / (COLUMN_RIGHT[k] - COLUMN_LEFT[k]));
      float look, colorPos;
      if (depth <= BORDER_LEDS) {
        float n = 0.5f * sinf(0.35f * x + 0.8f * r - shimmerA)
                + 0.3f * sinf(0.21f * x - 0.5f * r + shimmerB + 1.3f)
                + 0.2f * sinf(0.53f * x + 0.3f * r - shimmerC + 2.1f);
        look = (BORDER_LEVEL + BORDER_SHIMMER * n) * 0.9f;
        colorPos = 0.75f * across;
      } else {
        float front = 3.0f + progress * 47.0f + 1.8f * sinf(0.5585f * r - frontWob);
        float filled = smoothstep(-1.8f, 0.6f, front - x);
        if (filled <= 0.0f) continue;
        float d = (front - x - 0.5f) / 1.3f;
        float edgeGlow = expf(-d * d) * (1.0f - p4);
        float ripple = 0.5f * sinf(0.45f * x + 0.9f * r - rippleA)
                     + 0.5f * sinf(0.23f * x - 0.6f * r - rippleB + 1.7f);
        look = filled * (CENTRE_LEVEL + CENTRE_RIPPLE * ripple + WAVE_EDGE_GLOW * edgeGlow);
        colorPos = 0.25f + 0.75f * across;
      }

      float rgb[3];
      paletteColor(COLUMN_PALETTE[k], colorPos, rgb);
      float level = ledLevel(clamp01(look));
      for (uint8_t i = 0; i < 3; i++) out[idx][i] = rgb[i] * level;
    }
  }
}

// ---------------------------------------------------------------------------
// SHOW
// ---------------------------------------------------------------------------
// endOfAutoStep: the step is fading out at the end of its auto time, so the
// dotted wave keeps its last colour instead of starting the next colour round.
void renderStep(uint8_t s, uint32_t ms, uint32_t startedAt, bool endOfAutoStep, float out[][3]) {
  if (s == 0) {
    // Colour changes every WAVE_COLOUR_MS with a short fade
    uint32_t t = ms - startedAt;
    if (endOfAutoStep && t >= STEP_MS[0]) t = STEP_MS[0] - 1;
    uint32_t n = t / WAVE_COLOUR_MS;
    uint8_t cur = WAVE_ORDER[n % 4];
    uint8_t prev = WAVE_ORDER[(n + 3) % 4];
    float fade = n == 0 ? 1.0f : clamp01((float)(t % WAVE_COLOUR_MS) / WAVE_FADE_MS);
    renderDottedWave(ms, prev, cur, fade, out);
  } else if (s == 1) {
    renderFourFields(ms, startedAt, out);
  } else {
    renderDottedWave(ms, PAL_ORANGE, PAL_ORANGE, 1.0f, out);
  }
}

void goToStep(uint8_t s, uint32_t ms, bool wasAuto) {
  prevStep = step;
  prevStepStartedAt = stepStartedAt;
  prevWasAuto = wasAuto;
  step = s;
  stepStartedAt = ms;
  Serial.print(autoShow ? F("Auto: ") : F("Hold: "));
  Serial.println(STEP_NAME[step]);
}

void printHelp() {
  Serial.println();
  Serial.println(F("Air Show control:"));
  Serial.println(F("  1 = hold dotted wave (colours keep changing)"));
  Serial.println(F("  2 = hold Four Fields"));
  Serial.println(F("  3 = hold orange wave"));
  Serial.println(F("  a = auto: restart the whole show"));
}

void handleSerial(uint32_t ms) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '3') {
      autoShow = false;
      firstStep = false;
      goToStep(c - '1', ms, false);
    } else if (c == 'a' || c == 'A') {
      autoShow = true;
      firstStep = false;
      goToStep(0, ms, false);
    } else if (c != '\n' && c != '\r' && c != ' ') {
      printHelp();
    }
  }
}

void updateShow(uint32_t ms) {
  if (autoShow && ms - stepStartedAt >= STEP_MS[step]) {
    firstStep = false;
    goToStep((step + 1) % NUM_STEPS, ms, true);
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
  renderStep(step, ms, stepStartedAt, false, frameA);

  // Cross-fade from the previous step during the first CROSSFADE_MS
  float blend = firstStep ? 1.0f : clamp01((float)(ms - stepStartedAt) / CROSSFADE_MS);
  if (blend < 1.0f) {
    // The previous step keeps running while it fades out
    renderStep(prevStep, ms, prevStepStartedAt, prevWasAuto, frameB);
    blend = smoothstep(0.0f, 1.0f, blend);
  }

  uint32_t totalLevel = 0;
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    uint8_t out[3], brightest = 0;
    for (uint8_t c = 0; c < 3; c++) {
      float v = frameA[i][c];
      if (blend < 1.0f) v = frameB[i][c] + (v - frameB[i][c]) * blend;
      out[c] = (uint8_t)(v + 0.5f);
      if (out[c] > brightest) brightest = out[c];
    }
    // Too dim to hold its colour (e.g. dim orange shows as red specks): switch off
    if (brightest < 6) out[0] = out[1] = out[2] = 0;
    totalLevel += out[0] + out[1] + out[2];
    strip.setPixelColor(i, out[0], out[1], out[2]);
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
  for (uint8_t p = 0; p < NUM_PALETTES; p++) {
    bool darkFirst = luminance(PALETTES[p][0]) < luminance(PALETTES[p][PALETTE_SIZE - 1]);
    for (uint8_t i = 0; i < PALETTE_SIZE; i++) {
      uint8_t src = darkFirst ? i : PALETTE_SIZE - 1 - i;
      for (uint8_t c = 0; c < 3; c++) {
        ledPalette[p][i][c] = Adafruit_NeoPixel::gamma8(PALETTES[p][src][c]);
      }
    }
  }

  strip.begin();
  strip.clear();
  strip.show();

  stepStartedAt = millis();
  printHelp();
  Serial.print(F("Auto: "));
  Serial.println(STEP_NAME[step]);
}

void loop() {
  static uint32_t lastFrame = 0;
  uint32_t now = millis();

  handleSerial(now);
  updateShow(now);

  if (now - lastFrame < 1000UL / TARGET_FPS) return;
  lastFrame = now;

#ifdef WIRING_TEST
  renderWiringTest();
#else
  renderFrame(now);
#endif

  strip.show();
}
