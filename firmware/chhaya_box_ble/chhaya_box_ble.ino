// CHHAYA button box, v27: for the Chhaya iPad app (Bluetooth LE) with WiFi as a backup.
//
//   Button 1 (GPIO18) -> TALK   : wake Chhaya / answer a ringing call / "stop and listen to me"
//   Button 2 (GPIO4)  -> CALLS  : open the call log
//   Button 3 (GPIO19) -> HANGUP : cut a call, stop Chhaya talking, pause a song
//   Knob     (GPIO34) -> iPad system volume (as a Bluetooth volume remote)
//   LED strip (GPIO27)          : Chhaya's state (listening / thinking / speaking), patterns set by the page
//   Status LED (GPIO2)          : on = Chhaya app connected, slow blink = only paired, off = nothing
//
// HOW IT TALKS TO THE iPad
//   Bluetooth LE, one connection, two jobs:
//   * "Chhaya Box" service (Nordic UART UUIDs): buttons, knob, LED state, WiFi settings,
//     as short text lines. The Chhaya app connects to it by itself; nothing to pair for this.
//   * A tiny HID "volume remote" (volume up / down only, NOT a keyboard). For the knob to change
//     the iPad's volume, pair it once: iPad Settings -> Bluetooth -> "Chhaya Box".
//   WiFi (optional backup): when the app isn't connected over Bluetooth, button presses go to
//   Supabase (broadcast channel chhaya-box-<DEVICE_ROW_ID>) and the LEDs follow
//   devices.voice_led_state, exactly like the old WiFi box.
//
// WiFi NAME AND PASSWORD
//   Set them from the app (long-press the "button box" chip -> Box settings). They're saved on
//   the ESP32 and can be changed any time; no re-upload needed. The ESP32 only joins 2.4 GHz WiFi.
//
// LED PATTERNS
//   The page sends its pattern table (CONFIG.ledPatterns) whenever it connects; the box keeps a
//   copy so the WiFi backup uses the same patterns. Effects: off, solid, breathe, pulse, blink,
//   spin, rainbow (each with colour, speed and brightness).
//
// ---------------------------------------------------------------------
// BEFORE UPLOADING
//   Tools -> Board            -> "ESP32 Dev Module"
//   Tools -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)"
//   Libraries (Library Manager): "NimBLE-Arduino" by h2zero (version 2.x) and "Adafruit NeoPixel".
//   Remove/unpair the old "MITRA Back Button" from the iPad's Bluetooth list first.
//   Serial Monitor at 115200 shows what's happening.
// ---------------------------------------------------------------------

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"

#define FIRMWARE_VERSION "27"
#define BOX_NAME "Chhaya Box"

// Optional first-time WiFi. Leave "" and set it from the app instead (recommended).
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASSWORD ""

// Must be the same as CONFIG.hardware.deviceId in the Chhaya page.
#define DEVICE_ROW_ID "69bd0e47-d6fd-4c2b-85c0-979b71908d9d"
#define SUPABASE_URL "https://eklumiubhrjvjfgmvuhu.supabase.co"
// Public anon key (the same one that is in the page). Not a secret.
#define SUPABASE_ANON_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImVrbHVtaXViaHJqdmpmZ212dWh1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODgzNTU5MzQsImV4cCI6MjEwMzkzMTkzNH0.vLm5y6ihjRMSI_d7jQJQwg7XIwrl30hr8TxM2KxQFHg"

#define TALK_BUTTON_PIN   18
#define CALLS_BUTTON_PIN  4
#define HANGUP_BUTTON_PIN 19
#define STATUS_LED_PIN    2
#define POT_PIN           34
#define NEOPIXEL_PIN      27
#define NEOPIXEL_COUNT    72      // number of LEDs on the strip
#define DEBOUNCE_MS       250

#define USE_POT           true    // set false if no knob is connected
#define VOLUME_STEPS      16
#define ENDSTOP_RESYNC_PRESSES 20

#define LED_POLL_MS       700     // WiFi backup: how often to read devices.voice_led_state
#define WIFI_RETRY_MS     15000
#define MIN_FREE_HEAP     12000

// Chhaya Box service. Same UUIDs as the "Nordic UART" service, so the page's Web Bluetooth
// path (Chrome on a computer, Bluefy) can use this box too.
#define BOX_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BOX_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // page -> box (write)
#define BOX_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // box -> page (notify)

// HID report: consumer control, 1 byte: bit0 volume up, bit1 volume down, bit2 mute.
static const uint8_t HID_REPORT_MAP[] = {
  0x05, 0x0C,        // Usage Page (Consumer)
  0x09, 0x01,        // Usage (Consumer Control)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x03,        //   Report Count (3)
  0x09, 0xE9,        //   Usage (Volume Increment)
  0x09, 0xEA,        //   Usage (Volume Decrement)
  0x09, 0xE2,        //   Usage (Mute)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x95, 0x05,        //   Report Count (5) - padding
  0x81, 0x03,        //   Input (Constant)
  0xC0               // End Collection
};
#define HID_VOL_UP   0x01
#define HID_VOL_DOWN 0x02

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* boxTx = nullptr;
NimBLECharacteristic* hidInput = nullptr;
volatile bool bleLinked = false;       // any central connected (iPad)
volatile bool appSubscribed = false;   // the Chhaya app is listening on the box service
volatile bool hidSubscribed = false;   // iPad paired and using the volume remote

// Lines from the app, handed from the Bluetooth task to the main loop.
SemaphoreHandle_t rxLock;
String rxPending;
// Lines to the app, from any task, sent by the main loop.
SemaphoreHandle_t txLock;
String txPending;

// ---------------- LED patterns ----------------
enum Effect : uint8_t { FX_OFF, FX_SOLID, FX_BREATHE, FX_PULSE, FX_BLINK, FX_SPIN, FX_RAINBOW };
const char* const EFFECT_NAMES[] = { "off", "solid", "breathe", "pulse", "blink", "spin", "rainbow" };
struct Pattern { uint8_t fx; uint8_t r, g, b; uint16_t periodMs; uint8_t brightness; };

// Chhaya's states. The page can send any of these names.
const char* const STATE_NAMES[] = { "idle", "listening", "thinking", "speaking", "ringing", "reminder" };
const int STATE_COUNT = 6;
Pattern statePatterns[STATE_COUNT] = {
  { FX_OFF,     0,   0,   0,    0,   0 },   // idle
  { FX_BREATHE, 0, 255,  40, 1600, 200 },   // listening: green
  { FX_SPIN,    0,  60, 255, 1000, 200 },   // thinking: blue
  { FX_PULSE, 255, 170,   0,  700, 200 },   // speaking: yellow
  { FX_BLINK,   0, 200, 255,  800, 255 },   // ringing: cyan
  { FX_PULSE, 255,  90,   0, 1200, 220 },   // reminder: orange
};
volatile int currentState = 0;
Pattern directPattern;               // "LEDX:" test pattern from the settings screen
volatile bool useDirect = false;
unsigned long directUntil = 0;

// ---------------- WiFi ----------------
SemaphoreHandle_t wifiLock;          // guards wifiSsid / wifiPass (main loop and WiFi task both use them)
String wifiSsid, wifiPass;
volatile bool wifiReconfigure = false;
volatile bool wifiResend = true;     // send the WiFi status to the app again
volatile int wifiFailReason = 0;     // last disconnect reason from the WiFi driver
QueueHandle_t cloudQueue;            // button presses / knob for the WiFi backup
struct CloudEvent { char btn[8]; int vol; };
uint32_t cloudCounter = 0;

struct Button { int pin; const char* name; bool last; unsigned long pressedAt; };
Button buttons[] = {
  { TALK_BUTTON_PIN,   "TALK",   HIGH, 0 },
  { CALLS_BUTTON_PIN,  "CALLS",  HIGH, 0 },
  { HANGUP_BUTTON_PIN, "HANGUP", HIGH, 0 },
};

// =====================================================================
// Bluetooth
// =====================================================================
void queueToApp(const String& line) {
  xSemaphoreTake(txLock, portMAX_DELAY);
  if (txPending.length() < 1024) txPending += line + "\n";
  xSemaphoreGive(txLock);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleLinked = true;
    Serial.printf("[Bluetooth] connected: %s\n", info.getAddress().toString().c_str());
    // Snappy but battery-friendly connection interval (units of 1.25 ms): 15-30 ms.
    s->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleLinked = s->getConnectedCount() > 0;
    if (!bleLinked) { appSubscribed = false; hidSubscribed = false; }
    Serial.printf("[Bluetooth] disconnected (reason %d), advertising again\n", reason);
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("[Bluetooth] %s\n", info.isEncrypted() ? "paired / encrypted" : "pairing failed");
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    xSemaphoreTake(rxLock, portMAX_DELAY);
    if (rxPending.length() < 1024) rxPending.concat((const char*) v.data(), v.length());
    xSemaphoreGive(rxLock);
  }
};

class TxCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
    appSubscribed = subValue != 0;
    Serial.println(appSubscribed ? "[App] Chhaya app connected" : "[App] Chhaya app left");
    if (appSubscribed) {
      queueToApp("CHHAYA:READY " FIRMWARE_VERSION);
      wifiResend = true;
    }
  }
};

class HidCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
    hidSubscribed = subValue != 0;
    Serial.println(hidSubscribed ? "[Volume] iPad is using the volume remote" : "[Volume] volume remote not in use");
  }
};

void setupBluetooth() {
  NimBLEDevice::init(BOX_NAME);
  NimBLEDevice::setPower(9);  // dBm
  // "Just works" pairing with bonding, needed for the volume remote (HID).
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  bleServer->advertiseOnDisconnect(true);

  // Volume remote
  NimBLEHIDDevice* hid = new NimBLEHIDDevice(bleServer);
  hid->setManufacturer("Chhaya");
  hid->setPnp(0x02, 0x303A, 0x8001, 0x0100);   // USB vendor id source, Espressif VID
  hid->setHidInfo(0x00, 0x01);
  hid->setReportMap((uint8_t*) HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  hidInput = hid->getInputReport(1);
  hidInput->setCallbacks(new HidCallbacks());
  hid->setBatteryLevel(100);

  // Chhaya Box service
  NimBLEService* svc = bleServer->createService(BOX_SERVICE_UUID);
  boxTx = svc->createCharacteristic(BOX_TX_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  boxTx->setCallbacks(new TxCallbacks());
  NimBLECharacteristic* rx = svc->createCharacteristic(BOX_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());

  bleServer->start();

  // Advertising: flags + appearance + both services (29 of 31 bytes); the name goes in the scan response.
  NimBLEAdvertisementData adv;
  adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  adv.setAppearance(0x03C0);  // generic HID device
  adv.addServiceUUID(hid->getHidService()->getUUID());
  adv.addServiceUUID(NimBLEUUID(BOX_SERVICE_UUID));
  NimBLEAdvertisementData scan;
  scan.setName(BOX_NAME);
  NimBLEAdvertising* a = NimBLEDevice::getAdvertising();
  a->setAdvertisementData(adv);
  a->setScanResponseData(scan);
  a->enableScanResponse(true);
  a->start();
  Serial.println("[Bluetooth] advertising as '" BOX_NAME "'");
}

// Sends queued lines to the app in chunks that fit even the smallest Bluetooth packet.
void flushToApp() {
  if (!appSubscribed) {
    xSemaphoreTake(txLock, portMAX_DELAY); txPending = ""; xSemaphoreGive(txLock);
    return;
  }
  xSemaphoreTake(txLock, portMAX_DELAY);
  String out = txPending; txPending = "";
  xSemaphoreGive(txLock);
  for (unsigned int i = 0; i < out.length(); i += 20) {
    String part = out.substring(i, min((unsigned int) out.length(), i + 20));
    boxTx->notify((const uint8_t*) part.c_str(), part.length());
    delay(4);
  }
}

void tapVolume(uint8_t bit) {
  uint8_t v = bit;
  hidInput->setValue(&v, 1); hidInput->notify();
  delay(12);
  v = 0;
  hidInput->setValue(&v, 1); hidInput->notify();
}

// =====================================================================
// Messages from the page
//   HELLO                         -> CHHAYA:READY <version> + WiFi status
//   LED:<state>                   -> show that state's pattern (legacy GREEN/BLUE/YELLOW/OFF work too)
//   MAP:<state>=<effect>,<RRGGBB>,<periodMs>,<brightness>  -> set a state's pattern (saved)
//   LEDX:<effect>,<RRGGBB>,<periodMs>,<brightness>          -> show a pattern for 5 s (testing)
//   WIFI:<name><TAB><password>    -> save WiFi and connect
//   WIFI?                         -> WiFi status
// =====================================================================
int stateIndex(const String& s) {
  String n = s; n.trim(); n.toLowerCase();
  if (n == "green" || n == "wake") n = "listening";
  else if (n == "blue") n = "thinking";
  else if (n == "yellow" || n == "responding") n = "speaking";
  else if (n == "off") n = "idle";
  for (int i = 0; i < STATE_COUNT; i++) if (n == STATE_NAMES[i]) return i;
  return -1;
}

bool parsePattern(const String& spec, Pattern& p) {
  // effect,RRGGBB,periodMs,brightness  (only the effect is required)
  String parts[4]; int n = 0, start = 0;
  for (int i = 0; i <= (int) spec.length() && n < 4; i++) {
    if (i == (int) spec.length() || spec[i] == ',') { parts[n++] = spec.substring(start, i); start = i + 1; }
  }
  parts[0].trim(); parts[0].toLowerCase();
  int fx = -1;
  for (int i = 0; i < 7; i++) if (parts[0] == EFFECT_NAMES[i]) fx = i;
  if (fx < 0) return false;
  p.fx = fx;
  uint32_t rgb = n > 1 && parts[1].length() ? strtoul(parts[1].c_str(), nullptr, 16) : 0xFFFFFF;
  p.r = (rgb >> 16) & 0xFF; p.g = (rgb >> 8) & 0xFF; p.b = rgb & 0xFF;
  p.periodMs = n > 2 && parts[2].toInt() > 0 ? constrain(parts[2].toInt(), 100, 20000) : 1000;
  p.brightness = n > 3 && parts[3].length() ? constrain(parts[3].toInt(), 0, 255) : 200;
  return true;
}

String patternToSpec(const Pattern& p) {
  char buf[40];
  snprintf(buf, sizeof buf, "%s,%02X%02X%02X,%u,%u", EFFECT_NAMES[p.fx], p.r, p.g, p.b, p.periodMs, p.brightness);
  return String(buf);
}

void loadSettings() {
  prefs.begin("chhaya", false);
  wifiSsid = prefs.getString("ssid", DEFAULT_WIFI_SSID);
  wifiPass = prefs.getString("pass", DEFAULT_WIFI_PASSWORD);
  for (int i = 0; i < STATE_COUNT; i++) {
    String spec = prefs.getString((String("m_") + STATE_NAMES[i]).c_str(), "");
    Pattern p;
    if (spec.length() && parsePattern(spec, p)) statePatterns[i] = p;
  }
}

void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  if (line == "HELLO") {
    queueToApp("CHHAYA:READY " FIRMWARE_VERSION);
    wifiResend = true;
  } else if (line.startsWith("LED:")) {
    int i = stateIndex(line.substring(4));
    if (i >= 0) { currentState = i; useDirect = false; }
  } else if (line.startsWith("MAP:")) {
    int eq = line.indexOf('=');
    int i = eq > 4 ? stateIndex(line.substring(4, eq)) : -1;
    Pattern p;
    if (i >= 0 && parsePattern(line.substring(eq + 1), p)) {
      statePatterns[i] = p;
      String key = String("m_") + STATE_NAMES[i], spec = patternToSpec(p);
      if (prefs.getString(key.c_str(), "") != spec) prefs.putString(key.c_str(), spec);  // save flash wear
    }
  } else if (line.startsWith("LEDX:")) {
    Pattern p;
    if (parsePattern(line.substring(5), p)) { directPattern = p; useDirect = true; directUntil = millis() + 5000; }
  } else if (line.startsWith("WIFI:")) {
    String rest = line.substring(5);
    int tab = rest.indexOf('\t');
    String ssid = tab >= 0 ? rest.substring(0, tab) : rest;
    String pass = tab >= 0 ? rest.substring(tab + 1) : "";
    ssid.trim();
    if (ssid.length() > 32 || pass.length() > 63) { queueToApp("WIFI:FAIL too-long " + ssid); return; }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    xSemaphoreTake(wifiLock, portMAX_DELAY);
    wifiSsid = ssid; wifiPass = pass;
    xSemaphoreGive(wifiLock);
    Serial.println("[WiFi] new network saved from the app: '" + ssid + "'");   // password never printed
    queueToApp("WIFI:CONNECTING " + ssid);
    wifiReconfigure = true;
  } else if (line == "WIFI?") {
    wifiResend = true;
  }
}

void processFromApp() {
  xSemaphoreTake(rxLock, portMAX_DELAY);
  int nl;
  String lines;
  while ((nl = rxPending.indexOf('\n')) >= 0) {
    lines += rxPending.substring(0, nl + 1);
    rxPending.remove(0, nl + 1);
  }
  xSemaphoreGive(rxLock);
  int start = 0;
  while ((nl = lines.indexOf('\n', start)) >= 0) { handleLine(lines.substring(start, nl)); start = nl + 1; }
}

// =====================================================================
// LED strip
// =====================================================================
uint32_t scaled(const Pattern& p, float k) {
  k = constrain(k, 0.0f, 1.0f);
  return strip.Color((uint8_t)(p.r * k), (uint8_t)(p.g * k), (uint8_t)(p.b * k));
}

void renderLeds() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last < 20) return;   // 50 frames a second
  last = now;
  if (useDirect && (long)(now - directUntil) > 0) useDirect = false;
  const Pattern& p = useDirect ? directPattern : statePatterns[currentState];
  strip.setBrightness(p.brightness);
  float phase = p.periodMs ? (float)(now % p.periodMs) / p.periodMs : 0;
  switch (p.fx) {
    case FX_OFF: strip.clear(); break;
    case FX_SOLID: strip.fill(scaled(p, 1)); break;
    case FX_BREATHE: strip.fill(scaled(p, 0.12f + 0.88f * (0.5f - 0.5f * cosf(phase * 2 * PI)))); break;
    case FX_PULSE: strip.fill(scaled(p, phase < 0.3f ? 1.0f : 1.0f - (phase - 0.3f) / 0.7f * 0.8f)); break;
    case FX_BLINK: strip.fill(phase < 0.5f ? scaled(p, 1) : 0); break;
    case FX_SPIN: {
      float head = phase * NEOPIXEL_COUNT;
      for (int i = 0; i < NEOPIXEL_COUNT; i++) {
        float d = head - i; if (d < 0) d += NEOPIXEL_COUNT;
        strip.setPixelColor(i, scaled(p, d < NEOPIXEL_COUNT / 4.0f ? 1.0f - d / (NEOPIXEL_COUNT / 4.0f) : 0.05f));
      }
      break;
    }
    case FX_RAINBOW:
      for (int i = 0; i < NEOPIXEL_COUNT; i++)
        strip.setPixelColor(i, strip.ColorHSV((uint16_t)((phase + (float) i / NEOPIXEL_COUNT) * 65535)));
      break;
  }
  strip.show();
}

void renderStatusLed() {
  bool on = appSubscribed ? true : (bleLinked ? (millis() / 1000) % 2 : false);
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

// =====================================================================
// Buttons and knob
// =====================================================================
void sendButton(const char* name) {
  if (appSubscribed) {
    queueToApp(String("BTN:") + name);
    Serial.printf("[Button] %s -> app (Bluetooth)\n", name);
  } else if (WiFi.status() == WL_CONNECTED) {
    CloudEvent e = {}; strncpy(e.btn, name, sizeof e.btn - 1); e.vol = -1;
    xQueueSend(cloudQueue, &e, 0);
    Serial.printf("[Button] %s -> Supabase (WiFi backup)\n", name);
  } else {
    Serial.printf("[Button] %s pressed but the app isn't connected and there's no WiFi\n", name);
  }
}

void handleButtons() {
  unsigned long now = millis();
  for (auto& b : buttons) {
    bool s = digitalRead(b.pin);
    if (s == LOW && b.last == HIGH && now - b.pressedAt > DEBOUNCE_MS) { b.pressedAt = now; sendButton(b.name); }
    b.last = s;
  }
}

float smoothedPot = -1;
int lastVolumeStep = -1;
void handleKnob() {
  if (!USE_POT) return;
  static unsigned long lastSample = 0;
  unsigned long now = millis();
  if (now - lastSample < 25) return;
  lastSample = now;

  // Median of 9 readings + slow average: ignores the jitter of a noisy knob.
  int r[9];
  for (int i = 0; i < 9; i++) r[i] = analogRead(POT_PIN);
  for (int i = 1; i < 9; i++) { int v = r[i], j = i - 1; while (j >= 0 && r[j] > v) { r[j + 1] = r[j]; j--; } r[j + 1] = v; }
  smoothedPot = smoothedPot < 0 ? r[4] : smoothedPot + 0.08f * (r[4] - smoothedPot);

  // Notches with hysteresis, so it never flickers between two steps.
  float pos = (smoothedPot / 4095.0f) * (VOLUME_STEPS - 1);
  if (lastVolumeStep < 0) { lastVolumeStep = constrain((int) round(pos), 0, VOLUME_STEPS - 1); return; }
  int step = lastVolumeStep;
  if (pos > lastVolumeStep + 0.8f) step = constrain((int) floor(pos + 0.2f), 0, VOLUME_STEPS - 1);
  else if (pos < lastVolumeStep - 0.8f) step = constrain((int) ceil(pos - 0.2f), 0, VOLUME_STEPS - 1);
  if (step == lastVolumeStep) return;
  int delta = step - lastVolumeStep;
  lastVolumeStep = step;
  int pct = step * 100 / (VOLUME_STEPS - 1);

  if (hidSubscribed) {
    // iPad system volume, like the old box. At either end, press extra times so the iPad matches the knob.
    bool atEnd = step == 0 || step == VOLUME_STEPS - 1;
    int presses = atEnd ? ENDSTOP_RESYNC_PRESSES : abs(delta);
    uint8_t bit = (atEnd ? step > 0 : delta > 0) ? HID_VOL_UP : HID_VOL_DOWN;
    Serial.printf("[Volume] knob %d%% -> iPad volume %s x%d\n", pct, bit == HID_VOL_UP ? "up" : "down", presses);
    for (int i = 0; i < presses; i++) { tapVolume(bit); delay(atEnd ? 30 : 50); }
  } else if (appSubscribed) {
    // Not paired for the volume remote: Chhaya's own volume instead.
    queueToApp("VOL:" + String(pct));
  } else if (WiFi.status() == WL_CONNECTED) {
    CloudEvent e = {}; e.vol = pct;
    xQueueSend(cloudQueue, &e, 0);
  }
}

// =====================================================================
// WiFi backup (own task, so a slow network never delays a button)
// =====================================================================
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) wifiFailReason = info.wifi_sta_disconnected.reason;
  else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) wifiFailReason = 0;
}

String wifiStatusLine() {
  xSemaphoreTake(wifiLock, portMAX_DELAY);
  String ssid = wifiSsid;
  xSemaphoreGive(wifiLock);
  if (!ssid.length()) return "WIFI:OFF";
  if (WiFi.status() == WL_CONNECTED) return "WIFI:OK " + WiFi.localIP().toString() + " " + ssid;
  int r = wifiFailReason;
  if (r == 201) return "WIFI:FAIL not-found " + ssid;                         // no network with that name
  if (r == 15 || r == 202 || r == 204 || r == 2) return "WIFI:FAIL wrong-password " + ssid;
  if (r) return "WIFI:FAIL error-" + String(r) + " " + ssid;
  return "WIFI:CONNECTING " + ssid;
}

bool wifiBegin() {
  xSemaphoreTake(wifiLock, portMAX_DELAY);
  String ssid = wifiSsid, pass = wifiPass;
  xSemaphoreGive(wifiLock);
  WiFi.disconnect(true);
  delay(100);
  wifiFailReason = 0;
  if (!ssid.length()) { WiFi.mode(WIFI_OFF); Serial.println("[WiFi] no network set (set it from the app)"); return false; }
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // smaller current spikes; plenty for a home router
  // Modem sleep stays ON: Bluetooth and WiFi share one radio.
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.println("[WiFi] connecting to '" + ssid + "'...");
  return true;
}

String extractJsonString(const String& json, const char* key) {
  int k = json.indexOf(String("\"") + key + "\"");
  if (k < 0) return "";
  int q1 = json.indexOf('"', json.indexOf(':', k) + 1);
  int q2 = json.indexOf('"', q1 + 1);
  return q1 < 0 || q2 < 0 ? "" : json.substring(q1 + 1, q2);
}

void cloudBroadcast(WiFiClientSecure& tls, const String& payload) {
  HTTPClient http;
  http.begin(tls, String(SUPABASE_URL) + "/realtime/v1/api/broadcast");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(4000);
  String body = String("{\"messages\":[{\"topic\":\"chhaya-box-") + DEVICE_ROW_ID + "\",\"event\":\"box\",\"payload\":" + payload + "}]}";
  int code = http.POST(body);
  if (code < 200 || code >= 300) { Serial.printf("[WiFi] broadcast failed (%d)\n", code); tls.stop(); }
  http.end();
}

void wifiTask(void*) {
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient poll;
  poll.setReuse(true);
  poll.setTimeout(2500);
  unsigned long lastPoll = 0, lastTry = millis();
  bool wasConnected = false;
  bool haveNetwork = wifiBegin();
  String reported;

  for (;;) {
    if (wifiReconfigure) { wifiReconfigure = false; tls.stop(); haveNetwork = wifiBegin(); lastTry = millis(); wasConnected = false; }

    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wasConnected) {
      Serial.println("[WiFi] connected, IP " + WiFi.localIP().toString());
      cloudBroadcast(tls, String("{\"hello\":\"box\",\"fw\":\"" FIRMWARE_VERSION "\",\"n\":") + (++cloudCounter) + "}");
    }
    wasConnected = connected;
    if (!connected && haveNetwork && millis() - lastTry > WIFI_RETRY_MS) { lastTry = millis(); WiFi.reconnect(); }

    // Report status changes to the app (the password is never sent back).
    String st = wifiStatusLine();
    if (wifiResend) { wifiResend = false; reported = ""; }
    if (appSubscribed && st != reported) { reported = st; queueToApp(st); }

    // Button presses / knob while the app isn't connected over Bluetooth.
    CloudEvent e;
    while (connected && xQueueReceive(cloudQueue, &e, 0) == pdTRUE) {
      String payload = e.btn[0] ? String("{\"btn\":\"") + e.btn + "\"" : String("{\"vol\":") + e.vol;
      cloudBroadcast(tls, payload + ",\"n\":" + (++cloudCounter) + "}");
    }

    // LED state from Supabase, only when the app isn't driving the LEDs over Bluetooth.
    if (connected && !appSubscribed && millis() - lastPoll > LED_POLL_MS) {
      lastPoll = millis();
      poll.begin(tls, String(SUPABASE_URL) + "/rest/v1/devices?id=eq." DEVICE_ROW_ID "&select=voice_led_state");
      poll.addHeader("apikey", SUPABASE_ANON_KEY);
      poll.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
      int code = poll.GET();
      if (code == 200) {
        int i = stateIndex(extractJsonString(poll.getString(), "voice_led_state"));
        if (i >= 0 && !useDirect) currentState = i;
      } else if (code < 0) {
        tls.stop();
      }
      poll.end();
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

// =====================================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // weak USB power reset the board at boot
  Serial.begin(115200);
  for (auto& b : buttons) pinMode(b.pin, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  analogReadResolution(12);
  rxLock = xSemaphoreCreateMutex();
  txLock = xSemaphoreCreateMutex();
  wifiLock = xSemaphoreCreateMutex();
  cloudQueue = xQueueCreate(8, sizeof(CloudEvent));

  strip.begin();
  strip.setBrightness(80);
  if (esp_reset_reason() != ESP_RST_SW) {   // boot check: green, blue, yellow
    strip.fill(strip.Color(0, 255, 40)); strip.show(); delay(250);
    strip.fill(strip.Color(0, 60, 255)); strip.show(); delay(250);
    strip.fill(strip.Color(255, 170, 0)); strip.show(); delay(250);
  }
  strip.clear(); strip.show();

  Serial.println("\nCHHAYA button box v" FIRMWARE_VERSION " (Bluetooth LE + WiFi backup)");
  loadSettings();
  setupBluetooth();
  WiFi.onEvent(onWiFiEvent);
  delay(300);
  xTaskCreatePinnedToCore(wifiTask, "wifi", 10240, nullptr, 1, nullptr, 0);
}

void loop() {
  processFromApp();
  handleButtons();
  handleKnob();
  renderLeds();
  renderStatusLed();
  flushToApp();

  static unsigned long lowSince = 0, lastBeat = 0;
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    if (!lowSince) lowSince = millis();
    else if (millis() - lowSince > 30000) { Serial.println("[Heal] memory low for 30 s, restarting"); delay(100); ESP.restart(); }
  } else lowSince = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    Serial.printf("[Status] app: %s | volume remote: %s | %s | LED: %s | free memory %u\n",
      appSubscribed ? "connected" : "not connected", hidSubscribed ? "paired" : "not paired",
      wifiStatusLine().c_str(), STATE_NAMES[currentState], ESP.getFreeHeap());
  }
  delay(5);
}
