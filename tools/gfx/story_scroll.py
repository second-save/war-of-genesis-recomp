#!/usr/bin/env python3
"""Title menu "Story" scroll (TTLDATA/MAL.CF): rebuild it in English.

MAL.CF is a raw 640-pixel-wide bitmap (640 x 3215, colour 1 on 0) that TTL
scrolls through: the legend of Antaria, set in Hangul. The English
(translations/<L>/TTLDATA/story.txt) is set with the game's proportional
ASCII font (NEWASCII.FNT) into a bitmap of the same size.

  story_scroll.py build GAME_DIR TRANS_DIR OUT   -> OUT/TTLDATA/MAL.CF
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "text"))
from ttl_pages import Font  # noqa: E402

W = 640
LEFT, RIGHT = 80, 560      # paragraph column (as the Korean)
LIST_X = 220               # god lists
LINE = 25                  # line pitch of the original


def build(game: Path, trans: Path, out: Path):
    src = game / "TTLDATA" / "MAL.CF"
    h = src.stat().st_size // W
    font = Font((game / "NEWASCII.FNT").read_bytes())
    lines = [l for l in (trans / "TTLDATA" / "story.txt").read_text().split("\n") if not l.startswith("#")]
    px = {}
    y = 3
    para = []

    def flush():
        nonlocal y, para
        if para:
            text = " ".join(para)
            wrapped = font.met.wrap(text, RIGHT - LEFT - 8)
            for i, l in enumerate(wrapped):
                font.draw(px, LEFT + (8 if i == 0 else 0), y, l)
                y += LINE
            para = []

    for l in lines:
        if l.startswith("= "):
            flush()
            t = l[2:]
            font.draw(px, (W - font.met.width(t)) // 2, y, t)
            y += LINE
        elif l.startswith("> "):
            flush()
            font.draw(px, LIST_X, y, l[2:])
            y += LINE
        elif l.startswith("~ "):
            flush()
            t = l[2:]
            font.draw(px, 96 + 8 * (len(t) - len(t.lstrip())), y, t.lstrip())
            y += LINE
        elif not l.strip():
            flush()
            y += LINE
        else:
            para.append(l.strip())
    flush()
    if y > h:
        sys.exit(f"story scroll needs {y} px, has {h}")
    data = bytearray(W * h)
    for (x, yy), c in px.items():
        if 0 <= x < W and 0 <= yy < h:
            data[yy * W + x] = c
    dst = out / "TTLDATA" / "MAL.CF"
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(bytes(data))
    print(f"story scroll: {y} of {h} px used")


if __name__ == "__main__":
    if len(sys.argv) != 5 or sys.argv[1] != "build":
        sys.exit(__doc__)
    build(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
