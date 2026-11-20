#!/usr/bin/env python3
"""badge.py IN OUT [LABEL] [small]: Chromium Beta icon (orange "B" badge, bottom right).
With LABEL (e.g. "webOS 4"): an orange label badge across the bottom instead, for a separate tester app.
With LABEL and "small": a small grey label in the bottom-right corner (the old app id's last release: "OLD")."""
import sys
from PIL import Image, ImageDraw, ImageFont
F = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
im = Image.open(sys.argv[1]).convert("RGBA"); W, H = im.size
d = ImageDraw.Draw(im)
label = sys.argv[3] if len(sys.argv) > 3 else ""
small = len(sys.argv) > 4 and sys.argv[4] == "small"
if label and small:
    # grey pill, about a fifth of the icon's height, text width plus padding, in the bottom-right corner
    h = max(12, int(H * 0.22)); font = ImageFont.truetype(F, max(7, int(h * 0.62)))
    l, t, rr, b = d.textbbox((0, 0), label, font=font)
    w = (rr - l) + int(h * 0.8)
    x1, y1 = W - 2, H - 2; x0, y0 = x1 - w, y1 - h
    d.rounded_rectangle([x0 - 2, y0 - 2, x1 + 1, y1 + 1], radius=h // 2 + 2, fill=(255, 255, 255, 255))
    d.rounded_rectangle([x0, y0, x1, y1], radius=h // 2, fill=(95, 99, 104, 255))
    d.text(((x0 + x1) / 2, (y0 + y1) / 2 + 1), label, font=font, fill=(255, 255, 255, 255), anchor="mm")
elif not label:
    r = int(W * 0.24); cx, cy = W - r - 1, H - r - 1
    d.ellipse([cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2], fill=(255, 255, 255, 255))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(234, 134, 0, 255))
    d.text((cx, cy + 1), "B", font=ImageFont.truetype(F, int(r * 1.35)), fill=(255, 255, 255, 255), anchor="mm")
else:
    # pill along the bottom edge; the font shrinks until the label fits with some padding
    h = int(H * 0.30); x0, y0, x1, y1 = int(W * 0.02), H - h - 1, W - int(W * 0.02) - 1, H - 1
    size = int(h * 0.72)
    while size > 6:
        font = ImageFont.truetype(F, size)
        l, t, rr, b = d.textbbox((0, 0), label, font=font)
        if rr - l <= (x1 - x0) - h * 0.6:
            break
        size -= 1
    d.rounded_rectangle([x0 - 2, y0 - 2, x1 + 2, y1 + 2], radius=h // 2 + 2, fill=(255, 255, 255, 255))
    d.rounded_rectangle([x0, y0, x1, y1], radius=h // 2, fill=(234, 134, 0, 255))
    d.text(((x0 + x1) / 2, (y0 + y1) / 2 + 1), label, font=font, fill=(255, 255, 255, 255), anchor="mm")
im.save(sys.argv[2])
