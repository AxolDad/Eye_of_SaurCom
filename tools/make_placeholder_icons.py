#!/usr/bin/env python3
# The placeholder icon of Eye of ZorCom: an eye on a dark rounded square, until the project has its own artwork. Writes every file the build
# and the packages use: packaging/icons/zorcom.svg, zorcom_1024.png, ZorCom.icns; packaging/windows/ZorCom.ico, header.bmp, welcome.bmp.
# Run from the repository root: python3 tools/make_placeholder_icons.py   (needs Pillow)
import math
from PIL import Image, ImageDraw, ImageFont

BG = (27, 35, 43)
ACCENT, WHITE, PUPIL = (79, 184, 209), (236, 240, 243), (12, 16, 20)


def eye_outline(cx, cy, w, h, n=120):
    """The almond of the eye: two circular arcs meeting at the corners."""
    pts = []
    for i in range(n + 1):
        t = math.pi * i / n
        pts.append((cx - w / 2 + w * i / n, cy - h / 2 * math.sin(t)))
    for i in range(n + 1):
        t = math.pi * i / n
        pts.append((cx + w / 2 - w * i / n, cy + h / 2 * math.sin(t)))
    return pts


def icon(size):
    s = 4  # drawn large and scaled down, for smooth edges
    S = size * s
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    r = int(S * 0.22)
    d.rounded_rectangle((0, 0, S - 1, S - 1), radius=r, fill=BG)
    cx, cy = S / 2, S / 2
    w, h = S * 0.74, S * 0.42
    d.polygon(eye_outline(cx, cy, w, h), fill=WHITE)
    ir = h * 0.46
    d.ellipse((cx - ir, cy - ir, cx + ir, cy + ir), fill=ACCENT)
    pr = ir * 0.48
    d.ellipse((cx - pr, cy - pr, cx + pr, cy + pr), fill=PUPIL)
    hr = ir * 0.18
    hx, hy = cx - ir * 0.32, cy - ir * 0.34
    d.ellipse((hx - hr, hy - hr, hx + hr, hy + hr), fill=WHITE)
    return im.resize((size, size), Image.LANCZOS)


def svg():
    S = 1024
    cx = cy = S / 2
    w, h = S * 0.74, S * 0.42
    ir = h * 0.46
    pr = ir * 0.48
    hr = ir * 0.18
    hx, hy = cx - ir * 0.32, cy - ir * 0.34
    pts = eye_outline(cx, cy, w, h, 60)
    path = "M" + " L".join("%.1f,%.1f" % p for p in pts) + " Z"
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {S} {S}" width="{S}" height="{S}">
  <title>Eye of ZorCom (placeholder icon)</title>
  <rect width="{S}" height="{S}" rx="{S * 0.22:.0f}" fill="#1b232b"/>
  <path d="{path}" fill="#ecf0f3"/>
  <circle cx="{cx:.0f}" cy="{cy:.0f}" r="{ir:.1f}" fill="#4fb8d1"/>
  <circle cx="{cx:.0f}" cy="{cy:.0f}" r="{pr:.1f}" fill="#0c1014"/>
  <circle cx="{hx:.1f}" cy="{hy:.1f}" r="{hr:.1f}" fill="#ecf0f3"/>
</svg>
'''


def font(px):
    for f in ("DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "Arial Bold.ttf"):
        try:
            return ImageFont.truetype(f, px)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    big = icon(1024)
    big.save("packaging/icons/zorcom_1024.png")
    open("packaging/icons/zorcom.svg", "w").write(svg())
    big.save("packaging/icons/ZorCom.icns")
    big.save("packaging/windows/ZorCom.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    # NSIS: the welcome page (164 x 314) and the header (150 x 57), 24-bit BMP
    wel = Image.new("RGB", (164, 314), BG)
    ic = icon(120)
    wel.paste(ic, (22, 70), ic)
    d = ImageDraw.Draw(wel)
    f = font(17)
    for i, line in enumerate(("Eye of", "ZorCom")):
        tw = d.textlength(line, font=f)
        d.text(((164 - tw) / 2, 205 + i * 24), line, fill=WHITE, font=f)
    wel.save("packaging/windows/welcome.bmp")
    hdr = Image.new("RGB", (150, 57), WHITE)
    ic = icon(45)
    hdr.paste(ic, (150 - 45 - 6, 6), ic)
    hdr.save("packaging/windows/header.bmp")


if __name__ == "__main__":
    main()
