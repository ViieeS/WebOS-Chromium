#!/usr/bin/env python3
"""badge.py IN OUT [LABEL]: Chromium Beta icon (orange "B" badge, bottom right).
With LABEL (e.g. "webOS 4"): an orange label badge across the bottom instead, for a separate tester app."""
import sys
from PIL import Image, ImageDraw, ImageFont
F = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
im = Image.open(sys.argv[1]).convert("RGBA"); W, H = im.size
d = ImageDraw.Draw(im)
label = sys.argv[3] if len(sys.argv) > 3 else ""
if not label:
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
