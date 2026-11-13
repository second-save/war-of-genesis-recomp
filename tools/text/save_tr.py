#!/usr/bin/env python3
"""Write translations/en/CHAPDATA/VISUAL.<n>.json from one English line per entry (stdin)."""
import json, sys
n = sys.argv[1]
ko = json.load(open(f"work/ko/CHAPDATA/VISUAL.{n}.json"))
en = [l.rstrip("\n") for l in sys.stdin if l.strip()]
if len(en) != len(ko):
    sys.exit(f"VISUAL.{n}: {len(en)} lines for {len(ko)} entries")
out = [{"code": k["code"], "hash": k["hash"], "en": t} for k, t in zip(ko, en)]
open(f"translations/en/CHAPDATA/VISUAL.{n}.json", "w").write(json.dumps(out, ensure_ascii=False, indent=1) + "\n")
print(f"VISUAL.{n}: {len(en)} entries")
