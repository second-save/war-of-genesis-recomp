#!/usr/bin/env python3
"""Package a built translation overlay for release without game data.

The overlay (lang/<L>/) holds rebuilt game files. A release ships only:
  TEXT.TXT, STRINGS.TXT  (translation text)
  FILES.PAT              (each rebuilt file as copy/insert ops against the
                          player's original file: only the new bytes are stored)
The runtime rebuilds the files from the player's game on first launch.

  make_patch.py GAME_DIR LANG_DIR OUT_DIR

FILES.PAT, little-endian: "GPAT" u32 version(1) u32 nfiles, then per file:
  u16 path_len, path ('/'-separated, relative to the game folder),
  u32 src_size, u32 src_crc32, u32 dst_size, u32 dst_crc32, u32 nops,
  ops: u8 0 + u32 offset + u32 length (copy from the original) |
       u8 1 + u32 length + bytes (insert)
"""
import difflib
import shutil
import struct
import sys
import zlib
from pathlib import Path

MIN_COPY = 8  # shorter matches are cheaper as inserts


def same_size_ops(a: bytes, b: bytes):
    """Files rebuilt in place (same length): copy the unchanged runs."""
    ops, pending, i = [], bytearray(), 0
    while i < len(b):
        j = i
        while j < len(b) and a[j] == b[j]:
            j += 1
        if j - i >= MIN_COPY:
            if pending:
                ops.append((1, bytes(pending)))
                pending = bytearray()
            ops.append((0, i, j - i))
            i = j
            continue
        pending += b[i:max(j, i + 1)]
        i = max(j, i + 1)
    if pending:
        ops.append((1, bytes(pending)))
    return ops


def ops_for(a: bytes, b: bytes):
    if len(a) == len(b):
        return same_size_ops(a, b)
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    ops, pending = [], bytearray()
    pos = 0
    for blk in sm.get_matching_blocks():
        if blk.b > pos:
            pending += b[pos:blk.b]
        if blk.size >= MIN_COPY:
            if pending:
                ops.append((1, bytes(pending)))
                pending = bytearray()
            ops.append((0, blk.a, blk.size))
        else:
            pending += b[blk.b:blk.b + blk.size]
        pos = blk.b + blk.size
    if pending:
        ops.append((1, bytes(pending)))
    return ops


def apply(a: bytes, ops) -> bytes:
    out = bytearray()
    for op in ops:
        out += a[op[1]:op[1] + op[2]] if op[0] == 0 else op[1]
    return bytes(out)


def main(game: Path, lang: Path, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    entries, stored, total = [], 0, 0
    for f in sorted(p for p in lang.rglob("*") if p.is_file()):
        rel = f.relative_to(lang).as_posix()
        src = game / rel
        if not src.exists():
            if rel in ("TEXT.TXT", "STRINGS.TXT"):
                shutil.copyfile(f, out / rel)
            continue
        a, b = src.read_bytes(), f.read_bytes()
        ops = ops_for(a, b)
        assert apply(a, ops) == b, rel
        rec = bytearray(struct.pack("<H", len(rel)) + rel.encode())
        rec += struct.pack("<IIIII", len(a), zlib.crc32(a), len(b), zlib.crc32(b), len(ops))
        for op in ops:
            if op[0] == 0:
                rec += struct.pack("<BII", 0, op[1], op[2])
            else:
                rec += struct.pack("<BI", 1, len(op[1])) + op[1]
                stored += len(op[1])
        total += len(b)
        entries.append(bytes(rec))
    data = b"GPAT" + struct.pack("<II", 1, len(entries)) + b"".join(entries)
    (out / "FILES.PAT").write_bytes(data)
    print(f"{len(entries)} files, {total} bytes rebuilt from {stored} new bytes; FILES.PAT {len(data)} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
