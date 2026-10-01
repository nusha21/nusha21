# BREETHR 694 – Curved Wave v3

Firmware for the 694-LED BREETHR panel (ESP32 + WS2812B).
Sketch: `breethr_694_curved_wave_v3.ino` (keep it inside a folder with the same name).

## What was asked

1. Start from `breethr_694_curved_wave__revuised.ino`, change nothing else.
2. Remove the dust (SPS30) and CO2 (SCD4x) sensor code.
3. Keep the curved wave animation; the wave moves slowly from left to right.
4. Outdoor colours first, then the four indoor parameters, each as a new wave from the left.
5. Make it slower, with a black screen between outdoor and indoor.
6. Fix the colours (they showed green / looked dull) and allow manual hex changes.

## What the panel does (one cycle = 78 s, then repeats)

| # | Step | Colour | Time |
|---|------|--------|------|
| 1 | Outdoor HIGH – fills in from the **left**, starting from black | `COLOR_OUTDOOR_HIGH` | 10 s |
| 2 | Outdoor LOW – pushes in from the **right** over step 1 | `COLOR_OUTDOOR_LOW` | 10 s fill + 10 s hold |
| 3 | Fade to **black screen** | – | 2 s fade + 2 s black |
| 4 | Temperature – fills in from the left, from black | Temperature colours | 10 s |
| 5 | Humidity – flows in from the left over Temperature | Humidity colours | 10 s |
| 6 | PM 2.5 – flows in from the left over Humidity | PM 2.5 colours | 10 s |
| 7 | CO2 – flows in from the left over PM 2.5 | CO2 colours | 10 s |
| 8 | Fade to **black screen**, then back to step 1 | – | 2 s fade + 2 s black |

Throughout, the wave ribbons keep moving left → right.

## Where to change things

### Colours (lines 154–169)

Write a hex colour as `0x` + 6 characters, no `#` (e.g. `#EDAB7A` → `0xEDAB7A`).

| Line | Setting | Current |
|------|---------|---------|
| 154 | `COLOR_OUTDOOR_HIGH` – outdoor first wave | `0xDB6E2A` orange |
| 155 | `COLOR_OUTDOOR_LOW` – outdoor second wave | `0xC4A3D6` lilac |
| 160 / 166 | Temperature left / right | `0xA8CFDA` → `0xABA7EE` |
| 161 / 167 | Humidity left / right | `0xD3ABC8` → `0xDDB0BE` |
| 162 / 168 | PM 2.5 left / right | `0xEDAB7A` → `0xEDAB7A` |
| 163 / 169 | CO2 left / right | `0xF5D969` → `0xF5D969` (solid yellow) |

Indoor colours fade from the LEFT value to the RIGHT value across the panel.
For a solid colour, put the same value in both.

### Colour look (lines 40, 61, 64, 68)

| Line | Setting | Current | Notes |
|------|---------|---------|-------|
| 40 | `COLOR_ORDER` | `GRB` | If red shows as green (or the reverse), change to `RGB`. |
| 61 | `BRIGHTNESS` | `220` | 0–255. |
| 64 | `COLOR_GAMMA` | `2.4f` | `1.0f` = LEDs show your hex codes exactly. Higher = deeper, stronger colours. |
| 68 | `LED_CORRECTION` | `0xFFB0F0` | Removes the green tint (WS2812B green is too strong). Lower the middle byte (`B0`) for even less green; `0xFFFFFF` = off. |
| 69 | `MAX_MILLIAMPS` | `3000` | Power limit; raise only if your 5 V supply can deliver more. |

### Timing (lines 86–114, milliseconds: 1000 = 1 s)

| Line | Setting | Current | What it does |
|------|---------|---------|--------------|
| 86 | `WAVE_A_PERIOD_MS` | 9900 | Main ribbon speed (lower = faster) |
| 87 | `WAVE_B_PERIOD_MS` | 12300 | Second ribbon speed (lower = faster) |
| 93 | `APRICOT_FILL_MS` | 10000 | Outdoor HIGH fill time (name kept from the old apricot colour) |
| 94 | `APRICOT_HOLD_MS` | 0 | Extra time on full outdoor HIGH colour |
| 97 | `LILAC_FILL_MS` | 10000 | Outdoor LOW fill time |
| 98 | `LILAC_HOLD_MS` | 10000 | Time on full outdoor LOW colour |
| 101 | `BLACK_FADE_MS` | 2000 | Fade to black before Temperature |
| 102 | `BLACK_HOLD_MS` | 2000 | Black screen before Temperature |
| 106 | `INDOOR_FILL_MS` | 10000 | Fill time for each indoor parameter |
| 107 | `INDOOR_HOLD_MS` | 0 | Extra time on each full indoor colour |
| 110 | `END_FADE_MS` | 2000 | Fade to black after CO2 |
| 111 | `END_BLACK_MS` | 2000 | Black screen after CO2 |
| 114 | `COLOR_EDGE` | 0.12 | Softness of the edge between two colours |

## What was removed / kept

- **Removed:** all SCD4x (CO2) and SPS30 (dust) code – libraries, I2C setup, reading and retry logic.
- **Kept unchanged:** WiFi, server polling (`/api/state`), relay control, saved settings.
  The board still posts to `/api/readings` every 10 s, using the last saved values
  (there are no live sensor readings any more).

## If a change does not show on the panel

1. Check you are editing the newest file (line 64 must contain `COLOR_GAMMA`).
2. Quick test: set line 154 to `0xFF0000` and upload.
   - First wave is **red** → uploads work; put your colours back.
   - First wave is **green** → change line 40 to `RGB`.
   - **No change** → the upload is not reaching the board; check that the
     Arduino window ends with "Hard resetting via RTS pin…" / "Done uploading".
3. "Multiple libraries were found for WiFi.h" is only a notice, not an error.

## Other files

- `preview.mp4` – screen preview of the sequence (colours from an earlier version).
- `../breethr_color_test/breethr_color_test.ino` – optional panel test
  (red/green/blue check and gamma comparison).
