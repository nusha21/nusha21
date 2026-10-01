# Breethr Vercel cloud — exact setup order

## A. Deploy the cloud controller first

1. Put the contents of `web/` in the root of a Git repository. The project root
   must contain `index.html`, `package.json`, `vercel.json`, `api/`, `lib/`,
   `assets/`, and `fonts/`.
2. In Vercel, choose **Add New -> Project** and import that repository as a
   new controller project. Do not replace the existing `project-57vve` data
   project unless its current `/api/state` and `/api/readings` routes are also
   being merged into the same repository.
3. Choose **Other** as the Framework Preset.
4. Leave Build Command and Output Directory at their default/empty values.
5. Deploy once.
6. Open the new Vercel project, then open **Storage**.
7. Choose **Create Database -> Blob** and create a **Private** Blob store.
8. Connect the store to this project and include the Production environment.
   Vercel creates `BLOB_READ_WRITE_TOKEN` automatically.
9. Redeploy the project so the Production deployment receives the token.
10. Open `https://YOUR-PROJECT.vercel.app/api/command?device=breethr-esp32`.
    A working setup returns JSON rather than a storage error.

## B. Put the final Vercel URL into the firmware

1. Open:
   `firmware/BreethrVercelCloud/BreethrVercelCloud.ino`
2. Near the top, find:
   `const char CONTROL_BASE_URL[] = "https://YOUR-PROJECT.vercel.app";`
3. Replace only the URL with the exact Production URL from Vercel. Do not add a
   trailing slash.
4. Confirm `DEVICE_ID` remains `breethr-esp32`; it must match `DEVICE_ID` in
   `web/app.js`.

## C. Compile and upload the ESP32 firmware

Install/select:

- ESP32 Arduino core 3.3.12 or compatible
- FastLED 3.10.5 or compatible
- ArduinoJson 6.x
- The correct ESP32 board and serial port

Compile and upload. If the selected board reports that the sketch is too
large, choose a partition scheme with a larger app partition, such as
`Huge APP (3MB No OTA/1MB SPIFFS)` or its equivalent.

## D. Give the ESP32 internet Wi-Fi

The firmware first tries credentials already saved on the ESP32, then the
original baked-in fallback. If neither connects within about 20 seconds:

1. Join the temporary open network `Breethr ABCDE`.
2. Stay connected when the phone warns that it has no internet.
3. Open `http://192.168.4.1` if the setup page does not appear automatically.
4. Enter the destination 2.4 GHz Wi-Fi name and password.
5. After connection succeeds, the temporary network and its setup web server
   close automatically.

Only this one-time provisioning step is local. Normal playback does not use a
local hostname, local IP address, mDNS, or same-network browser access.

## E. Test from Vercel

1. Keep the ESP32 powered and connected to internet Wi-Fi.
2. Open the Production Vercel URL from any internet connection, including
   mobile data or a completely different Wi-Fi network.
3. Press **Play animation**.
4. The page first shows `Command sent. Waiting for the panel...`.
5. The ESP32 receives the command within about five seconds, restarts the
   animation, and sends an acknowledgement.
6. The page changes to `Animation playing`.

If the ESP32 is offline, the page says the command is queued. The most recent
command remains in Blob storage and is handled when the panel reconnects.

## Fast diagnosis

- `/api/play` storage error: Blob is not connected, is not Private, or the
  Production deployment does not have `BLOB_READ_WRITE_TOKEN`; reconnect and
  redeploy.
- `/api/command` works but the panel does nothing: verify `CONTROL_BASE_URL` in
  the uploaded sketch exactly matches the deployed Production URL.
- Page stays waiting: use Serial Monitor at 115200 baud and look for
  `[CLOUD CONTROL] Playing command ...`.
- Button works only after a second press following an old firmware upload:
  upload this cloud sketch again and ensure the Vercel and firmware device IDs
  both remain `breethr-esp32`.
