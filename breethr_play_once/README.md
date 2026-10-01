# BREETHR 694 – Play once, replay from the web

Firmware for the 694-LED BREETHR panel (ESP32 + WS2812B).
Sketch: `breethr_play_once.ino` (keep it inside a folder with the same name).
Web app: `../breethr_play_web` (deploy to Vercel, see its README).

## What it does

1. **On boot** the panel plays the curved-wave animation **once** (78 s), then goes black.
   The animation is the same as `breethr_694_curved_wave_v3`, including its colours
   (first wave `#D68354`, CO2 `#F5E8AE`) and the green-tint fix.
2. **Wi‑Fi**, while the animation plays:
   - First tries the built-in network (`WIFI_SSID` / `WIFI_PASSWORD`, lines 35–36).
   - Then the network saved from the setup page.
   - Networks that are out of range are skipped.
   - If none connects, the panel opens its own hotspot **`Breethr ABCDE`**.
     `ABCDE` is the last 5 digits of the panel's MAC address (shown in Serial Monitor at boot).
3. **Once online** the panel checks the web app every 3 s. Each press of **PLAY**
   restarts the animation from the beginning and plays it once.

## Setting up Wi‑Fi (setup hotspot)

1. On a phone, join the Wi‑Fi network `Breethr ABCDE` (no password).
2. The setup page opens by itself. If it doesn't, open `http://192.168.4.1`.
3. Tap your network, type the password, press **Connect**.
4. On success the page says "Connected" and the hotspot closes. The network is saved
   and used on every boot from now on.
5. If the password is wrong, the page says so and you can try again.

The hotspot only appears when the panel can't get online:

- at boot, when no known network connects;
- when the Wi‑Fi is lost for more than 60 s.

While the hotspot is open and nobody is connected to it, the panel retries the known
networks every 60 s and closes the hotspot when one works.

**Change the Wi‑Fi:** hold the **BOOT** button on the ESP32 for 5 s. The saved network is
forgotten and the setup hotspot opens.

## Before uploading

| Line | Setting | What to put |
|------|---------|-------------|
| 35 | `WIFI_SSID` | Built-in network name, or `""` for none |
| 36 | `WIFI_PASSWORD` | Its password |
| 39 | `PLAY_SERVER_URL` | Your Vercel address, e.g. `https://breethr-play.vercel.app` (no `/` at the end) |

Arduino IDE: board **ESP32 Dev Module**, library **FastLED**. Tested to compile with
esp32 core 3.3.12 and FastLED 3.10.3. No other libraries are needed.

## Other settings

| Setting | Current | Notes |
|---------|---------|-------|
| `PLAY_POLL_INTERVAL_MS` | 3000 | How often the panel asks the web app. Keep ≥ 3000 for Vercel's free plan. |
| `WIFI_CONNECT_TIMEOUT_MS` | 15000 | Time allowed per connection attempt. |
| `WIFI_LOST_PORTAL_MS` | 60000 | Wi‑Fi lost this long → open the setup hotspot. |
| `SETUP_BUTTON_PIN` | 0 | BOOT button. |
| Colours, timing, gamma, `LED_CORRECTION` | as v3 | Same lines/meaning as in `breethr_694_curved_wave_v3`. |

## Removed from the old firmware

The relay (pin 32) and the `/api/state` and `/api/readings` server calls are gone.
The built-in Wi‑Fi login is kept, as asked.

## Serial Monitor (115200)

Useful lines: `Hotspot name if setup is needed: "Breethr ABCDE"`, `Connected, IP …`,
`Web app reachable (seq N)`, `PLAY pressed (seq N)`, `Web app answered 500` (Redis not set up).
