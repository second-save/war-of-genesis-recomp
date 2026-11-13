#!/usr/bin/env python3
"""Battle scenarios (SCENARIO/*.SCE): extract and rebuild their text.

Dialogue messages are records  00 03 00 <speaker> <u16 length> <text>  where the
text is Johab lines ending in '\\n' followed by a 0 byte; the length counts that
0. CHRSYS shows a message three lines per page. The mission briefing is a
null-terminated string near the end of the file (" << title >>\\n\\n...").

Record lengths chain the file together, so the rebuilt files keep every byte
count: each non-empty Korean line becomes an "@@XYZ" marker (padded with
spaces), and the English lines go to TEXT_SCE.TXT. English is wrapped to the
box width; it may take more lines than the Korean (more pages) when the record
has room for the extra markers.

  extract GAME_DIR WORK_DIR        -> WORK_DIR/ko/SCENARIO/SCE.json (unique texts, local only)
  build   GAME_DIR TRANS_DIR OUT   -> OUT/SCENARIO/*.SCE + OUT/TEXT_SCE.TXT
"""
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from talk import Metrics, b36  # noqa: E402

LINE_PX = 190   # battle message box (11 Hangul syllables)
BRIEF_PX = 200  # mission briefing box
FIRST_ID = 35000  # ids below are town talk


def messages(d: bytes):
    """[(start, text_len_without_0, text)] for dialogue records."""
    out = []
    for m in re.finditer(rb"\x00\x03\x00(.)(..)", d, re.S):
        n = struct.unpack("<H", m.group(2))[0]
        s = m.end()
        t = d[s:s + n]
        if 2 < n < 600 and t.endswith(b"\n\x00"):
            try:
                out.append((s, n - 1, t[:-1].decode("johab")))
            except UnicodeDecodeError:
                pass
    return out


def briefing(d: bytes):
    """(start, length, text) of the mission briefing, or None."""
    i = d.rfind(b" << ")
    if i < 0:
        i = d.rfind(b"<< ")
    if i < 0:
        return None
    j = d.find(b"\0", i)
    try:
        return i, j - i, d[i:j].decode("johab")
    except UnicodeDecodeError:
        return None


def key(text: str) -> str:
    return hashlib.sha1(text.encode("utf-8")).hexdigest()[:12]


def sce_files(game: Path):
    return sorted((game / "SCENARIO").glob("*.SCE"))


def extract(game: Path, work: Path):
    uniq = {}
    for f in sce_files(game):
        d = f.read_bytes()
        items = [("msg", t) for _, _, t in messages(d)]
        b = briefing(d)
        if b:
            items.append(("briefing", b[2]))
        for kind, t in items:
            e = uniq.setdefault(key(t), {"key": key(t), "kind": kind, "ko": t, "where": []})
            e["where"].append(f.stem)
    out = work / "ko" / "SCENARIO" / "SCE.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(list(uniq.values()), ensure_ascii=False, indent=1) + "\n")
    print(f"{len(uniq)} unique texts")


def build(game: Path, trans: Path, out_dir: Path):
    tr = {t["key"]: t["en"] for t in json.loads((trans / "SCENARIO" / "SCE.json").read_text()) if t.get("en")}
    met = Metrics((game / "NEWASCII.FNT").read_bytes())
    ids = {}        # (key, line index) -> id
    lines_out = {}  # id -> text
    overflow = 0

    def new_id(k, i, text):
        if (k, i) not in ids:
            ids[(k, i)] = FIRST_ID + len(ids)
            lines_out[ids[(k, i)]] = text
        return ids[(k, i)]

    for f in sce_files(game):
        data = bytearray(f.read_bytes())
        changed = False
        for start, n, ko in messages(bytes(data)):
            en = tr.get(key(ko))
            if not en:
                continue
            en = en.replace("\\n", "\n")
            ko_lines = ko.split("\n")[:-1]          # text ends with '\n'
            slots = [i for i, l in enumerate(ko_lines) if l.strip()]
            wrapped = met.wrap(en, LINE_PX)
            if len(slots) == len(ko_lines) and len(wrapped) > len(slots) and 6 * len(wrapped) <= n:
                # No deliberate blank lines: English may take more lines (the game
                # pages every three), as long as the markers fit in the record.
                ko_lines = [""] * len(wrapped)
                slots = list(range(len(wrapped)))
            if len(wrapped) > len(slots):
                overflow += 1
                print(f"  {f.stem}: needs {len(wrapped)} lines, has {len(slots)}: {en[:60]}...")
                wrapped = wrapped[:len(slots)]
            wrapped += [""] * (len(slots) - len(wrapped))
            k = key(ko)
            direct = "\n".join(wrapped) + "\n"
            if len(slots) == len(ko_lines) and len(direct) <= n:
                # Short enough to store as is: no marker needed.
                last = direct.rstrip("\n")
                body = last + " " * (n - len(direct)) + direct[len(last):]
                data[start:start + n] = body.encode("ascii")
                changed = True
                continue
            per_line = [""] * len(ko_lines)
            for slot, line in zip(slots, wrapped):
                if line:
                    per_line[slot] = "@@" + b36(new_id(k, slot, line))
            body = "\n".join(per_line) + "\n"
            if len(body) > n:  # not enough room for a marker per line: one marker, all lines
                body = "@@" + b36(new_id(k, -1, "\\n".join(w for w in wrapped if w))) + "\n" * len(ko_lines)
            if len(body) > n:  # a tiny record ("!!!"): keep what fits of the English
                body = (en.replace("\n", " ")[:n - 1]) + "\n"
                print(f"  {f.stem}: {n}-byte record, English cut to '{body.strip()}'")
            last = body.rstrip("\n")
            body = last + " " * (n - len(body)) + body[len(last):]  # pad inside the last line
            data[start:start + n] = body.encode("ascii")
            changed = True
        b = briefing(bytes(data))
        if b and tr.get(key(b[2])):
            start, n, ko = b
            # Title, blank line, then the objective wrapped to the briefing box.
            paras = tr[key(ko)].replace("\\n", "\n").split("\n")
            out = [paras[0], ""] + [w for p in paras[1:] if p.strip() for w in met.wrap(p, BRIEF_PX)]
            mark = "@@" + b36(new_id(key(ko), -1, "\\n".join(out)))
            data[start:start + n] = (mark + "\0" * (n - len(mark))).encode("ascii")
            changed = True
        if changed:
            dst = out_dir / "SCENARIO" / f.name
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(bytes(data))
    assert FIRST_ID + len(ids) < 36 ** 3
    (out_dir / "TEXT_SCE.TXT").write_text("".join(f"{i}\t{t}\n" for i, t in sorted(lines_out.items())))
    print(f"{len(tr)} texts translated, {len(lines_out)} lines" + (f", {overflow} too long" if overflow else ""))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "extract" and len(sys.argv) == 4:
        extract(Path(sys.argv[2]), Path(sys.argv[3]))
    elif cmd == "build" and len(sys.argv) == 5:
        build(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
    else:
        sys.exit(__doc__)
