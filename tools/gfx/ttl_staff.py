#!/usr/bin/env python3
"""Ending credits (TTLDATA/STAFF.SPR): romanise the Hangul names.

The frames are white text (colour 1) on black boxes (colour 0); the role titles
are already English. Text rows are found by scanning for runs of lines with
white pixels; each row listed in translations/<L>/TTLDATA/staff.json is
erased and redrawn in capitals (Copperplate, like the titles), centred where
the Korean was, with the same colour-0 shadow. Needs the font: without it the file is left out (Korean names).

  ttl_staff.py build GAME_DIR TRANS_DIR OUT   -> OUT/TTLDATA/STAFF.SPR
"""
import json
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spr  # noqa: E402

FONTS = ["/System/Library/Fonts/Supplemental/Copperplate.ttc"]


def rows(px):
    ys = sorted({y for (x, y), c in px.items() if c == 1})
    out, start, prev = [], None, None
    for y in ys:
        if start is None:
            start = prev = y
        elif y <= prev + 2:
            prev = y
        else:
            out.append((start, prev))
            start = prev = y
    if start is not None:
        out.append((start, prev))
    return out


def build(game: Path, trans: Path, out: Path):
    font_path = next((f for f in FONTS if Path(f).exists()), None)
    if not font_path:
        print("  ttl_staff: no font, credits stay Korean")
        return
    cfg = json.loads((trans / "TTLDATA" / "staff.json").read_text())["frames"]
    src = (game / "TTLDATA" / "STAFF.SPR").read_bytes()
    frames = spr.frames(src, 640)
    body = bytearray(src[:0x80])
    for (p, q, f), repl in zip(frames, cfg + [[]] * len(frames)):
        px = f.pixels()
        rs = rows(px)
        # Height of an English title row: the font size to use.
        title = [b - a + 1 for (a, b), r in zip(rs, repl) if r is None] or [12]
        cap = max(title)
        w = max(f.w, max(x for x, _ in px) + 1)  # header width (the boxes are 206 px)
        for (a, b), text in zip(rs, repl):
            if text is None:
                continue
            xs = [x for (x, y), c in px.items() if c == 1 and a <= y <= b]
            cx = (min(xs) + max(xs)) // 2
            for k in [k for k in px if a - 1 <= k[1] <= b + 4]:
                del px[k]  # the Korean row and its shadow
            size = 15
            while True:
                fnt = ImageFont.truetype(font_path, size)
                bb = fnt.getbbox(text.upper())
                if (bb[2] - bb[0]) <= w - 6 or size <= 8:
                    break
                size -= 1
            im = Image.new("L", (bb[2] + 4, bb[3] + 4), 0)
            ImageDraw.Draw(im).text((0, 0), text.upper(), font=fnt, fill=255)
            ox = max(2, min(w - (bb[2] - bb[0]) - 2, cx - (bb[2] - bb[0]) // 2)) - bb[0]
            oy = (a + b) // 2 - (bb[1] + bb[3]) // 2
            on = {(ox + x, oy + y) for y in range(im.height) for x in range(im.width)
                  if im.getpixel((x, y)) >= 120}
            for (x, y) in on:  # shadow (colour 0) down-right, as in the original
                if (x + 1, y + 1) not in on:
                    px[(x + 1, y + 1)] = 0
            for t in on:
                px[t] = 1
        runs = spr.vsp_encode(px, 640)
        hdr = bytearray(src[p:p + 0x20])
        struct.pack_into("<I", hdr, 0, len(runs))
        body += hdr + runs
    dst = out / "TTLDATA" / "STAFF.SPR"
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(bytes(body))
    print("credits built")


if __name__ == "__main__":
    if len(sys.argv) != 5 or sys.argv[1] != "build":
        sys.exit(__doc__)
    build(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
