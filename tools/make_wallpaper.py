#!/usr/bin/env python3
"""Synthesize the Vista-style aurora wallpaper for Shiro Mini OS.

Renders a deep-teal aurora backdrop with flowing cyan/green light
curtains and a soft yellow-green glow near the bottom, in the spirit
of the Windows Vista default wallpaper, and writes it as a JPEG the
kernel decodes with stb_image.

Design constraint (from tests/qemu_test.py): the harness counts
"desktop" pixels inside the old gradient's RGB envelope
(R 0..26, G 18..66, B 30..87, B > R+8), so the base gradient stays
in that dark-teal family and the bright curtains cover a minority
of the area.

Usage: python3 tools/make_wallpaper.py [output.jpg]
"""
import math
import sys

from PIL import Image

W, H = 1024, 640


def render():
    img = Image.new("RGB", (W, H))
    px = img.load()

    # Aurora curtains: (phase, slope, width, color) bands whose
    # centerline undulates with y = a*sin(x*f + p) + slope*x.
    bands = [
        (0.0, 0.16, 95.0, (40, 170, 130)),   # bright cyan-green
        (2.1, 0.10, 60.0, (52, 185, 168)),    # cyan
        (4.0, 0.22, 130.0, (26, 130, 96)),   # emerald wash
        (1.2, 0.05, 40.0, (90, 195, 140)),   # thin bright ribbon
    ]
    for y in range(H):
        v = y / (H - 1)
        # vertical base gradient: deep teal, stays in the harness
        # gradient envelope (dark family) over most of the frame
        br = 9 + 7 * v
        bg = 40 + 22 * v
        bb = 58 + 14 * v
        for x in range(W):
            u = x / (W - 1)
            r, g, b = br, bg, bb
            # aurora curtains (additive glow, gaussian falloff)
            for phase, slope, width, (cr, cg, cb) in bands:
                cy = (0.30 + 0.22 * math.sin(u * 4.2 + phase)
                      + slope * (u - 0.5)) * H
                d = (y - cy) / width
                glow = math.exp(-d * d) * (0.55 + 0.45 * math.sin(u * 9.0 + phase * 2.0) ** 2)
                fade = 1.0 - 0.45 * v  # curtains dim toward the bottom
                r += cr * glow * 0.21 * fade
                g += cg * glow * 0.21 * fade
                b += cb * glow * 0.21 * fade
            # bottom-left yellow-green glow (Vista's light pool)
            dx = (u - 0.18) * 1.4
            dy = (v - 1.05) * 1.9
            pool = math.exp(-(dx * dx + dy * dy) * 2.6)
            r += 46 * pool
            g += 62 * pool
            b += 13 * pool
            # gentle vignette
            vx = (u - 0.5) * 2
            vy = (v - 0.45) * 2
            vig = 1.0 - 0.16 * (vx * vx + vy * vy)
            px[x, y] = (max(0, min(255, int(r * vig))),
                        max(0, min(255, int(g * vig))),
                        max(0, min(255, int(b * vig))))
    return img


if __name__ == "__main__":
    import os
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/wallpaper.jpg"
    parent = os.path.dirname(out)
    if parent:
        os.makedirs(parent, exist_ok=True)
    render().save(out, quality=88)
    print("wrote", out)
