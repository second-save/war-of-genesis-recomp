#!/usr/bin/env python3
"""Character gallery pages (TTLDATA/*_.SPR): rebuild them in English.

Each page is a one-colour picture of Korean text (colour 1) in a TTL sprite
file (long frame headers, 640-pixel pitch; BLACK_/CROW_ have two frames). The
English is word-wrapped and drawn with the game's proportional ASCII font
(NEWASCII.FNT, as the dialogue hooks draw it) into the same frame sizes;
text that doesn't fit on the first frame continues on the second.

  ttl_pages.py build GAME_DIR TRANS_DIR OUT   -> OUT/TTLDATA/*_.SPR
"""
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "text"))
import spr  # noqa: E402
from talk import Metrics  # noqa: E402

LINE = 18     # px between lines (16-px font)
COLOUR = 1


class Font:
    def __init__(self, data: bytes):
        self.data = data
        self.met = Metrics(data)

    def draw(self, px, x, y, text):
        for ch in text:
            if ch == " ":
                x += 4
                continue
            g = self.data[ord(ch) * 16:ord(ch) * 16 + 16]
            bits = 0
            for r in g:
                bits |= r
            cols = [k for k in range(8) if bits & (0x80 >> k)]
            if not cols:
                x += 4
                continue
            for r in range(16):
                for k in range(8):
                    if g[r] & (0x80 >> k):
                        px[(x + k - cols[0], y + r)] = COLOUR
            x += cols[-1] - cols[0] + 2


def build(game: Path, trans: Path, out: Path):
    font = Font((game / "NEWASCII.FNT").read_bytes())
    for page in json.loads((trans / "TTLDATA" / "pages.json").read_text()):
        src = (game / "TTLDATA" / page["file"]).read_bytes()
        frames = spr.frames(src, 640)
        lines = font.met.wrap(page["en"], frames[0][2].w - 8)
        body = bytearray(src[:0x80])
        for n, (p, q, f) in enumerate(frames):
            per = (f.h - 16) // LINE + 1
            if n == len(frames) - 1:
                per = max(per, len(lines))  # last frame grows to fit the rest
            mine, lines = lines[:per], lines[per:]
            h = max(f.h, (len(mine) - 1) * LINE + 16)
            px = {}
            for i, l in enumerate(mine):
                font.draw(px, 0, i * LINE, l)
            px = {k: v for k, v in px.items() if k[0] < f.w and k[1] < h}
            runs = spr.vsp_encode(px, 640) if px else struct.pack("<H", 0)
            hdr = bytearray(src[p:p + 0x20])
            struct.pack_into("<IHH", hdr, 0, len(runs), f.w, h)
            if h > f.h:
                print(f"  {page['file']}: frame {n} {f.h} -> {h} px high")
            body += hdr + runs
        if lines:
            print(f"  {page['file']}: {len(lines)} lines don't fit")
        dst = out / "TTLDATA" / page["file"]
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(bytes(body))
    print("character pages built")


if __name__ == "__main__":
    if len(sys.argv) != 5 or sys.argv[1] != "build":
        sys.exit(__doc__)
    build(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
