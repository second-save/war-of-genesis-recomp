#!/usr/bin/env python3
"""Build lang/<code>/STRINGS.TXT from translations/<code>/strings.tsv.

Each line of the output is "<Johab text><TAB><replacement>"; the native text
hooks replace any drawn string that matches a key exactly (names, labels).

  build TRANS_DIR OUT
"""
import sys
from pathlib import Path

if len(sys.argv) != 4 or sys.argv[1] != "build":
    sys.exit(__doc__)
trans, out = Path(sys.argv[2]), Path(sys.argv[3])
data = bytearray()
n = 0
for line in (trans / "strings.tsv").read_text().splitlines():
    if not line.strip() or line.startswith("#"):
        continue
    ko, en = line.split("\t")[:2]
    data += ko.encode("johab") + b"\t" + en.encode("ascii") + b"\n"
    n += 1
out.mkdir(parents=True, exist_ok=True)
(out / "STRINGS.TXT").write_bytes(bytes(data))
print(f"{n} strings")
