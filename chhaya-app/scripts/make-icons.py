"""Builds the app icon, splash screen and loader face from the Chhaya face
that is embedded in tablet/index.html (AVATAR_IMG). Needs Pillow.
Run from chhaya-app/:  python3 scripts/make-icons.py"""
import base64, io, json, re
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
html = (ROOT.parent / "tablet" / "index.html").read_text(encoding="utf-8")
b64 = re.search(r'const AVATAR_IMG = "data:image/\w+;base64,([^"]+)"', html).group(1)
face = Image.open(io.BytesIO(base64.b64decode(b64))).convert("RGBA")
face = face.crop(face.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox())  # trim empty border
BG = (255, 246, 236, 255)  # same cream as the page

def centred(size, scale):
    canvas = Image.new("RGBA", (size, size), BG)
    side = int(size * scale); k = side / max(face.size)
    f = face.resize((round(face.width * k), round(face.height * k)), Image.LANCZOS)
    canvas.alpha_composite(f, ((size - f.width) // 2, (size - f.height) // 2))
    return canvas

assets = ROOT / "ios" / "App" / "App" / "Assets.xcassets"
# App icon: one 1024 px image (Xcode 14+ makes every size from it). No transparency allowed.
icon_dir = assets / "AppIcon.appiconset"
centred(1024, 0.86).convert("RGB").save(icon_dir / "AppIcon-512@2x.png")
# Splash: Capacitor's LaunchScreen shows the "Splash" image set, aspect-fill.
splash_dir = assets / "Splash.imageset"
for name in ("splash-2732x2732.png", "splash-2732x2732-1.png", "splash-2732x2732-2.png"):
    centred(2732, 0.28).convert("RGB").save(splash_dir / name)
# Loader page face (round)
f = face.copy(); f.thumbnail((360, 360), Image.LANCZOS)
mask = Image.new("L", f.size, 0); ImageDraw.Draw(mask).ellipse((0, 0, f.width, f.height), fill=255)
out = Image.new("RGBA", f.size, (0, 0, 0, 0)); out.paste(f, (0, 0), mask)
out.save(ROOT / "www" / "face.png")
print("icons, splash and face.png written")
