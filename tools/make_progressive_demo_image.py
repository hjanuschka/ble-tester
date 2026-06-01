from PIL import Image, ImageDraw, ImageFont, ImageFilter
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "examples" / "ESP32C3_All_BLE_Tester"
REF = Path(__file__).resolve().parent / "dino_reference.png"
JPG = OUT / "dino.jpg"
HDR = OUT / "dino_jpg.h"

W, H = 640, 420
img = Image.new("RGB", (W, H), "black")
p = img.load()

# Strong full-frame gradient. Progressive JPEG first scans make this visible early.
for y in range(H):
    for x in range(W):
        nx = x / (W - 1)
        ny = y / (H - 1)
        wave = 0.5 + 0.5 * math.sin(nx * 10.0 + ny * 5.0)
        red = int(35 + 210 * max(0, 1 - 1.35 * math.hypot(nx - 0.18, ny - 0.28)))
        green = int(30 + 180 * max(0, 1 - 1.15 * math.hypot(nx - 0.78, ny - 0.72)))
        blue = int(55 + 170 * (0.30 + 0.70 * nx) * (0.45 + 0.55 * (1 - ny)))
        p[x, y] = (min(255, red + int(22 * wave)), min(255, green + int(18 * (1 - wave))), min(255, blue))

d = ImageDraw.Draw(img)

# Fine detail grid and diagonals to show progressive sharpening.
for x in range(0, W, 32):
    d.line([(x, 0), (x, H)], fill=(255, 255, 255), width=1)
for y in range(0, H, 32):
    d.line([(0, y), (W, y)], fill=(0, 0, 0), width=1)
for off in range(-H, W, 42):
    d.line([(off, H), (off + H, 0)], fill=(255, 193, 7), width=1)

# Panels.
overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
od = ImageDraw.Draw(overlay)
od.rounded_rectangle((24, 20, W - 24, 116), radius=18, fill=(0, 0, 0, 165), outline=(255, 193, 7, 255), width=3)
od.rounded_rectangle((24, H - 92, W - 24, H - 22), radius=16, fill=(0, 0, 0, 150), outline=(255, 255, 255, 180), width=2)
od.rounded_rectangle((32, 132, 210, 344), radius=14, fill=(0, 0, 0, 95), outline=(255, 255, 255, 150), width=2)
img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")
d = ImageDraw.Draw(img)

# Fonts.
def font(size, bold=False):
    candidates = [
        "C:/Windows/Fonts/consolab.ttf" if bold else "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf",
    ]
    for c in candidates:
        try:
            return ImageFont.truetype(c, size)
        except Exception:
            pass
    return ImageFont.load_default()

f_big = font(46, True)
f_mid = font(24, True)
f_small = font(16, False)
f_num = font(18, True)

# Text stays crisp in later progressive scans.
d.text((44, 30), "BLE PROGRESSIVE JPEG", font=f_big, fill=(255, 193, 7), stroke_width=2, stroke_fill=(0, 0, 0))
d.text((48, 84), "first: blurry full image  ->  later: sharp dino, text, and grid", font=f_small, fill=(235, 235, 235))

# Use the requested Chrome Dino image. White background is made transparent;
# the dino itself is kept as the original grey pixel art.
ref = Image.open(REF).convert("RGBA")
# Crop away the thumbnail border, then mask non-white pixels.
ref = ref.crop((25, 20, 197, 205))
scale_h = 230
scale_w = int(ref.width * scale_h / ref.height)
ref = ref.resize((scale_w, scale_h), Image.Resampling.NEAREST)
mask = Image.new("L", ref.size, 0)
mp = mask.load()
rp = ref.load()
for y in range(ref.height):
    for x in range(ref.width):
        r, g, b, a = rp[x, y]
        # Include dark/grey pixels, exclude white thumbnail background.
        lum = (r + g + b) // 3
        mp[x, y] = 255 if lum < 235 else 0
# Slight outline/shadow behind the exact dino for contrast.
dino_x = 300
# center the dino in the main visual area.
dino_x = (W - scale_w) // 2 + 55
dino_y = 126
shadow = Image.new("RGBA", ref.size, (0, 0, 0, 190))
img.paste(shadow, (dino_x + 5, dino_y + 7), mask.filter(ImageFilter.MaxFilter(5)))
img.paste(ref, (dino_x, dino_y), mask)
d = ImageDraw.Draw(img)

# Fine detail block: should be unreadable/soft early and crisp later.
d.text((48, 142), "fine detail", font=f_small, fill=(255, 255, 255), stroke_width=1, stroke_fill=(0, 0, 0))
for y in range(168, 326, 12):
    for x in range(48, 194, 12):
        fill = (255, 255, 255) if ((x // 12) + (y // 12)) % 2 == 0 else (0, 0, 0)
        d.rectangle((x, y, x + 8, y + 8), fill=fill)

# Labels around dino.
d.text((380, 132), "REAL CHROME DINO", font=f_mid, fill=(255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0))
d.text((382, 162), "progressive scans refine edges", font=f_small, fill=(255, 255, 255), stroke_width=1, stroke_fill=(0, 0, 0))

# Bottom chunk strip.
d.text((44, H - 80), "BLE pull mode: lets go -> IMG metadata -> get 0, get 1, get 2 ...", font=f_mid, fill=(255, 255, 255), stroke_width=1, stroke_fill=(0, 0, 0))
for i in range(16):
    x0 = 48 + i * 34
    color = (255, 193, 7) if i % 2 == 0 else (106, 191, 105)
    d.rounded_rectangle((x0, H - 46, x0 + 26, H - 26), radius=5, fill=color, outline=(0, 0, 0), width=2)
    d.text((x0 + 5, H - 45), str(i), font=f_num, fill=(0, 0, 0))

img = img.filter(ImageFilter.UnsharpMask(radius=1.0, percent=120, threshold=2))
img.save(JPG, "JPEG", quality=78, optimize=True, progressive=True)

data = JPG.read_bytes()
lines = [
    "#pragma once",
    "#include <Arduino.h>",
    "",
    "// Generated by tools/make_progressive_demo_image.py",
    "// Progressive JPEG demo image for BLE chunked transfer.",
    f"static const uint32_t DINO_JPG_LEN = {len(data)};",
    "static const uint8_t DINO_JPG[] PROGMEM = {",
]
for i in range(0, len(data), 12):
    chunk = data[i:i + 12]
    suffix = "," if i + 12 < len(data) else ""
    lines.append("  " + ", ".join(f"0x{b:02X}" for b in chunk) + suffix)
lines.append("};")
HDR.write_text("\n".join(lines) + "\n", encoding="ascii")

print(f"wrote {JPG} ({len(data)} bytes)")
print(f"wrote {HDR}")
