# Chhaya iPad app: build and setup guide

What's in this repo:

| Folder | What it is |
|---|---|
| `tablet/index.html` | The Chhaya page, **v27**. Upload it to the Vercel project **ipadtest2**. It still works in Chrome as before. |
| `chhaya-app/` | The iPad app (Capacitor 8). It opens the Chhaya page built into the app (`npm run sync` copies `tablet/index.html` in). |
| `firmware/chhaya_box_ble/` | The new ESP32 button box sketch (Bluetooth LE, with WiFi as a backup). |

Plan on about 1–2 hours the first time. After that, most changes are just a Vercel upload.

---

## 1. Put the new page on Vercel (5 min)

1. Upload `tablet/index.html` to the Vercel project **ipadtest2** and redeploy.
2. Check the address. The app expects **https://ipadtest2.vercel.app/**. If your address is different, change it in two places:
   - `chhaya-app/www/index.html`: `REMOTE_URL`
   - `chhaya-app/capacitor.config.json`: `allowNavigation`
   - also `CONFIG.app.remoteUrl` in `tablet/index.html`

   Then run `npm run sync` again (see step 2).

From now on, any change to the page only needs a Vercel upload. The app picks it up the next time it opens, with no rebuild.

## 2. Build the app on the Mac (30–45 min the first time)

**One-time setup**
1. Open Xcode once and let it install its extra components. When asked which platforms to add, tick **iOS**.
2. Install Node.js 22 (LTS) from nodejs.org.
3. Get this repo onto the Mac: `git clone` it, or GitHub → Code → Download ZIP.

**Build**
```bash
cd chhaya-app
npm install
npm run sync        # copies tablet/index.html into the app as the offline copy
npm run open        # opens the project in Xcode
```

**In Xcode**
1. In the left sidebar click **App** (blue icon), then **TARGETS → App → Signing & Capabilities**.
2. Tick **Automatically manage signing**. Under **Team**, choose **Add an Account…** and sign in with your Apple ID. Then pick **"<your name> (Personal Team)"**.
3. If Xcode says the bundle identifier `com.nusha.chhaya` "is not available", change it to something unique, e.g. `com.nusha.chhaya2`.
4. Connect the iPad with a cable, unlock it, and tap **Trust**. Choose the iPad at the top of the Xcode window.
5. **On the iPad:** Settings → Privacy & Security → **Developer Mode** → On. The iPad restarts.
6. Press **▶ Run** in Xcode.
7. The first time, the iPad says "Untrusted Developer". Go to Settings → General → **VPN & Device Management** → your Apple ID → **Trust**. Then press ▶ Run again.

**Free Apple ID vs the $99 account**
- With a free Apple ID the app **stops opening after 7 days**. To fix that, plug the iPad into the Mac and press ▶ Run again. It takes 1 minute, and settings and pairing are kept.
- The $99/year Apple Developer Program makes it last a year and allows TestFlight. You can upgrade any time. Nothing else changes.

**If the build fails:** send me a screenshot of the red error in Xcode (the ⚠️/❌ panel on the left). I can't run Xcode where I work, so a first-build error is possible and quick to fix.

## 3. Flash the new button box sketch (15 min)

1. On the iPad: Settings → Bluetooth → **MITRA Back Button** → ⓘ → **Forget This Device**. That was the old box.
2. In Arduino IDE:
   - **Tools → Board → ESP32 Dev Module**
   - **Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**
   - Library Manager: install **NimBLE-Arduino** by h2zero (version **2.x**) and **Adafruit NeoPixel**. The old HijelHID library isn't needed any more.
   - ESP32 board package: 3.x is recommended. The sketch was compiled against 2.0.9.
3. Open `firmware/chhaya_box_ble/chhaya_box_ble.ino` and click **Upload**. It has finished when you see "Hard resetting via RTS pin".
4. Serial Monitor (115200): the box prints `advertising as 'Chhaya Box'` and a `[Status]` line every 10 seconds.

The pins are the same as before (TALK 18, CALLS 4, HANGUP 19, knob 34, LED strip 27, status LED 2). The WiFi name and password are **not** in the sketch any more. You set them from the app (step 4).

## 4. First start on the iPad (10 min)

1. Open **Chhaya**. Allow **Microphone**, **Camera**, **Bluetooth** and **Notifications**. The prompts are in Hindi and English.
2. Chhaya starts by herself, with no "शुरू करें" tap. The button box connects on its own within a few seconds, and the chip at the bottom says **"बटन बॉक्स जुड़ा है"**.
3. **Volume knob (once):** Settings → Bluetooth → tap **Chhaya Box** under Other Devices → Pair. After that the knob changes the iPad's volume. Before pairing, it only changes Chhaya's own volume.
4. **WiFi for the box:** long-press the "बटन बॉक्स" chip (hold about 1 second) to open **Box settings**. Type the WiFi name and password and tap **WiFi सेव करें**. The status changes to ✓ जुड़ा है, or tells you if the password is wrong or the network isn't found. You can change them here any time.
5. **LED टेस्ट** shows each LED pattern in turn.
6. Close the Box settings, check the tests below, and you're done.

**Test checklist**

| Do | Expect |
|---|---|
| Say "छाया" | Green breathing LEDs, then blue spinning while she thinks, then yellow pulsing while she speaks |
| Button 1 | She says "जी…" and listens. Pressing it while she's talking stops her and she listens |
| Button 2 | The call log opens |
| Button 3 while she talks, or during a call | She stops / the call ends |
| Knob | The iPad volume bar moves |
| Switch the box off and on | The chip says it lost the box, then it reconnects by itself |
| Guardian calls | The iPad rings, and the video connects both ways |
| Add a reminder for 2 minutes from now, then close the app | The iPad shows a notification at that time |

Add `?debug=1` to the page address, or look at the bottom of the screen, to check the version. The status line starts with **v27 app** when it's running inside the app.

## 5. Everyday settings

- **LED patterns:** edit `CONFIG.ledPatterns` in `tablet/index.html` and upload to Vercel. The format is `"effect,RRGGBB colour,cycle ms,brightness 0-255"`. The effects are `off`, `solid`, `breathe`, `pulse`, `blink`, `spin` and `rainbow`. The box gets the new patterns the next time the app connects, and keeps them for its WiFi backup too.
- **WiFi password changed?** Box settings → type the new one → Save. Buttons and LEDs keep working over Bluetooth in the meantime.
- **Replaced the ESP32?** Box settings → **दूसरा बॉक्स**. The app forgets the old box and finds the new one.
- **Recipient's name.** In Supabase → SQL editor run:
  ```sql
  update care_recipients set name = 'Sarla', greeting_name = 'सरला जी'
  where id = '3e2ce008-b83e-4fbf-9f67-d7bed9f0973f';
  ```

## 6. Kiosk mode (recommended)

- Settings → Accessibility → **Guided Access** → On, and set a passcode.
- Open Chhaya and triple-click the top (or home) button → **Start**. The iPad now stays in Chhaya. Triple-click again and enter the passcode to leave.
- The app already keeps the screen on while it's open, and hides the status bar and home bar. Keep the iPad on its charger.

## Good to know

- Apple doesn't let any app listen for "छाया" while the iPad is **locked** or another app is in front. Guided Access keeps Chhaya in front.
- If the app is fully closed, a guardian's video call **can't ring the iPad**. That would need CallKit/VoIP push, which is a separate project. Reminders still arrive as notifications.
- How the app finds the box: it connects to the box the iPad is already connected to, or the one it used last time, or scans for any "Chhaya Box". Nothing needs pairing except the volume knob.
- WiFi backup: when the app isn't connected over Bluetooth, the box sends button presses through Supabase (`chhaya-box-<deviceId>` broadcast) and reads the LED state from `devices.voice_led_state`, like the old WiFi box.
