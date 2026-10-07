#!/usr/bin/env python3
"""Render the Photon64 project banner (assets/banner.png).

Design language matches the app (src/web/app.html :root, src/web/art.js):
dark #0e0f13 canvas, quad palette green/blue/red/yellow, the cartridge
mark with its four-field label + spark, rounded wordmark, 4-color stripe.
Renders at 2x then downscales for crisp edges.

Usage: .venv/bin/python assets/banner.py [--out assets/banner.png]
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
try:
    from PIL import Image, ImageDraw, ImageFont, ImageFilter
except ImportError:
    sys.exit("needs Pillow: .venv/bin/pip install -r tools/requirements.txt")

W, H, SS = 1280, 640, 2  # final size + supersample
BG = (14, 15, 19)
GREEN, BLUE, RED, YELLOW = (34, 179, 92), (59, 123, 255), (234, 67, 53), (246, 194, 28)
WHITE, DIM = (243, 244, 246), (154, 161, 174)
FONTS = "/System/Library/Fonts/Supplemental"
F_ROUND = f"{FONTS}/Arial Rounded Bold.ttf"
F_ITAL = f"{FONTS}/Arial Bold Italic.ttf"
F_REG = f"{FONTS}/Arial.ttf"


def vgrad(w, h, top, bot):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        px_line = tuple(round(a + (b - a) * t) for a, b in zip(top, bot))
        for x in range(w):
            px[x, y] = px_line
    return img


def radial(w, h, color, alpha_max):
    """Radial glow, brightest at center, fading to transparent at edges."""
    import numpy as np
    yy, xx = np.mgrid[0:h, 0:w]
    d = np.sqrt(((xx - w / 2) / (w / 2)) ** 2 + ((yy - h / 2) / (h / 2)) ** 2)
    a = np.clip(1 - d, 0, 1) ** 1.6 * alpha_max
    layer = np.zeros((h, w, 4), np.uint8)
    layer[..., 0:3] = color
    layer[..., 3] = a.astype(np.uint8)
    return Image.fromarray(layer, "RGBA")


def shell_outline(s, ox, oy):
    """Cartridge shell polygon in device px (48-unit art.js geometry)."""
    pts = []
    # top edge + rounded top corners (r=4 units, centers (9,9) and (39,9))
    for cx, a0 in ((9, 180), (39, 270)):
        for i in range(13):
            a = math.radians(a0 + i * 90 / 12)
            pts.append((ox + (cx + 4 * math.cos(a)) * s, oy + (9 + 4 * math.sin(a)) * s))
    # right side, step in, right leg, bottom, left leg, step out, left side
    for x, y in ((43, 32), (39.8, 35.2), (39.8, 43), (8.2, 43), (8.2, 35.2), (5, 32)):
        pts.append((ox + x * s, oy + y * s))
    # close up the left side (arc points already cover top)
    pts.append((ox + 5 * s, oy + 9 * s))
    return pts


def draw_logo(img, cx, top, size):
    """Draw the cartridge mark; size = height px, cx = center x."""
    s = size / 48 * SS
    ox, oy = (cx * SS - 24 * s), top * SS
    canvas = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(canvas)
    pts = shell_outline(s, ox, oy)
    # vertical gradient shell through the outline mask
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).polygon(pts, fill=255)
    grad = vgrad(img.size[0], img.size[1], (112, 118, 127), (58, 62, 70))
    img.paste(Image.composite(grad.convert("RGBA"), Image.new("RGBA", img.size, (0, 0, 0, 0)), mask), (0, 0), mask)
    # outline stroke
    d.line(pts + [pts[0]], fill=(154, 160, 170, 140), width=max(2, int(s * 0.5)), joint="curve")
    # contact notches
    for x0 in (12, 17.2, 27.8, 33):
        d.rectangle([ox + x0 * s, oy + 38 * s, ox + (x0 + 3) * s, oy + 43 * s], fill=(34, 37, 43, 255))
    # four-field label
    lx, ly, lw, lh = ox + 10 * s, oy + 9.5 * s, 28 * s, 20 * s
    lm = Image.new("L", img.size, 0)
    ImageDraw.Draw(lm).rounded_rectangle([lx, ly, lx + lw, ly + lh], radius=3 * s, fill=255)
    label = Image.new("RGBA", img.size, (0, 0, 0, 0))
    dl = ImageDraw.Draw(label)
    dl.rectangle([lx, ly, lx + lw / 2, ly + lh / 2], fill=GREEN + (255,))
    dl.rectangle([lx + lw / 2, ly, lx + lw, ly + lh / 2], fill=BLUE + (255,))
    dl.rectangle([lx, ly + lh / 2, lx + lw / 2, ly + lh], fill=RED + (255,))
    dl.rectangle([lx + lw / 2, ly + lh / 2, lx + lw, ly + lh], fill=YELLOW + (255,))
    # white spark
    sp = [(24, 12.2), (25.4, 18), (30.6, 19.5), (25.4, 21), (24, 26.8), (22.6, 21), (17.4, 19.5), (22.6, 18)]
    dl.polygon([(ox + x * s, oy + y * s) for x, y in sp], fill=(255, 255, 255, 255))
    img.paste(Image.composite(label, Image.new("RGBA", img.size, (0, 0, 0, 0)), lm), (0, 0), lm)
    img.alpha_composite(canvas)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="assets/banner.png")
    args = ap.parse_args()
    img = vgrad(W * SS, H * SS, (17, 19, 25), BG).convert("RGBA")
    # soft blue aura behind the mark + faint warm echo lower right
    aura = radial(W * SS, H * SS, BLUE, 46)
    img.alpha_composite(aura, (int(-W * SS * 0.28), int(-H * SS * 0.28)))
    echo = radial(W * SS // 2, H * SS // 2, (246, 194, 28), 14)
    img.alpha_composite(echo, (int(W * SS * 0.55), int(H * SS * 0.45)))

    draw_logo(img, cx=280, top=160, size=360)

    d = ImageDraw.Draw(img)
    tx = 560 * SS
    avail = W * SS - tx - 64 * SS

    def word_width(px):
        fw = ImageFont.truetype(F_ROUND, px)
        f6 = ImageFont.truetype(F_ITAL, px)
        return fw, f6, d.textlength("Photon", font=fw) + 6 * SS + d.textlength("64", font=f6)

    px = 148 * SS
    f_word, f_64, word_w = word_width(px)
    while word_w > avail and px > 40 * SS:  # auto-fit the wordmark
        px = int(px * avail / word_w)
        f_word, f_64, word_w = word_width(px)
    cy = 260 * SS
    d.text((tx, cy), "Photon", font=f_word, fill=WHITE + (255,), anchor="lm")
    w_photon = d.textlength("Photon", font=f_word)
    d.text((tx + w_photon + 6 * SS, cy), "64", font=f_64, fill=BLUE + (255,), anchor="lm")

    tag = "A Nintendo 64 emulator for the browser."
    tpx = 46 * SS
    f_tag = ImageFont.truetype(F_REG, tpx)
    while d.textlength(tag, font=f_tag) > avail and tpx > 20 * SS:
        tpx = int(tpx * avail / d.textlength(tag, font=f_tag))
        f_tag = ImageFont.truetype(F_REG, tpx)
    d.text((tx, 382 * SS), tag, font=f_tag, fill=DIM + (255,), anchor="lm")

    # signature 4-color stripe, wordmark width
    sy, sh = 448 * SS, 10 * SS
    stripe = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ds = ImageDraw.Draw(stripe)
    seg = word_w / 4
    for i, c in enumerate((GREEN, BLUE, RED, YELLOW)):
        ds.rectangle([tx + seg * i, sy, tx + seg * (i + 1), sy + sh], fill=c + (255,))
    sm = Image.new("L", img.size, 0)
    ImageDraw.Draw(sm).rounded_rectangle([tx, sy, tx + word_w, sy + sh], radius=sh // 2, fill=255)
    img.paste(Image.composite(stripe, Image.new("RGBA", img.size, (0, 0, 0, 0)), sm), (0, 0), sm)

    img.convert("RGB").resize((W, H), Image.LANCZOS).save(args.out)
    print("wrote", args.out)


main()
