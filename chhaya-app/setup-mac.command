#!/bin/bash
# Double-click this file on the Mac (or run it in Terminal) to prepare the Chhaya app and open it in Xcode.
cd "$(dirname "$0")" || exit 1
echo "== Chhaya app setup =="
if ! command -v node >/dev/null 2>&1; then
  echo "Node.js is not installed. Opening the download page: install the LTS version, then double-click this file again."
  open "https://nodejs.org/en/download"
  read -r -p "Press Enter to close..."
  exit 1
fi
if ! xcode-select -p 2>/dev/null | grep -q Xcode.app; then
  echo "Pointing the command-line tools at Xcode (asks for your Mac password)..."
  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
fi
npm install || { echo "npm install failed"; read -r -p "Press Enter..."; exit 1; }
npm run sync || { echo "sync failed"; read -r -p "Press Enter..."; exit 1; }
echo
echo "Done. Xcode is opening. Next: Signing & Capabilities -> Team -> your Personal Team, pick the iPad, press Run."
npx cap open ios
