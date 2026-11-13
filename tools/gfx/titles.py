#!/usr/bin/env python3
"""Chapter title cards (CHAPDATA/PERIODn.VSP): render English replacements.

The originals are Hanja/Hangul calligraphy in an orange ramp, drawn into the
640x480 prologue screen. Each English title is set in the original's bounding
box (one line, or two when it doesn't fit), filled with the original's colour
ramp row by row and outlined with its darkest colour, using only colours the
original uses (the prologue palette is shared with the rest of the screen).

The generated files are committed to translations/en/CHAPDATA/ (they contain
only rendered text), so building the overlay doesn't need the font.

  titles.py build GAME_DIR TRANS_DIR [FONT]   -> TRANS_DIR/CHAPDATA/PERIODn.VSP
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spr  # noqa: E402

FONT = "/System/Library/Fonts/Supplemental/Georgia Bold Italic.ttf"


def ramp(px, pal):
    """Colours of the big glyphs, top to bottom: [(index, rgb)], and the darkest index."""
    rows = {}
    ys = [y for _, y in px]
    y0 = max(ys) - (max(ys) - min(ys)) * 2 // 3  # lower part: the large glyphs, not the ruby
    for (x, y), c in px.items():
        if y >= y0:
            rows.setdefault(y, []).append(c)
    rgb = lambda c: tuple(pal[3 * c:3 * c + 3])  # noqa: E731
    lum = lambda c: sum(rgb(c))  # noqa: E731
    used = sorted(set(px.values()), key=lum)
    dark = used[0]
    bright = [c for c in used if lum(c) > lum(dark) + 60] or used
    out = []
    for y in sorted(rows):
        cs = [c for c in rows[y] if c in bright] or rows[y]
        best = max(set(cs), key=cs.count)
        out.append(best)
    return out, dark


def layout(text, w, h, font_path):
    """Largest size (<= 30 px) that fits in w x h, on one line or two."""
    for size in range(30, 9, -1):
        f = ImageFont.truetype(font_path, size)
        for lines in ([text], split2(text)):
            if not lines:
                continue
            boxes = [f.getbbox(l) for l in lines]
            lw = max(b[2] - b[0] for b in boxes) + 4
            asc, desc = f.getmetrics()
            if lw <= w and (asc + desc) * len(lines) <= h:
                return f, lines, size
    raise ValueError(f"'{text}' doesn't fit {w}x{h}")


def split2(text):
    words = text.split()
    if len(words) < 2:
        return None
    best = min(range(1, len(words)), key=lambda i: abs(len(" ".join(words[:i])) - len(" ".join(words[i:]))))
    return [" ".join(words[:best]), " ".join(words[best:])]


def render(text, orig_px, pal, font_path):
    xs = [x for x, _ in orig_px]
    ys = [y for _, y in orig_px]
    w, h = max(xs) + 1, max(ys) + 1
    font, lines, size = layout(text, w, h, font_path)
    colours, dark = ramp(orig_px, pal)
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)
    asc, desc = font.getmetrics()
    lh = asc + desc
    top = (h - lh * len(lines)) // 2 + (h - lh * len(lines)) // 4  # sit a little low, like the Hanja
    for i, l in enumerate(lines):
        b = font.getbbox(l)
        x = (w - (b[2] - b[0])) // 2 - b[0]
        d.text((x, top + i * lh), l, font=font, fill=255)
    on = {(x, y) for y in range(h) for x in range(w) if mask.getpixel((x, y)) >= 110}
    px = {}
    for (x, y) in on:
        ty = min(len(colours) - 1, max(0, ((y - top) % lh) * len(colours) // max(1, lh)))
        px[(x, y)] = colours[ty]
    for (x, y) in list(on):  # outline below/right for depth, as in the original
        for dx, dy in ((1, 1), (1, 0), (0, 1)):
            q = (x + dx, y + dy)
            if q not in on and q[0] < w and q[1] < h:
                px[q] = dark
    return px


def build(game: Path, trans: Path, font_path=FONT):
    pal = spr.palette(game / "CHAPDATA" / "PTTL.PAL")
    for t in json.loads((trans / "CHAPDATA" / "titles.json").read_text()):
        orig = spr.vsp_pixels((game / "CHAPDATA" / t["file"]).read_bytes())
        px = render(t["en"], orig, pal, font_path)
        (trans / "CHAPDATA" / t["file"]).write_bytes(spr.vsp_encode(px))
        print(t["file"], t["en"])


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5) or sys.argv[1] != "build":
        sys.exit(__doc__)
    build(Path(sys.argv[2]), Path(sys.argv[3]), *(sys.argv[4:5] or [FONT]))
