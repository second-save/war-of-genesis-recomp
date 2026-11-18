#!/usr/bin/env python3
"""Battle technique icons (SPECIAL.ICO): English labels in place of the Hanja.

SPECIAL.ICO holds 18 icons of 4 frames each (a coin spin: full, narrow, edge,
narrow mirrored), 24x24, shown in the technique ring of a sword, spear, axe or
bow user (CHRSYS 100C:0C3B: sword icons 0-6, spear 7-10, axe 11-14, bow 15-17).
Each original is one Hanja in white (colour 1) outlined in blue (42). The
English label is set in a built-in 3x5 pixel font with the same colours; the
narrow frames are the label squeezed, the edge frame is kept.

  special_icons.py build GAME_DIR TRANS_DIR OUT_DIR   -> OUT_DIR/SPECIAL.ICO
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spr  # noqa: E402

# Icon order of the file; the Hanja and what it stands for.
LABELS = [
    ("COMBO",),        # 連 consecutive strikes   (sword)
    ("BIND",),         # 制 restrain
    ("COUN", "TER"),   # 反 counter
    ("FLY",),          # 飛 flying blow
    ("KILL",),         # 殺 deadly blow
    ("SOUL",),         # 魂 soul
    ("BREAK",),        # 破 break
    ("BIND",),         # 制                       (spear)
    ("COMBO",),        # 連
    ("FLY",),          # 飛
    ("SOUL",),         # 魂
    ("COUN", "TER"),   # 反                       (axe)
    ("KILL",),         # 殺
    ("FLY",),          # 飛
    ("RAGE",),         # 狂 frenzy
    ("KILL",),         # 殺                       (bow)
    ("COMBO",),        # 連
    ("BOLT",),         # 雷 thunder
]

FONT = {  # 3x5
    "A": [".#.", "#.#", "###", "#.#", "#.#"],
    "B": ["##.", "#.#", "##.", "#.#", "##."],
    "C": [".##", "#..", "#..", "#..", ".##"],
    "D": ["##.", "#.#", "#.#", "#.#", "##."],
    "E": ["###", "#..", "##.", "#..", "###"],
    "F": ["###", "#..", "##.", "#..", "#.."],
    "G": [".##", "#..", "#.#", "#.#", ".##"],
    "I": ["###", ".#.", ".#.", ".#.", "###"],
    "K": ["#.#", "#.#", "##.", "#.#", "#.#"],
    "L": ["#..", "#..", "#..", "#..", "###"],
    "M": ["#...#", "##.##", "#.#.#", "#...#", "#...#"],
    "N": ["##.", "#.#", "#.#", "#.#", "#.#"],
    "O": [".#.", "#.#", "#.#", "#.#", ".#."],
    "R": ["##.", "#.#", "##.", "#.#", "#.#"],
    "S": [".##", "#..", ".#.", "..#", "##."],
    "T": ["###", ".#.", ".#.", ".#.", ".#."],
    "U": ["#.#", "#.#", "#.#", "#.#", ".#."],
    "Y": ["#.#", "#.#", ".#.", ".#.", ".#."],
}
SIZE = 24
INK, EDGE = 1, 42


def label_bits(lines):
    """Set of (x, y) ink pixels, centred in the 24x24 frame."""
    h = 6 * len(lines) - 1
    y0 = (SIZE - h) // 2 + 1  # the Hanja sit slightly low (rows 7-18)
    ink = set()
    for li, word in enumerate(lines):
        w = sum(len(FONT[ch][0]) + 1 for ch in word) - 1
        x = (SIZE - w) // 2
        for ch in word:
            for y, row in enumerate(FONT[ch]):
                for dx, c in enumerate(row):
                    if c == "#":
                        ink.add((x + dx, y0 + 6 * li + y))
            x += len(FONT[ch][0]) + 1
    return ink


def outlined(ink):
    px = {}
    for x, y in ink:
        for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, -1), (-1, 1), (1, 1)):
            q = (x + dx, y + dy)
            if q not in ink and 0 <= q[0] < SIZE and 0 <= q[1] < SIZE:
                px[q] = EDGE
    for p in ink:
        px[p] = INK
    return px


def squeeze(px, width, mirror=False):
    """The finished icon seen at an angle: columns sampled down to `width`, centred."""
    xs = [x for x, _ in px]
    x0, span = min(xs), max(xs) - min(xs) + 1
    nx0 = (SIZE - width) // 2
    out = {}
    for nx in range(width):
        sx = x0 + (nx * span + span // 2) // width
        for (x, y), c in px.items():
            if x == sx:
                out[(nx0 + (width - 1 - nx if mirror else nx), y)] = c
    return out


def encode(px, w=SIZE, h=SIZE, pitch=320):
    """Frame in the short-header form: {u32 size, w, h, x, y, nruns}, runs."""
    lin = sorted(y * pitch + x for (x, y) in px)
    runs, pos, i = bytearray(), 0, 0
    n = 0
    while i < len(lin):
        j = i
        while j + 1 < len(lin) and lin[j + 1] == lin[j] + 1:
            j += 1
        k = j - i + 1
        runs += struct.pack("<HBB", lin[i] - pos, k // 4, k % 4)
        runs += bytes(px[(p % pitch, p // pitch)] for p in range(lin[i], lin[j] + 1))
        pos = lin[j] + 1
        n += 1
        i = j + 1
    body = struct.pack("<HHHHH", w, h, 0, 0, n) + runs
    return struct.pack("<I", len(body) - 8) + body


def build(game, out):
    src = (Path(game) / "SPECIAL.ICO").read_bytes()
    frames = spr.frames(src)
    assert len(frames) == 4 * len(LABELS), len(frames)
    data = bytearray(src[:0x80])
    for icon, lines in enumerate(LABELS):
        full = outlined(label_bits(lines))
        narrow = 0
        for k in range(4):
            a, b, f = frames[4 * icon + k]
            xs = [x for x, _ in f.pixels()]
            w = max(xs) - min(xs) + 1
            if w <= 2:  # edge-on: the original frame is just a line
                data += src[a:b]
            elif w >= 10:
                data += encode(full)
            else:  # the original's narrow frames, the second one mirrored
                data += encode(squeeze(full, w, mirror=narrow > 0))
                narrow += 1
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "SPECIAL.ICO").write_bytes(bytes(data))
    # Round trip: the decoder must read back what was written.
    back = spr.frames(bytes(data))
    assert len(back) == len(frames)
    return len(back)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "build":
        print(build(sys.argv[2], sys.argv[4]), "frames")
    else:
        sys.exit(__doc__)
