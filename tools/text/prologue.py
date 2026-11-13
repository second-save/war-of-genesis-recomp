#!/usr/bin/env python3
"""Chapter prologue pages (CHAPDATA/PE<n>): one null-terminated Johab string.

  extract GAME_DIR WORK_DIR         -> WORK_DIR/ko/CHAPDATA/PE.json (Korean, local only)
  build   GAME_DIR TRANS_DIR OUT    -> OUT/CHAPDATA/PE<n> from TRANS_DIR/CHAPDATA/PE.json

CHP reads the whole file, so the English may be longer than the original.
"""
import hashlib, json, sys
from pathlib import Path


def text_of(p: Path) -> str:
    return p.read_bytes().split(b"\0")[0].decode("johab")


def h(s: str) -> str:
    return hashlib.sha1(s.encode("utf-8")).hexdigest()[:10]


def pages(game: Path):
    return sorted((game / "CHAPDATA").glob("PE[0-9]*"), key=lambda p: int(p.name[2:]))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "extract" and len(sys.argv) == 4:
        game, work = Path(sys.argv[2]), Path(sys.argv[3])
        out = [{"file": p.name, "ko": text_of(p), "hash": h(text_of(p))} for p in pages(game)]
        dst = work / "ko" / "CHAPDATA" / "PE.json"
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(json.dumps(out, ensure_ascii=False, indent=1) + "\n")
        print(f"{len(out)} pages")
    elif cmd == "build" and len(sys.argv) == 5:
        game, trans, outdir = Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
        tr = {t["file"]: t for t in json.loads((trans / "CHAPDATA" / "PE.json").read_text())}
        for p in pages(game):
            t = tr.get(p.name)
            if not t or not t.get("en"):
                continue
            if t.get("hash") and t["hash"] != h(text_of(p)):
                print(f"{p.name}: source changed since translation")
            dst = outdir / "CHAPDATA" / p.name
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(t["en"].encode("ascii") + b"\0")
        print(f"{sum(1 for t in tr.values() if t.get('en'))}/{len(pages(game))} pages translated")
    else:
        sys.exit(__doc__)
