# Air Wave – ESP32 LED panel

3D ribbon animations for a 694-LED oval WS2812B panel, with two patterns:

- **Silk** (default): a see-through sheet of silk with one glowing edge that folds
  gently, with occasional random swells rising through it. Colours run along its length:
  the left tip is the theme's first colour and the right tip its last.
  Preview: `previews/silk-wave-preview-medium.mp4`.
- **Bulges**: a lit, solid ribbon. Bulges appear at random places, swell up, drift
  left → right and fade away. The front face is the first colour, the back face the last.
  Preview: `preview.mp4`.

Type `p` in the Serial Monitor to switch patterns. `previews/` also holds the design
options that led here.

## Colour themes

The colour changes every 5 seconds in auto mode, with a 1 s cross-fade:
Humidity → Temperature → PM2.5 → CO2 → …

Control it from the **Serial Monitor at 115200 baud**:

| Type | Colour | Palette |
|------|--------|---------|
| `1` | Humidity | Lilac → Blush (`#C4A3D6` … `#DDB0BE`) |
| `2` | Temperature | Sky → Periwinkle (`#A8CFDA` … `#ABA7EE`) |
| `3` | PM2.5 | Apricot → Burnt orange (`#DC8E65` … `#D3613D`) |
| `4` | CO2 | Amber → Honey (`#EDB45E` … `#F0C765`) |
| `a` | Auto | back to changing every 5 s |
| `p` | — | switch pattern (Silk ↔ Bulges) |

Picking `1`–`4` holds that colour until you type `a`. Colours work in both patterns.

## Panel

| Row | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|-----|---|---|---|---|---|---|---|---|---|----|----|----|----|----|----|
| LEDs | 43 | 44 | 45 | 46 | 47 | 48 | 49 | 50 | 49 | 48 | 47 | 46 | 45 | 44 | 43 |

Total 694. Data enters at the **right end of the top row**. The rows zig-zag:
row 1 runs right→left, row 2 left→right, and so on, ending at the left end of row 15.

## Setup (Arduino IDE)

1. Install the **esp32** board package (Espressif) and the **Adafruit NeoPixel** library.
2. Open `air_wave/air_wave.ino`.
3. Edit the settings at the top of the file:
   - `DATA_PIN`: the GPIO wired to the panel's DIN (default 5)
   - `MAX_BRIGHTNESS`: 0–255 (default 90)
   - `MAX_MILLIAMPS`: keep this below your 5 V supply's rating (default 4000 mA)
   - `THEME_HOLD_MS`, `THEME_FADE_MS`: time on each colour and cross-fade time
   - `START_PATTERN`: 0 = Silk, 1 = Bulges at power-on
   - Silk: `SILK_WIDTH` (thickness), `SILK_START`/`SILK_LENGTH` (tips), `SILK_WAVE`
     (wave height), `SILK_WAVE_MS`/`SILK_CROSS_MS`/`SILK_RIPPLE_MS` (speed),
     `SILK_EDGE` (edge-line brightness), `SILK_BODY` (how visible the faint side is),
     `SILK_GLOW` (overall brightness), `SILK_SWELL_*` (random swells)
   - Bulges: `BULGE_GLOW` (brightness) and the settings below
   - `RIBBON_START`, `RIBBON_LENGTH`: where the ribbon's tips are (the rest stays dark)
   - `RIBBON_WIDTH`, `RIBBON_THIN`: bulge size and thickness between bulges
   - `BULGE_EVERY_MS`, `BULGE_LIFE_*`, `BULGE_SIZE_*`, `BULGE_SPEED_*`: how often bulges
     appear, how long they live, how big they get and how fast they drift
   - `BULGE_LIFT`, `BULGE_POP`: how far bulges rise/dip and swell toward you
   - `SWELL_PERIOD_MS`, `ROLL_PERIOD_MS`: background wave and twist speed
   - `CAMERA_DIST`, `CAMERA_TILT`, `GLOW`: strength of the 3D look and brightness
4. Upload, then open the Serial Monitor at 115200 baud.

**Wiring check:** uncomment `#define WIRING_TEST`. You should see LED #0 blue at the
top-right, each row's left end red and its right end green. If the red and green ends
look jumbled, the zig-zag direction is different from what the sketch assumes.

**Hardware tips:** connect ESP32 GND to the LED supply GND. Use a ~330 Ω resistor in
the data line. With 694 LEDs, inject power at both ends of the panel.
