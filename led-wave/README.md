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

## Four Fields (`four_fields/four_fields.ino`)

A separate sketch with four sensor columns, left to right: **Temperature | Humidity |
PM2.5 | CO2**, split by dark 1-LED divider lines ("Perimeter: outside / Centre: inside").
Each column has a 2-LED border (rows 1–2, 14–15 and the 2 LEDs at each row end) in the
darker colour of its palette, shimmering gently. Inside that is a 1-LED dark gap, then a
soft centre in the lighter colour. At start-up a wave flows left → right and fills the
centres one after another (about 8 s), and then they stay full with soft ripples.
`four_fields/preview.mp4` shows the sketch's actual output.

Serial Monitor (115200 baud):

| Type | Does |
|------|------|
| `1`–`4` | spotlight Temperature / Humidity / PM2.5 / CO2 (the others dim) |
| `a` | all four equally |
| `r` | replay the fill wave |

Settings at the top of the file: `DATA_PIN`, `MAX_BRIGHTNESS`, `MAX_MILLIAMPS`,
`BORDER_LEDS`, `FILL_TIME_MS`, `BORDER_LEVEL`/`BORDER_SHIMMER`, `CENTRE_LEVEL`/
`CENTRE_RIPPLE`, `WAVE_EDGE_GLOW`, `SPOTLIGHT_DIM`.

## Air Show (`air_show/air_show.ino`)

A looping show with a smooth 1 s cross-fade between steps:

| Step | Time | Shows |
|------|------|-------|
| 1 | 20 s | Swell wave: a glowing ribbon riding an ocean swell left → right (uneven waves, soft edges, slow roll, dotted mesh); colour changes every 5 s: Humidity → Temperature → PM2.5 → CO2 |
| 2 | 10 s | Four Fields: Temperature / Humidity / PM2.5 / CO2 columns filling with a wave |
| 3 | 8 s | Swell wave in dark orange → light orange |

Serial Monitor (115200 baud): `1` / `2` / `3` hold that step, `a` restarts the whole show.
Timing is set by `STEP_MS`, `CROSSFADE_MS` and `WAVE_COLOUR_MS` at the top of the file.
`air_show/preview.mp4` shows the sketch's actual output.

## Swell Show (`swell_show/swell_show.ino`)

A calmer version of Air Show without the orange step. The loop is: swell wave (20 s,
colour every 5 s: Humidity → Temperature → PM2.5 → CO2) → Four Fields (10 s) → repeat,
with 2 s cross-fades and 2 s colour fades. Motion is about 1.5–2× slower, with softer
mesh lines, gentler small waves and rounder crests. Serial Monitor: `1` holds the swell
wave, `2` holds Four Fields, `a` restarts the show. `swell_show/preview.mp4` shows its output.

## Flow Wave (`flow_wave/flow_wave.ino`)

A thick, soft band of light, with a bright core and a wide dim glow, gently waving
left → right, with brighter patches drifting along it. The colour flows continuously
through the sensor palettes, with each new colour arriving from the right:
CO2 gold → PM2.5 orange/peach → Humidity pink/lilac → Temperature sky/periwinkle,
about 14 s per full cycle. Serial Monitor: `1`–`4` hold Humidity / Temperature / PM2.5 /
CO2 (with a smooth fade), and `a` lets the colours flow again. `flow_wave/preview.mp4`
shows its output, and `compare-with-reference.png` compares it with the reference video.

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
