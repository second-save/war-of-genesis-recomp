#!/usr/bin/env python3
"""Rough census of Johab (KSSM) text in the game files: null-terminated runs that
decode as Johab and contain at least one Hangul syllable."""
import re, sys, collections
from pathlib import Path

def runs(data, minlen=4):
    for m in re.finditer(rb'[\x20-\x7e\x84-\xd3\x41-\xfe]{%d,}' % minlen, data):
        s = m.group()
        try:
            t = s.decode('johab')
        except UnicodeDecodeError:
            continue
        if any('가' <= ch <= '힣' for ch in t):
            yield m.start(), t

root = Path(sys.argv[1])
by_ext = collections.Counter(); chars = collections.Counter(); uniq = set()
for p in sorted(root.rglob('*')):
    if not p.is_file(): continue
    d = p.read_bytes()
    for off, t in runs(d):
        ext = p.suffix.upper() or p.name
        by_ext[ext] += 1; chars[ext] += len(t); uniq.add(t.strip())
for ext, n in by_ext.most_common():
    print(f"{ext:8} {n:7} strings {chars[ext]:9} chars")
print("unique strings:", len(uniq), " total hangul syllables:", sum(sum('가'<=c<='힣' for c in u) for u in uniq))
