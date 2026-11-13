#!/usr/bin/env python3
"""Batch static recompiler runner for all War of Genesis executables."""
import subprocess
import sys
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent.parent.parent
GAME_DIR = BASE_DIR / "The-War-of-Genesis_DOS_KO"
OUT_BASE = BASE_DIR / "src" / "modules" / "generated"

MODULES = [
    ("MAIN.EXE", "main", 0x0810),
    ("CHP.EXE", "chp", 0x0810),
    ("PACK.EXE", "pack", 0x0810),
    ("CHRSYS.EXE", "chrsys", 0x0810),
]

def main():
    lifter_py = BASE_DIR / "tools" / "lifter" / "lift16.py"

    for exe_name, mod_name, load_seg in MODULES:
        exe_path = GAME_DIR / exe_name
        out_dir = OUT_BASE / mod_name
        print(f"============================================================")
        print(f" Lifting {exe_name} -> {out_dir}")
        print(f"============================================================")

        cmd = [
            sys.executable,
            str(lifter_py),
            str(exe_path),
            "--name", mod_name,
            "--out", str(out_dir),
            "--load-seg", hex(load_seg)
        ]
        hint_file = BASE_DIR / "tools" / "lifter" / "hints" / f"{mod_name}.json"
        if hint_file.exists():
            cmd.extend(["--hints", str(hint_file)])

        ret = subprocess.run(cmd)
        if ret.returncode != 0:
            print(f"[ERR] Failed to lift {exe_name} (code {ret.returncode})")
            sys.exit(ret.returncode)

    # TTL.EXE: 32-bit DOS/4GW title program.
    cmd = [sys.executable, str(BASE_DIR / "tools" / "lifter" / "lift_le.py"), str(GAME_DIR / "TTL.EXE"),
           "--name", "ttl", "--out", str(OUT_BASE / "ttl")]
    hint_file = BASE_DIR / "tools" / "lifter" / "hints" / "ttl.json"
    if hint_file.exists():
        cmd.extend(["--hints", str(hint_file)])
    print("============================================================")
    print(f" Lifting TTL.EXE -> {OUT_BASE / 'ttl'}")
    print("============================================================")
    ret = subprocess.run(cmd)
    if ret.returncode != 0:
        sys.exit(ret.returncode)

    print("\nAll modules successfully recompiled to C++!")

if __name__ == "__main__":
    main()
