"""Colour helpers for gen.py: OKLCH/hex parsing and #RRGGBBAA output."""
import math
import re


def oklch_to_rgb(L, C, H):
    """Convert an OKLCH colour to 8-bit sRGB (clipped to the sRGB gamut)."""
    a = C * math.cos(math.radians(H))
    b = C * math.sin(math.radians(H))
    l_ = L + 0.3963377774 * a + 0.2158037573 * b
    m_ = L - 0.1055613458 * a - 0.0638541728 * b
    s_ = L - 0.0894841775 * a - 1.2914855480 * b
    l, m, s = l_ ** 3, m_ ** 3, s_ ** 3
    r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s
    bb = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s

    def enc(x):
        x = min(1, max(0, x))
        return 12.92 * x if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055

    return tuple(round(enc(v) * 255) for v in (r, g, bb))


def parse(col):
    """Parse 'oklch(L C H [/ A])' or '#RRGGBB[AA]' into an (r, g, b, a) tuple of 0-255 ints."""
    col = col.strip()
    m = re.fullmatch(r"oklch\(\s*([\d.]+)(%?)\s+([\d.]+)\s+([\d.]+)\s*(?:/\s*([\d.]+)(%?))?\s*\)", col)
    if m:
        L = float(m.group(1)) / (100 if m.group(2) else 1)
        rgb = oklch_to_rgb(L, float(m.group(3)), float(m.group(4)))
        a = 1.0
        if m.group(5):
            a = float(m.group(5)) / (100 if m.group(6) else 1)
        return (*rgb, round(a * 255))
    m = re.fullmatch(r"#([0-9a-fA-F]{6})([0-9a-fA-F]{2})?", col)
    if m:
        h = m.group(1)
        return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), int(m.group(2) or "ff", 16))
    raise ValueError(col)


def hx(t):
    """Format an (r, g, b, a) tuple as #RRGGBBAA."""
    return "#%02X%02X%02X%02X" % t
