// Copies the Chhaya tablet page into the app; the app opens it directly.
// Run after every change to ../tablet/index.html:  npm run sync
// (www/loader.html is the optional "load from Vercel with offline fallback" start page,
//  not used while CONFIG.app.remoteUrl is empty.)
import { copyFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const src = resolve(here, "../../tablet/index.html");
const dest = resolve(here, "../www/index.html");
copyFileSync(src, dest);
console.log(`app page updated: ${dest}`);
