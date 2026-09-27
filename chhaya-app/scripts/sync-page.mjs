// Copies the Chhaya tablet page into the app as its offline fallback.
// Run after every change to ../tablet/index.html:  npm run sync
import { copyFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const src = resolve(here, "../../tablet/index.html");
const dest = resolve(here, "../www/offline/index.html");
mkdirSync(dirname(dest), { recursive: true });
copyFileSync(src, dest);
console.log(`offline copy updated: ${dest}`);
