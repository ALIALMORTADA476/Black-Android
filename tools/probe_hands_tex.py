#!/usr/bin/env python3
"""Check the hands texture statistics: is it actually a skin texture or near-white?"""
import os, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_hands_glb as h
import build_m91_glb as m91

img = Image.open(h.HANDS_TEX)
print("mode:", img.mode, "size:", img.size)
arr = np.asarray(img.convert("RGB"), dtype=np.float64)
print("mean RGB:", arr.reshape(-1,3).mean(axis=0))
print("overall mean luminance: %.1f" % arr.mean())
print("min/max:", arr.min(), arr.max())
# histogram of luminance
lum = arr.mean(axis=2)
print("frac pixels lum>240: %.4f" % np.mean(lum > 240))
print("frac pixels lum<40: %.4f" % np.mean(lum < 40))

# Also check the baked btex
btex_path = os.path.join(os.path.dirname(h.__file__), "..", "app", "src", "main", "assets", "weapons", "weapon_hands.btex")
if os.path.exists(btex_path):
    with open(btex_path, "rb") as f:
        hdr = f.read(32)
        data = np.frombuffer(f.read(), dtype=np.uint8)
    print("\nBTEX header:", hdr[:16].hex())
    # BTEX pixel data at offset 32, BGRA
    px = data.reshape(-1, 4) if len(data) % 4 == 0 else None
    if px is not None and len(px) == 1024*1024:
        b,g,r,a = px[:,0].astype(float), px[:,1].astype(float), px[:,2].astype(float), px[:,3].astype(float)
        print("BTEX mean BGRA:", b.mean(), g.mean(), r.mean(), a.mean())
        lum2 = (0.114*b + 0.587*g + 0.299*r)
        print("BTEX mean luminance: %.1f" % lum2.mean())
