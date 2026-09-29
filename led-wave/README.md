# Humidity Wave – ESP32 LED panel

Ocean-swell ribbon animation for a 694-LED oval WS2812B panel. A dotted-mesh ribbon
rides a swell left → right: uneven wave heights, sharp crests, flat troughs and soft
edges. It slowly rolls over as it goes, and the exact pattern practically never repeats.
The front face of the ribbon is Lilac (`#C4A3D6`) and the back face is Blush (`#DDB0BE`),
with the rest of the Humidity palette blended in as it rolls.

`preview.mp4` shows a render of the sketch's actual output.

## Panel

| Row | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|-----|---|---|---|---|---|---|---|---|---|----|----|----|----|----|----|
| LEDs | 43 | 44 | 45 | 46 | 47 | 48 | 49 | 50 | 49 | 48 | 47 | 46 | 45 | 44 | 43 |

Total 694. Data enters at the **right end of the top row**. The rows zig-zag:
row 1 runs right→left, row 2 left→right, and so on, ending at the left end of row 15.

## Setup (Arduino IDE)

1. Install the **esp32** board package (Espressif) and the **Adafruit NeoPixel** library.
2. Open `humidity_wave/humidity_wave.ino`.
3. Edit the settings at the top of the file:
   - `DATA_PIN`: the GPIO wired to the panel's DIN (default 5)
   - `MAX_BRIGHTNESS`: 0–255 (default 90)
   - `MAX_MILLIAMPS`: keep this below your 5 V supply's rating (default 4000 mA)
   - `SWELL_PERIOD_MS`, `CHOP_PERIOD_MS`, `ROLL_PERIOD_MS`: animation speed (lower = faster)
   - `SWELL_AMPLITUDE`, `CHOP_AMPLITUDE`, `CREST_SHARPNESS`: wave height and shape
   - `RIBBON_HALF_W`, `RIBBON_MIN_W`, `EDGE_SOFTNESS`, `HALO`: ribbon thickness and edges
   - `MESH_STRENGTH`, `MESH_SPACING`: how visible the dotted mesh is (0 = solid ribbon)
4. Upload.

**Wiring check:** uncomment `#define WIRING_TEST`. You should see LED #0 blue at the
top-right, each row's left end red and its right end green. If the red and green ends
look jumbled, the zig-zag direction is different from what the sketch assumes.

**Hardware tips:** connect ESP32 GND to the LED supply GND. Use a ~330 Ω resistor in
the data line. With 694 LEDs, inject power at both ends of the panel.
