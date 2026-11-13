#!/usr/bin/env python3
"""Merge 'index<TAB>English' lines (stdin) into translations/en/SCENARIO/SCE.json,
where index refers to work/ko/SCENARIO/SCE.json."""
import json, sys
from pathlib import Path
ko = json.load(open("work/ko/SCENARIO/SCE.json"))
dst = Path("translations/en/SCENARIO/SCE.json")
cur = {t["key"]: t for t in json.loads(dst.read_text())} if dst.exists() else {}
n = 0
for line in sys.stdin:
    if not line.strip():
        continue
    i, en = line.rstrip("\n").split("\t", 1)
    k = ko[int(i)]["key"]
    cur[k] = {"key": k, "en": en}
    n += 1
order = {e["key"]: i for i, e in enumerate(ko)}
out = sorted(cur.values(), key=lambda t: order.get(t["key"], 1 << 30))
dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_text(json.dumps(out, ensure_ascii=False, indent=1) + "\n")
print(f"{n} saved, {len(out)}/{len(ko)} total")
