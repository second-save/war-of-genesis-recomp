#!/usr/bin/env python3
"""Item descriptions (CSTLDATA/ITEMEXAM.DAT): rebuild in English.

Format: one 150-byte record per item (ITEM.DAT order), three 50-byte lines of
Johab text, each null-terminated. The Korean lines are at most ~30 bytes
(240 px); an English line of that width fits in 50 bytes, so the English is
stored directly (word-wrapped with the game's ASCII font metrics), no markers.

  build GAME_DIR TRANS_DIR OUT   -> OUT/CSTLDATA/ITEMEXAM.DAT
"""
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from talk import Metrics  # noqa: E402

LINE_PX = 236
REC, LINE = 150, 50


def build(game: Path, trans: Path, out_dir: Path):
    src = game / "CSTLDATA" / "ITEMEXAM.DAT"
    data = bytearray(src.read_bytes())
    met = Metrics((game / "NEWASCII.FNT").read_bytes())
    tr = json.loads((trans / "CSTLDATA" / "ITEMEXAM.json").read_text())
    stale = long_ = 0
    for t in tr:
        i = t["item"]
        ko = "\n".join(bytes(data[i * REC + k * LINE:i * REC + (k + 1) * LINE]).split(b"\0")[0].decode("johab")
                       for k in range(3))
        if t.get("hash") and t["hash"] != hashlib.sha1(ko.encode()).hexdigest()[:10]:
            stale += 1
        lines = met.wrap(t["en"], LINE_PX)
        if len(lines) > 3 or any(len(l) >= LINE for l in lines):
            long_ += 1
            print(f"  item {i}: too long ({len(lines)} lines): {t['en']}")
            lines = lines[:3]
        lines += [""] * (3 - len(lines))
        for k, l in enumerate(lines):
            b = l.encode("ascii")[:LINE - 1]
            data[i * REC + k * LINE:i * REC + (k + 1) * LINE] = b + b"\0" * (LINE - len(b))
    dst = out_dir / "CSTLDATA" / "ITEMEXAM.DAT"
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(bytes(data))
    print(f"{len(tr)} item descriptions" + (f", {stale} stale" if stale else "") + (f", {long_} too long" if long_ else ""))


if __name__ == "__main__":
    if len(sys.argv) != 5 or sys.argv[1] != "build":
        sys.exit(__doc__)
    build(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
