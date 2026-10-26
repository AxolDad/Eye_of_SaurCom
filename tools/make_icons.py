#!/usr/bin/env python3
# The icon of Eye of SaurCom: a burning eye with a slit pupil, watching from a black square (a nod to a certain eye on a tower; the
# name is the joke). Writes every file the build and the packages use: packaging/icons/saurcom.svg, saurcom_1024.png, SaurCom.icns;
# packaging/windows/SaurCom.ico, header.bmp, welcome.bmp. The PNG and the SVG are drawn from the same shapes.
# Run from the repository root: python3 tools/make_icons.py   (needs Pillow)
import math

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

BG = (10, 6, 6)
GLOW = (120, 18, 6)
PUPIL = (6, 2, 2)
TEXT = (255, 196, 120)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def flame(cx, cy, w, h, seed=0.0, lick=1.0, n=480):
    """A ring of fire around (cx, cy): an ellipse pointed at the corners, pushed out by tongues of flame that are tallest above and
    lean outwards. Fixed sums of sines, so every run draws the same picture; seed shifts the tongues between layers."""
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n
        x, y = math.cos(a), math.sin(a)
        r = 1.0 / (abs(x) ** 2.4 + abs(y) ** 2.4) ** (1 / 2.4)
        up = max(0.0, -y)
        side = abs(x)
        t1 = (0.5 + 0.5 * math.cos(9 * a + seed)) ** 6
        t2 = (0.5 + 0.5 * math.cos(16 * a + 2.1 + seed * 1.7)) ** 8
        t3 = (0.5 + 0.5 * math.cos(27 * a + 0.7 + seed * 2.3)) ** 10
        k = 1.0 + lick * ((0.06 + 0.42 * up ** 1.3 + 0.10 * side) * t1 + (0.04 + 0.22 * up) * t2 + (0.03 + 0.10 * up) * t3)
        # tongues lean away from the middle, as flames around a hot centre do
        lean = lick * 0.10 * up * (t1 + t2) * (1 if x > 0 else -1)
        pts.append((cx + (x * r * k + lean) * w / 2, cy + y * r * k * h / 2))
    return pts


def slit(cx, cy, w, h, n=120):
    """The pupil: a tall, thin lens."""
    right = [(cx + w / 2 * math.sin(math.pi * i / n) ** 1.3, cy - h / 2 + h * i / n) for i in range(n + 1)]
    left = [(cx - w / 2 * math.sin(math.pi * i / n) ** 1.3, cy + h / 2 - h * i / n) for i in range(n + 1)]
    return right + left


def streaks(cx, cy, w, h, n=56):
    """Fibres of the iris: lines out from near the pupil to the edge of the eye, at fixed pseudo-random angles and lengths."""
    out = []
    for i in range(n):
        a = 2 * math.pi * (i + 0.5 * math.sin(i * 12.9898)) / n
        r0 = 0.20 + 0.25 * (0.5 + 0.5 * math.sin(i * 78.233))
        r1 = r0 + 0.25 + 0.45 * (0.5 + 0.5 * math.sin(i * 37.719))
        x, y = math.cos(a), math.sin(a)
        e = 1.0 / (abs(x) ** 2.4 + abs(y) ** 2.4) ** (1 / 2.4)
        out.append(((cx + x * e * r0 * w / 2, cy + y * e * r0 * h / 2), (cx + x * e * r1 * w / 2, cy + y * e * r1 * h / 2), i % 3))
    return out


# the layers, outside in: (width, height, seed, lick, colour ramp range)
LAYERS = [(0.74, 0.50, 0.0, 1.0), (0.64, 0.39, 1.3, 0.8), (0.54, 0.28, 2.9, 0.5)]
LAYER_COL = [((150, 16, 4), (230, 70, 12)), ((235, 92, 14), (255, 150, 30)), ((255, 170, 40), (255, 236, 150))]


def geometry(S):
    return dict(cx=S / 2, cy=S * 0.54, pw=S * 0.05, ph=S * 0.30)


def radial(S, cx, cy, rx, ry, c_in, c_out):
    """c_in at the centre to c_out at the ellipse (and beyond)."""
    y, x = np.mgrid[0:S, 0:S]
    t = np.clip(np.sqrt(((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2), 0, 1)[..., None]
    rgb = np.array(c_in, float) * (1 - t) + np.array(c_out, float) * t
    return Image.fromarray(rgb.round().astype(np.uint8), "RGB")


def icon(size):
    s = 2 if size >= 512 else 4  # drawn large and scaled down, for smooth edges
    S = size * s
    g = geometry(S)
    cx, cy = g["cx"], g["cy"]
    small = size < 48
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), radius=int(S * 0.22), fill=255)
    base = Image.new("RGB", (S, S), BG)
    glow = Image.new("L", (S, S), 0)
    ImageDraw.Draw(glow).ellipse((cx - S * 0.44, cy - S * 0.34, cx + S * 0.44, cy + S * 0.30), fill=210)
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.07))
    base = Image.composite(Image.new("RGB", (S, S), GLOW), base, glow)
    for (lw, lh, seed, lick), (c_out, c_in) in zip(LAYERS, LAYER_COL):
        m = Image.new("L", (S, S), 0)
        ImageDraw.Draw(m).polygon(flame(cx, cy, S * lw, S * lh, seed, 0.4 if small else lick), fill=255)
        m = m.filter(ImageFilter.GaussianBlur(S * 0.0015))
        base = Image.composite(radial(S, cx, cy, S * lw * 0.5, S * lh * 0.6, c_in, c_out), base, m)
    if not small:
        d = ImageDraw.Draw(base, "RGBA")
        cols = [(190, 40, 6, 70), (240, 100, 16, 60), (255, 236, 170, 60)]
        for p0, p1, k in streaks(cx, cy, S * 0.56, S * 0.28):
            d.line((p0, p1), fill=cols[k], width=max(1, int(S * 0.0025)))
    # white heat around the pupil
    heat = Image.new("L", (S, S), 0)
    ImageDraw.Draw(heat).polygon(slit(cx, cy, g["pw"] * 3.2, g["ph"] * 0.95), fill=230)
    heat = heat.filter(ImageFilter.GaussianBlur(S * 0.02))
    base = Image.composite(Image.new("RGB", (S, S), (255, 248, 215)), base, heat)
    pw = g["pw"] * (1.6 if small else 1.0)
    ImageDraw.Draw(base).polygon(slit(cx, cy, pw, g["ph"]), fill=PUPIL)
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    im.paste(base, (0, 0), mask)
    return im.resize((size, size), Image.LANCZOS)


def path(pts):
    return "M" + " L".join("%.1f,%.1f" % p for p in pts) + " Z"


def hexc(c):
    return "#%02x%02x%02x" % c


def svg():
    S = 1024
    g = geometry(S)
    cx, cy = g["cx"], g["cy"]
    defs, body = [], []
    for i, ((lw, lh, seed, lick), (c_out, c_in)) in enumerate(zip(LAYERS, LAYER_COL)):
        rx, ry = S * lw * 0.5, S * lh * 0.6
        defs.append(f'''    <radialGradient id="f{i}" gradientUnits="userSpaceOnUse" cx="{cx:.0f}" cy="{cy:.0f}" r="{rx:.0f}" gradientTransform="translate({cx:.0f} {cy:.0f}) scale(1 {ry / rx:.3f}) translate({-cx:.0f} {-cy:.0f})">
      <stop offset="0" stop-color="{hexc(c_in)}"/><stop offset="1" stop-color="{hexc(c_out)}"/>
    </radialGradient>''')
        body.append(f'  <path d="{path(flame(cx, cy, S * lw, S * lh, seed, lick, 240))}" fill="url(#f{i})"/>')
    cols = ["#c82806", "#ff7814", "#ffe696"]
    ops = [0.27, 0.24, 0.24]
    for p0, p1, k in streaks(cx, cy, S * 0.56, S * 0.28):
        body.append(f'  <line x1="{p0[0]:.1f}" y1="{p0[1]:.1f}" x2="{p1[0]:.1f}" y2="{p1[1]:.1f}" stroke="{cols[k]}" stroke-opacity="{ops[k]}" stroke-width="2.5"/>')
    nl = "\n"
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {S} {S}" width="{S}" height="{S}">
  <title>Eye of SaurCom</title>
  <defs>
{nl.join(defs)}
    <radialGradient id="glow" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="{hexc(GLOW)}" stop-opacity="0.85"/><stop offset="1" stop-color="{hexc(GLOW)}" stop-opacity="0"/>
    </radialGradient>
    <filter id="heat" x="-1" y="-1" width="3" height="3"><feGaussianBlur stdDeviation="{S * 0.02:.0f}"/></filter>
  </defs>
  <rect width="{S}" height="{S}" rx="{S * 0.22:.0f}" fill="{hexc(BG)}"/>
  <ellipse cx="{cx:.0f}" cy="{cy:.0f}" rx="{S * 0.52:.0f}" ry="{S * 0.40:.0f}" fill="url(#glow)"/>
{nl.join(body)}
  <path d="{path(slit(cx, cy, g["pw"] * 3.2, g["ph"] * 0.95, 40))}" fill="#fff8d7" opacity="0.9" filter="url(#heat)"/>
  <path d="{path(slit(cx, cy, g["pw"], g["ph"], 40))}" fill="{hexc(PUPIL)}"/>
</svg>
'''


def font(px):
    for f in ("DejaVuSerif-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf", "Georgia Bold.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(f, px)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    big = icon(1024)
    big.save("packaging/icons/saurcom_1024.png")
    open("packaging/icons/saurcom.svg", "w").write(svg())
    big.save("packaging/icons/SaurCom.icns")
    sizes = [16, 24, 32, 48, 64, 128, 256]
    imgs = {n: icon(n) for n in sizes}   # each size drawn on its own, not shrunk from the big one
    imgs[256].save("packaging/windows/SaurCom.ico", sizes=[(n, n) for n in sizes], append_images=[imgs[n] for n in sizes[:-1]])
    # NSIS: the welcome page (164 x 314) and the header (150 x 57), 24-bit BMP
    wel = Image.new("RGB", (164, 314), BG)
    ic = icon(140)
    wel.paste(ic, (12, 60), ic)
    d = ImageDraw.Draw(wel)
    for i, (line, px) in enumerate((("Eye of", 16), ("SaurCom", 22))):
        f = font(px)
        tw = d.textlength(line, font=f)
        d.text(((164 - tw) / 2, 212 + i * 24), line, fill=TEXT, font=f)
    wel.save("packaging/windows/welcome.bmp")
    hdr = Image.new("RGB", (150, 57), (255, 255, 255))
    ic = icon(45)
    hdr.paste(ic, (150 - 45 - 6, 6), ic)
    hdr.save("packaging/windows/header.bmp")


if __name__ == "__main__":
    main()
