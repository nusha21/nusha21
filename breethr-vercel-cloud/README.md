# Breethr Vercel cloud controller

This version does **not** ask the deployed browser to contact the ESP32 on the
local network. The complete control path is:

`Vercel page -> Vercel Function -> private Vercel Blob -> ESP32 HTTPS poll`

The temporary `Breethr ABCDE` access point is retained only for entering Wi-Fi
credentials. Its setup web server closes after the ESP32 joins Wi-Fi.

## Contents

- `web/` - static interface, Vercel Functions, and Blob-backed command store.
- `firmware/BreethrVercelCloud/BreethrVercelCloud.ino` - updated ESP32 sketch.
- `HANDOFF_TO_DEVELOPER.md` - exact deployment and upload order.

## Runtime behavior

1. The animation plays once at boot.
2. The ESP32 connects to Wi-Fi, using saved credentials or its temporary setup
   access point when necessary.
3. The ESP32 polls the deployed Vercel command endpoint every five seconds.
4. Pressing the Vercel button writes a unique play command to a private Blob.
5. The ESP32 receives it, restarts the animation, and acknowledges it through
   Vercel. The web page reports `Animation playing` after acknowledgement.
6. The controller can be used from any internet connection; it does not need
   to be on the ESP32's Wi-Fi.

## Required Vercel service

Create a **Private Vercel Blob** store and connect it to the deployed project.
Vercel will add `BLOB_READ_WRITE_TOKEN` to the project automatically. Redeploy
after connecting the store so Production receives the variable.

The command data is private and is delivered only through the project's API
functions. No Blob token is included in the browser or firmware.

## Important upload order

Deploy Vercel first. Copy its production URL into `CONTROL_BASE_URL` near the
top of the firmware, with no trailing slash, and only then compile/upload the
ESP32 sketch.

Use a new Vercel controller project unless a developer is deliberately merging
these files into the existing `project-57vve` data project; replacing that
project outright could remove its current `/api/state` and `/api/readings`
implementation.

## Existing behavior preserved

The LED renderer, one-shot boot animation, relay state, persisted air state,
existing data-server URL, `/api/state` polling, and `/api/readings` posting are
preserved. Cloud animation control uses separate endpoints and does not require
the browser and ESP32 to share a LAN.

## Verification

The sketch was compile-checked with ESP32 Arduino core 3.3.12, FastLED 3.10.5,
and ArduinoJson 6.21.5 using the generic `esp32` board target. It uses 1,214,699
bytes (92%) of the default 1,310,720-byte application partition and 56,840
bytes (17%) of RAM. The Vercel JavaScript and serverless modules also passed
syntax/import validation with `@vercel/blob` 2.8.0.

## Prototype notes

- The controller link is intentionally a one-button prototype without user
  authentication. Anyone who has the deployed link can press the button.
- The firmware retains the original insecure TLS setting and baked-in Wi-Fi
  fallback. Replace both before a public production release.
- The supplied font archives are personal-use/trial fonts. Replace them with
  properly licensed webfonts before commercial public deployment.
