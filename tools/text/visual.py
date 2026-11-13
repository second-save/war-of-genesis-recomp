#!/usr/bin/env python3
"""Story dialogue scripts (CHAPDATA/VISUAL.n): extract and rebuild.

Format (Johab text): a sequence of entries `XY<text>` separated by '#', where XY is a
code: U/D + letter = portrait at the top/bottom (which portrait), or a single
letter such as M (narration). The
text is usually "Name - line". `#E` followed by 0x1A ends the script; the rest of
the file is zero padding.

  extract  GAME_DIR WORK_DIR        -> WORK_DIR/ko/CHAPDATA/VISUAL.n.json (Korean, local only)
  build    GAME_DIR TRANS_DIR OUT   -> OUT/CHAPDATA/VISUAL.n for each translated file

Translation files (TRANS_DIR/CHAPDATA/VISUAL.n.json) hold, per entry, the code,
the English text and a hash of the Korean it was made from. English text may use
"\\n" for a line break and "\\f" for a page break; the native typesetter wraps
words and paginates on its own.
"""
import hashlib
import json
import sys
from pathlib import Path


def parse(data: bytes):
    end = data.find(b"#E\x1a")
    body = data[:end] if end >= 0 else data.rstrip(b"\0")
    entries = []
    chunks = body.split(b"#")
    if not chunks[0]:  # files normally start without a '#'
        chunks = chunks[1:]
    for chunk in chunks:
        n = 2 if len(chunk) > 1 and 0x41 <= chunk[1] <= 0x5A else 1  # e.g. "UJ", or "M" + Hangul
        code = chunk[:n].decode("ascii", "replace")
        text = chunk[n:].decode("johab")
        entries.append({"code": code, "ko": text})
    return entries, end >= 0


def src_hash(ko: str) -> str:
    return hashlib.sha1(ko.encode("utf-8")).hexdigest()[:10]


def extract(game: Path, work: Path):
    for p in sorted((game / "CHAPDATA").glob("VISUAL.*"), key=lambda p: int(p.suffix[1:])):
        entries, _ = parse(p.read_bytes())
        for e in entries:
            e["hash"] = src_hash(e["ko"])
        out = work / "ko" / "CHAPDATA" / (p.name + ".json")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(entries, ensure_ascii=False, indent=1) + "\n")
        print(f"{p.name}: {len(entries)} entries")


def encode(text: str) -> bytes:
    out = bytearray()
    for ch in text:
        if ord(ch) < 0x80:
            out += ch.encode("ascii")
        else:
            out += ch.encode("johab")  # names or terms left in Korean
    return bytes(out)


def build(game: Path, trans: Path, out_dir: Path):
    for tp in sorted((trans / "CHAPDATA").glob("VISUAL.*.json")):
        name = tp.name[:-5]
        src = game / "CHAPDATA" / name
        entries, _ = parse(src.read_bytes())
        tr = json.loads(tp.read_text())
        if len(tr) != len(entries):
            sys.exit(f"{tp}: {len(tr)} entries, game file has {len(entries)}")
        data = bytearray()
        stale = 0
        for i, (e, t) in enumerate(zip(entries, tr)):
            if t.get("hash") and t["hash"] != src_hash(e["ko"]):
                stale += 1
            text = t.get("en") or e["ko"]  # untranslated entries stay Korean
            data += (b"#" if i else b"") + e["code"].encode("ascii") + encode(text)
        data += b"#E\x1a"
        out = out_dir / "CHAPDATA" / name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(bytes(data))
        done = sum(1 for t in tr if t.get("en"))
        print(f"{name}: {done}/{len(tr)} translated" + (f", {stale} stale" if stale else ""))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "extract" and len(sys.argv) == 4:
        extract(Path(sys.argv[2]), Path(sys.argv[3]))
    elif cmd == "build" and len(sys.argv) == 5:
        build(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
    else:
        sys.exit(__doc__)
