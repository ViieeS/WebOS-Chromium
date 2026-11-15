#!/usr/bin/env python3
"""splash.py IN OUT: Chromium Beta splash (orange BETA label under the logo)."""
import sys
from PIL import Image, ImageDraw, ImageFont
im = Image.open(sys.argv[1]).convert("RGB"); d = ImageDraw.Draw(im)
f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 44)
x, y = 960, 540 + 128 + 70
d.rounded_rectangle([x - 90, y - 34, x + 90, y + 34], radius=34, fill=(234, 134, 0))
d.text((x, y + 2), "BETA", font=f, fill=(255, 255, 255), anchor="mm")
im.save(sys.argv[2], optimize=True)
