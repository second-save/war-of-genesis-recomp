#!/usr/bin/env python3
"""Town talk (CSTLDATA/*.TAK): extract and rebuild.

Format: 8-byte header, then 30-byte records {u16 message id, u8 0xFF, 27 bytes
Johab text (null-terminated)}. Consecutive records with the same id are the lines
of one message (up to 13 syllables each).

  extract GAME_DIR WORK_DIR        -> WORK_DIR/ko/CSTLDATA/TAK.json  (unique messages, local only)
  build   GAME_DIR TRANS_DIR OUT   -> OUT/CSTLDATA/*.TAK + OUT/TEXT.TXT

English doesn't fit 26-byte records, so the rebuilt files hold "@@<id>" markers
and the lines go to TEXT.TXT, which the native PACK text hook reads. Each English
message is word-wrapped with the game's own ASCII font metrics (NEWASCII.FNT,
proportional as drawn by the hook) into at most as many lines as the Korean had.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

LINE_PX = 200  # box holds 13 Hangul syllables (208 px); keep a margin


def records(data: bytes):
    n = (len(data) - 8) // 30
    for i in range(n):
        r = data[8 + i * 30: 8 + i * 30 + 30]
        yield i, struct.unpack_from("<H", r, 0)[0], r[3:].split(b"\0")[0]


def messages(data: bytes):
    """[(id, [record indices], [Korean lines])] for non-empty messages."""
    out = []
    for i, rid, t in records(data):
        if rid == 0 and not t:
            continue
        if out and out[-1][0] == rid and out[-1][1][-1] == i - 1:
            out[-1][1].append(i)
            out[-1][2].append(t.decode("johab"))
        else:
            out.append((rid, [i], [t.decode("johab")]))
    return out


def key(lines):
    return hashlib.sha1("\n".join(lines).encode("utf-8")).hexdigest()[:12]


def tak_files(game: Path):
    return sorted((game / "CSTLDATA").glob("*.TAK"))


class Metrics:
    def __init__(self, font: bytes):
        self.adv = {}
        for c in range(32, 127):
            bits = 0
            for r in range(16):
                bits |= font[c * 16 + r]
            cols = [k for k in range(8) if bits & (0x80 >> k)]
            self.adv[chr(c)] = 4 if (c == 32 or not cols) else cols[-1] - cols[0] + 2

    def width(self, s: str) -> int:
        return sum(self.adv.get(ch, 8) for ch in s)

    def wrap(self, text: str, limit: int):
        lines = []
        for para in text.split("\n"):
            cur = ""
            for word in para.split():
                trial = word if not cur else cur + " " + word
                if self.width(trial) <= limit or not cur:
                    cur = trial
                else:
                    lines.append(cur)
                    cur = word
            lines.append(cur)
        return lines


def extract(game: Path, work: Path):
    uniq = {}
    for f in tak_files(game):
        for rid, idx, lines in messages(f.read_bytes()):
            k = key(lines)
            e = uniq.setdefault(k, {"key": k, "ko": lines, "where": []})
            e["where"].append(f"{f.name}#{rid}")
    out = work / "ko" / "CSTLDATA" / "TAK.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(list(uniq.values()), ensure_ascii=False, indent=1) + "\n")
    print(f"{len(uniq)} unique messages")


def build(game: Path, trans: Path, out_dir: Path):
    tr = {t["key"]: t["en"] for t in json.loads((trans / "CSTLDATA" / "TAK.json").read_text()) if t.get("en")}
    met = Metrics((game / "NEWASCII.FNT").read_bytes())
    text_lines = []
    overflow = 0
    for fno, f in enumerate(tak_files(game)):
        data = bytearray(f.read_bytes())
        for rid, idx, lines in messages(bytes(data)):
            en = tr.get(key(lines))
            if not en:
                continue
            wrapped = met.wrap(en, LINE_PX)
            if len(wrapped) > len(idx):
                overflow += 1
                print(f"  {f.name}#{rid}: needs {len(wrapped)} lines, has {len(idx)}: {en[:60]}...")
                wrapped = wrapped[:len(idx)]
            wrapped += [""] * (len(idx) - len(wrapped))
            for rec, line in zip(idx, wrapped):
                gid = (fno + 1) * 1000 + rec
                mark = f"@@{gid}".encode("ascii") if line else b""
                field = mark + b"\0" * (27 - len(mark))
                data[8 + rec * 30 + 3: 8 + rec * 30 + 30] = field
                if line:
                    text_lines.append(f"{gid}\t{line}")
        dst = out_dir / "CSTLDATA" / f.name
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(bytes(data))
    (out_dir / "TEXT.TXT").write_text("\n".join(text_lines) + "\n")
    print(f"{len(tr)} messages translated, {len(text_lines)} lines" + (f", {overflow} too long" if overflow else ""))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "extract" and len(sys.argv) == 4:
        extract(Path(sys.argv[2]), Path(sys.argv[3]))
    elif cmd == "build" and len(sys.argv) == 5:
        build(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
    else:
        sys.exit(__doc__)
