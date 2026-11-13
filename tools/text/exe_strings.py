#!/usr/bin/env python3
"""List the Korean string literals in the DOS programs' data segments
(menus, prompts, labels). Output: unique strings, one per line, to stdout.

  exe_strings.py GAME_DIR
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "lifter"))
from mz import MzExe  # noqa: E402


def dgroup(img: bytes) -> int:
    # Borland startup: mov dx, DGROUP (BA lo hi) in the first bytes
    i = img.find(b"\xba", 0, 16)
    return int.from_bytes(img[i + 1:i + 3], "little") * 16


def strings(path: Path):
    img = MzExe.load(path).image
    data = img[dgroup(img):]
    for m in re.finditer(rb"(?<=\x00)[\x20-\x7e\x84-\xd3\x41-\xfe]{2,80}\x00", data):
        raw = m.group()[:-1]
        try:
            t = raw.decode("johab")
        except UnicodeDecodeError:
            continue
        hangul = sum("가" <= ch <= "힣" for ch in t)
        if hangul and hangul * 2 >= len(raw) * 0.5:
            yield t


if __name__ == "__main__":
    game = Path(sys.argv[1])
    seen = []
    for exe in ("MAIN.EXE", "CHP.EXE", "PACK.EXE", "CHRSYS.EXE"):
        for t in strings(game / exe):
            if t not in seen:
                seen.append(t)
                print(f"{exe}\t{t}")
