#!/usr/bin/env python3
"""Static recompiler: 16-bit real-mode x86 (Borland C++ MZ) -> C++.

Pipeline
--------
1. Parse the MZ executable (image + relocation table).
2. Discover functions by recursive descent from the entry point, every
   direct near/far call target, hint-file entries and (speculatively) far code
   pointers found in relocated data.
3. For each function, collect the instructions reachable through
   intra-procedural edges.  Jumps into *other* function starts become tail
   calls.  Code shared by several functions is simply duplicated.
4. Emit one C++ function per x86 function.  Each function operates on the
   emulated CPU state (`gen::Cpu`) and flat memory owned by `gen::Machine`.
   Calls push a return address on the emulated stack (so `[bp+N]` argument
   accesses stay correct) and then call the native function directly.

Everything the generated code needs lives in `src/runtime/recomp.h`.

Usage
-----
    python3 tools/lifter/lift16.py GAME/MAIN.EXE --name main --out src/modules/generated/main
"""
from __future__ import annotations

import argparse
import collections
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_16, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mz import MzExe  # noqa: E402

DEFAULT_LOAD_SEG = 0x0810  # PSP lives at LOAD_SEG - 0x10

CTYPE = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}
STYPE = {1: "int8_t", 2: "int16_t", 4: "int32_t"}
BITS = {1: "8", 2: "16", 4: "32"}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}

REG = {}
for _n in "abcd":
    REG[f"{_n}l"] = (f"c.{_n}x.b.l", 1)
    REG[f"{_n}h"] = (f"c.{_n}x.b.h", 1)
    REG[f"{_n}x"] = (f"c.{_n}x.w", 2)
    REG[f"e{_n}x"] = (f"c.{_n}x.e", 4)
for _n in ("si", "di", "bp", "sp"):
    REG[_n] = (f"c.{_n}.w", 2)
    REG[f"e{_n}"] = (f"c.{_n}.e", 4)
for _n in ("cs", "ds", "es", "ss", "fs", "gs"):
    REG[_n] = (f"c.{_n}", 2)
for _n in range(5):
    REG[f"cr{_n}"] = (f"c.cr{_n}", 4)
for _n in range(8):
    REG[f"dr{_n}"] = (f"c.dr{_n}", 4)

SEG_PREFIX = {0x2E: "c.cs", 0x36: "c.ss", 0x3E: "c.ds", 0x26: "c.es", 0x64: "c.fs", 0x65: "c.gs"}

COND = {
    "jo": "c.of", "jno": "!c.of", "jb": "c.cf", "jae": "!c.cf", "je": "c.zf", "jne": "!c.zf",
    "jbe": "(c.cf || c.zf)", "ja": "(!c.cf && !c.zf)", "js": "c.sf", "jns": "!c.sf",
    "jp": "c.pf", "jnp": "!c.pf", "jl": "(c.sf != c.of)", "jge": "(c.sf == c.of)",
    "jle": "(c.zf || c.sf != c.of)", "jg": "(!c.zf && c.sf == c.of)",
}
SETCC = {"set" + k[1:]: v for k, v in COND.items()}

ALU2 = {"add": "op_add", "adc": "op_adc", "sub": "op_sub", "sbb": "op_sbb",
        "and": "op_and", "or": "op_or", "xor": "op_xor"}
SHIFTS = {"shl": "op_shl", "sal": "op_shl", "shr": "op_shr", "sar": "op_sar",
          "rol": "op_rol", "ror": "op_ror", "rcl": "op_rcl", "rcr": "op_rcr"}
STRING_OPS = {"movs", "stos", "lods", "cmps", "scas", "ins", "outs"}
TERMINATORS = {"ret", "retf", "iret", "ljmp", "jmp"}


def fname(seg: int, off: int) -> str:
    return f"F_{seg:04X}_{off:04X}"


@dataclass
class Function:
    seg: int
    off: int
    speculative: bool = False
    body: dict[int, object] = field(default_factory=dict)  # off -> insn
    labels: set[int] = field(default_factory=set)
    tables: dict[int, list[int] | None] = field(default_factory=dict)  # jmp insn off -> targets

    @property
    def linear(self) -> int:
        return self.seg * 16 + self.off


class LiftError(Exception):
    pass


class Lifter:
    def __init__(self, exe: MzExe, name: str, load_seg: int, hints: dict):
        self.exe = exe
        self.name = name
        self.load_seg = load_seg
        self.hints = hints
        self.md = Cs(CS_ARCH_X86, CS_MODE_16)
        self.md.detail = True
        self.relocs = exe.reloc_offsets
        self._cache: dict[tuple[int, int], object] = {}
        self.functions: dict[int, Function] = {}
        self.unimpl = collections.Counter()
        self.warnings: list[str] = []
        self.code_segs: set[int] = set()

    # ------------------------------------------------------------------ decode
    def decode(self, seg: int, off: int):
        key = (seg, off)
        if key in self._cache:
            return self._cache[key]
        io = seg * 16 + off
        insn = None
        if 0 <= io < len(self.exe.image):
            code = self.exe.image[io:io + 16]
            insn = next(self.md.disasm(code, off, count=1), None)
            if insn is not None:
                insn = self._fixup_fpu_emu(seg, insn)
        self._cache[key] = insn
        return insn

    def _fixup_fpu_emu(self, seg, insn):
        """Borland/Microsoft FPU emulator: `int 34h..3Bh` == opcode D8..DF + modrm.

        We record the real FPU instruction so the length is right and the
        runtime can emulate it.  `int 3Ch` carries a segment override, `int 3Dh`
        is FWAIT (2 bytes, no operand).
        """
        if insn.mnemonic != "int" or not insn.operands:
            return insn
        n = insn.operands[0].imm
        if not 0x34 <= n <= 0x3C:
            return insn
        io = seg * 16 + insn.address
        if n == 0x3C:
            b = self.exe.image[io + 2]
            real = bytes([0xD8 | (b & 7)]) + self.exe.image[io + 3:io + 16]
            extra = 1
        else:
            real = bytes([0xD8 + (n - 0x34)]) + self.exe.image[io + 2:io + 16]
            extra = 0
        fpu = next(Cs(CS_ARCH_X86, CS_MODE_16).disasm(real, 0, count=1), None)
        if fpu is None:
            return insn
        # Wrap: keep the original capstone object, attach metadata.
        return _FpuEmu(insn, fpu, length=2 + extra + fpu.size - 1, int_no=n)

    # ------------------------------------------------------------- discovery
    def add_function(self, seg, off, speculative=False):
        lin = seg * 16 + off
        if lin in self.functions:
            if not speculative:
                self.functions[lin].speculative = False
            return False
        if not 0 <= lin < len(self.exe.image):
            return False
        self.functions[lin] = Function(seg, off, speculative)
        self.code_segs.add(seg)
        return True

    def is_reloc_word(self, seg, insn, pos_in_insn) -> bool:
        return (seg * 16 + insn.address + pos_in_insn) in self.relocs

    def successors(self, fn: Function, seg: int, insn, preds: dict):
        """Return (intra_targets, call_targets(seg,off), falls_through)."""
        m = base_mnemonic(insn)
        intra, calls = [], []
        nxt = (insn.address + insn.size) & 0xFFFF
        if isinstance(insn, _FpuEmu):
            return intra, calls, True
        ops = insn.operands
        if m in COND or m in ("jcxz", "jecxz", "loop", "loope", "loopne"):
            intra.append(ops[0].imm & 0xFFFF)
            return intra, calls, True
        if m == "jmp":
            if ops[0].type == X86_OP_IMM:
                intra.append(ops[0].imm & 0xFFFF)
            else:
                targets = self.resolve_table(fn, seg, insn, preds)
                fn.tables[insn.address] = targets
                if targets:
                    intra.extend(targets)
            return intra, calls, False
        if m == "ljmp":
            if len(ops) == 2 and ops[0].type == X86_OP_IMM and self.is_reloc_word(seg, insn, 3):
                calls.append((ops[0].imm, ops[1].imm))
            return intra, calls, False
        if m == "call":
            if ops[0].type == X86_OP_IMM:
                calls.append((seg, ops[0].imm & 0xFFFF))
            return intra, calls, True
        if m == "lcall":
            if len(ops) == 2 and ops[0].type == X86_OP_IMM and self.is_reloc_word(seg, insn, 3):
                calls.append((ops[0].imm, ops[1].imm))
            return intra, calls, True
        if m in ("ret", "retf", "iret", "iretd"):
            return intra, calls, False
        return intra, calls, True

    def resolve_table(self, fn, seg, insn, preds):
        """Resolve `jmp word ptr cs:[reg + table]` Borland switch tables."""
        op = insn.operands[0]
        if op.type != X86_OP_MEM or insn.prefix[1] != 0x2E or op.size != 2 or op.mem.index:
            return None
        base = insn.reg_name(op.mem.base) if op.mem.base else None
        table = op.mem.disp & 0xFFFF
        # Pattern A: cmp REG, N / ja default / shl REG,1 (or add REG,REG) / jmp cs:[REG+table]
        cur, count = insn.address, None
        for _ in range(8):
            cur = preds.get(cur)
            if cur is None:
                break
            p = self.decode(seg, cur)
            if p is None or isinstance(p, _FpuEmu):
                break
            if base_mnemonic(p) == "cmp" and len(p.operands) == 2 and p.operands[1].type == X86_OP_IMM \
                    and p.operands[0].type == X86_OP_REG and p.reg_name(p.operands[0].reg) == base:
                count = (p.operands[1].imm & 0xFFFF) + 1
                break
            # The index register was loaded with a constant: this is not an index-bounded
            # table (e.g. a search table, pattern B), so an earlier cmp doesn't apply.
            if base_mnemonic(p) == "mov" and len(p.operands) == 2 and p.operands[0].type == X86_OP_REG \
                    and p.reg_name(p.operands[0].reg) == base and p.operands[1].type == X86_OP_IMM:
                break
        if count is None or count > 512:
            # Pattern B (search table): mov cx, N / mov bx, VALUES / scan loop / jmp cs:[bx + 2N].
            # The N case values at VALUES are followed by the N targets. Walk back from the jmp
            # to the loads that actually feed it.
            start = n = None
            cur = insn.address
            for _ in range(24):
                cur = preds.get(cur)
                if cur is None:
                    break
                p = self.decode(seg, cur)
                if p is None or isinstance(p, _FpuEmu):
                    break
                if base_mnemonic(p) == "mov" and len(p.operands) == 2 and p.operands[0].type == X86_OP_REG \
                        and p.operands[1].type == X86_OP_IMM:
                    r = p.reg_name(p.operands[0].reg)
                    if r == base and start is None:
                        start = p.operands[1].imm & 0xFFFF
                    elif r == "cx" and n is None:
                        n = p.operands[1].imm & 0xFFFF
                if start is not None and n is not None:
                    break
            if start is not None and n is not None and n * 2 == table and 0 < n <= 256:
                io = seg * 16 + start + table
                return [self.exe.word(io + 2 * k) for k in range(n)]
            self.warnings.append(f"{fname(fn.seg, fn.off)}: unresolved indirect jmp at {seg:04X}:{insn.address:04X} ({insn.op_str})")
            return None
        io = seg * 16 + table
        return [self.exe.word(io + 2 * k) for k in range(count)]

    def explore(self, fn: Function, stop_at_functions: bool):
        """BFS over intra-procedural edges.  Returns call targets found."""
        seg = fn.seg
        work = collections.deque([fn.off])
        seen: set[int] = set()
        preds: dict[int, int] = {}
        calls = []
        fn.body.clear()
        fn.labels.clear()
        fn.tables.clear()
        while work:
            off = work.popleft()
            if off in seen:
                continue
            if stop_at_functions and off != fn.off and (seg * 16 + off) in self.functions:
                continue
            seen.add(off)
            insn = self.decode(seg, off)
            fn.body[off] = insn
            if insn is None:
                continue
            intra, c, falls = self.successors(fn, seg, insn, preds)
            calls.extend(c)
            if falls:
                nxt = (off + insn.size) & 0xFFFF
                preds.setdefault(nxt, off)
                work.append(nxt)
            for t in intra:
                fn.labels.add(t)
                preds.setdefault(t, off)
                work.append(t)
        return calls

    def discover(self):
        e = self.exe
        self.add_function(e.init_cs, e.init_ip)
        for h in self.hints.get("functions", []):
            s, o = (int(x, 16) for x in h.split(":"))
            self.add_function(s, o)
        self.discover_borland_tables()
        changed = True
        while changed:
            changed = False
            for fn in list(self.functions.values()):
                if fn.body:
                    continue
                for (s, o) in self.explore(fn, stop_at_functions=False):
                    changed |= self.add_function(s, o)
                changed = True
            if not changed:
                changed = self.add_speculative_far_pointers()
        # Final pass: build bodies, splitting at function boundaries.
        for fn in self.functions.values():
            self.explore(fn, stop_at_functions=True)
        # Drop speculative functions that decode to garbage.
        for lin, fn in list(self.functions.items()):
            if fn.speculative and any(i is None for i in fn.body.values()):
                del self.functions[lin]

    def add_speculative_near_pointers(self) -> bool:
        """Near pointers in data tables pointing into known code segments."""
        added = False
        code_lin = {fn.linear for fn in self.functions.values()}
        # Collect candidate 16-bit words from data regions once
        data_words = set()
        for i in range(0, len(self.exe.image) - 1):
            if not self._inside_code(i):
                data_words.add(self.exe.word(i))

        for seg in self.code_segs:
            for off in data_words:
                lin = seg * 16 + off
                if lin in code_lin or not (0 <= lin < len(self.exe.image)):
                    continue
                insn = self.decode(seg, off)
                if insn is None or isinstance(insn, _FpuEmu) or insn.mnemonic == "add":
                    continue
                prev_ok = (off == 0)
                if not prev_ok:
                    for back in range(1, 16):
                        if off - back < 0:
                            break
                        pi = self.decode(seg, off - back)
                        if pi and base_mnemonic(pi) in ("ret", "retf", "jmp", "iret"):
                            pad = self.exe.image[seg * 16 + pi.address + pi.size : seg * 16 + off]
                            if all(b in (0, 0x90, 0xCC) for b in pad):
                                prev_ok = True
                                break
                if prev_ok and base_mnemonic(insn) in ("push", "enter", "mov", "sub", "xor", "cli", "cld"):
                    added |= self.add_function(seg, off, speculative=True)
        return added

    def discover_borland_tables(self):
        """Discover functions from Borland C++ RTL __InitStart__ / __ExitStart__ tables."""
        dgroup = None
        for i in list(self.md.disasm(self.exe.image[:64], 0)):
            if i.mnemonic == 'mov' and i.op_str.startswith('dx, 0x'):
                try:
                    dgroup = int(i.op_str.split('0x')[1], 16)
                    break
                except ValueError:
                    pass
        if dgroup is None:
            return
        insns = list(self.md.disasm(self.exe.image[:0x300], 0))
        for idx, i in enumerate(insns):
            if i.mnemonic == 'mov' and 'si' in i.op_str and idx + 2 < len(insns):
                i2 = insns[idx + 1]
                i3 = insns[idx + 2]
                if i2.mnemonic == 'mov' and 'di' in i2.op_str and i3.mnemonic == 'call':
                    try:
                        si_val = int(i.op_str.split(', ')[1], 16)
                        di_val = int(i2.op_str.split(', ')[1], 16)
                    except ValueError:
                        continue
                    if si_val < di_val and (di_val - si_val) % 6 == 0:
                        t_start = dgroup * 16 + si_val
                        for ent in range(t_start, dgroup * 16 + di_val, 6):
                            chunk = (self.exe.image[ent:] + b'\x00' * 6)[:6]
                            ctype = chunk[0]
                            w1 = int.from_bytes(chunk[2:4], 'little')
                            w2 = int.from_bytes(chunk[4:6], 'little')
                            if ctype == 0:
                                self.add_function(0, w1)
                            elif ctype == 1:
                                self.add_function(w2, w1)

    def add_speculative_far_pointers(self) -> bool:
        """Far pointers in data: relocated word preceded by an offset."""
        added = False
        code_lin = {fn.linear for fn in self.functions.values()}
        for r in sorted(self.relocs):
            seg = self.exe.word(r)
            if seg not in self.code_segs or r < 2:
                continue
            # Skip relocations that live inside instructions of known functions.
            off = self.exe.word(r - 2)
            lin = seg * 16 + off
            if lin in code_lin or self._inside_code(r):
                continue
            insn = self.decode(seg, off)
            if insn is None or isinstance(insn, _FpuEmu):
                continue
            if base_mnemonic(insn) in ("push", "enter", "mov", "cli", "pusha", "pushaw", "jmp",
                                       "ret", "retf", "iret", "xor", "sub", "cld", "nop"):
                added |= self.add_function(seg, off, speculative=True)
        return added

    def _inside_code(self, image_off: int) -> bool:
        if not hasattr(self, "_code_bytes"):
            self._code_bytes = set()
        if not self._code_bytes or getattr(self, "_code_bytes_n", 0) != len(self.functions):
            cb = set()
            for fn in self.functions.values():
                for o, i in fn.body.items():
                    if i is not None:
                        base = fn.seg * 16 + o
                        cb.update(range(base, base + i.size))
            self._code_bytes, self._code_bytes_n = cb, len(self.functions)
        return image_off in self._code_bytes

    # ---------------------------------------------------------------- emit
    def emit_all(self, out_dir: Path):
        out_dir.mkdir(parents=True, exist_ok=True)
        mod = self.name
        by_seg = collections.defaultdict(list)
        for fn in sorted(self.functions.values(), key=lambda f: (f.seg, f.off)):
            by_seg[fn.seg].append(fn)

        decl = [f"// Generated by tools/lifter/lift16.py from {self.exe.path.name}. DO NOT EDIT.",
                "#pragma once", '#include "runtime/recomp.h"', f"namespace gen::mod_{mod} {{",
                f"constexpr uint16_t kLoadSeg = 0x{self.load_seg:04X};"]
        decl += [f"void {fname(f.seg, f.off)}(Machine& m);" for f in sorted(self.functions.values(), key=lambda f: (f.seg, f.off))]
        decl.append("}  // namespace")
        (out_dir / f"{mod}_decl.h").write_text("\n".join(decl) + "\n")

        files = []
        for seg, fns in sorted(by_seg.items()):
            lines = [f"// Generated by tools/lifter/lift16.py from {self.exe.path.name}, segment {seg:04X}. DO NOT EDIT.",
                     f'#include "{mod}_decl.h"', f"namespace gen::mod_{mod} {{", ""]
            for fn in fns:
                lines += self.emit_function(fn)
            lines.append("}  // namespace")
            fn_path = out_dir / f"{mod}_seg{seg:04X}.cpp"
            fn_path.write_text("\n".join(lines) + "\n")
            files.append(fn_path.name)

        tab = [f"// Generated function table for {self.exe.path.name}. DO NOT EDIT.",
               f'#include "{mod}_decl.h"', '#include "runtime/module.h"', "",
               f"namespace gen::mod_{mod} {{", "static const LiftedFunction kFunctions[] = {"]
        for f in sorted(self.functions.values(), key=lambda f: (f.seg, f.off)):
            tab.append(f"    {{0x{f.seg:04X}, 0x{f.off:04X}, &{fname(f.seg, f.off)}}},")
        tab += ["};", "}  // namespace", "",
                "namespace gen {",
                f"extern const LiftedModule kLifted_{mod};",
                f"const LiftedModule kLifted_{mod} = {{",
                f'    "{mod}", "{self.exe.path.name}", mod_{mod}::kLoadSeg,',
                f"    mod_{mod}::kFunctions, sizeof(mod_{mod}::kFunctions) / sizeof(mod_{mod}::kFunctions[0]),",
                f"    0x{self.exe.init_cs:04X}, 0x{self.exe.init_ip:04X},",
                "};", "}  // namespace gen"]
        (out_dir / f"{mod}_table.cpp").write_text("\n".join(tab) + "\n")
        files.append(f"{mod}_table.cpp")

        (out_dir / "CMakeLists.txt").write_text(
            "# Generated by tools/lifter/lift16.py. DO NOT EDIT.\n"
            f"add_library(lifted_{mod} STATIC\n" + "".join(f"    {f}\n" for f in files) + ")\n"
            f"target_link_libraries(lifted_{mod} PRIVATE genesis_runtime)\n"
            f"genesis_configure_generated(lifted_{mod})\n")
        return files

    def emit_function(self, fn: Function) -> list[str]:
        seg = fn.seg
        offs = sorted(fn.body)
        jumped = set(t for t in fn.labels if t in fn.body)
        # Pass 1: generate statements for every instruction.
        blocks: dict[int, list[str]] = {}
        for idx, off in enumerate(offs):
            insn = fn.body[off]
            if insn is None:
                blocks[off] = [f"m.invalid_code(0x{seg:04X}, 0x{off:04X}); return;"]
                continue
            self.check_relocs(seg, insn)
            try:
                stmts, falls = self.emit_insn(fn, seg, insn)
            except LiftError as ex:
                self.unimpl[str(ex)] += 1
                stmts, falls = [f'm.unimplemented(0x{seg:04X}, 0x{off:04X}, "{insn_text(insn)}"); return;'], False
            stmts = [f"// {off:04X}: {insn_text(insn)}"] + stmts
            # Hint-supplied runtime hooks (e.g. present a frame after a VRAM blit).
            inject = self.hints.get("inject_after", {}).get(f"{seg:04X}:{off:04X}")
            if inject:
                stmts.append(inject)
            if falls:
                nxt = (off + insn.size) & 0xFFFF
                following = offs[idx + 1] if idx + 1 < len(offs) else None
                if nxt != following:
                    if nxt in fn.body:
                        stmts.append(f"goto L_{nxt:04X};")
                        jumped.add(nxt)
                    else:
                        target_fn = self.functions.get(seg * 16 + nxt)
                        if target_fn:
                            stmts.append(f"{fname(target_fn.seg, target_fn.off)}(m); return;  // falls into next function")
                        else:
                            stmts.append(f"m.call_near(c.cs, 0x{nxt:04X}); return;  // falls into next function")
            blocks[off] = stmts
        # Indirect-jump switches may have registered extra labels.
        jumped |= {t for t in fn.labels if t in fn.body}
        # Pass 2: render.
        out = [f"void {fname(seg, fn.off)}(Machine& m) {{", "    Cpu& c = m.cpu;", "    (void)c;"]
        if fn.off != offs[0]:
            out.append(f"    goto L_{fn.off:04X};")
            jumped.add(fn.off)
        for off in offs:
            if off in jumped:
                out.append(f"L_{off:04X}:")
            out += ["    " + s for s in blocks[off]]
        out += ["}", ""]
        return out

    def check_relocs(self, seg, insn):
        """Warn about relocated words inside an instruction that we don't model."""
        if isinstance(insn, _FpuEmu):
            return
        base = seg * 16 + insn.address
        for pos in range(insn.size - 1):
            if base + pos not in self.relocs:
                continue
            m = base_mnemonic(insn)
            if m in ("lcall", "ljmp") and pos == 3:
                continue
            if pos == insn.size - 2 and insn.imm_offset == pos:
                continue
            if pos == insn.size - 4 and insn.imm_offset == pos and insn.imm_size == 4:
                continue
            self.warnings.append(f"unmodelled relocation in {seg:04X}:{insn.address:04X} '{insn_text(insn)}' (byte {pos})")

    # ------------------------------------------------------------- operands
    def seg_of(self, insn, op) -> str:
        p = insn.prefix[1]
        if p:
            return SEG_PREFIX[p]
        if op.mem.base and insn.reg_name(op.mem.base) in ("bp", "ebp", "esp"):
            return "c.ss"
        return "c.ds"

    def ea(self, insn, op, extra: int = 0) -> str:
        mem = op.mem
        parts = []
        if mem.base:
            parts.append(REG[insn.reg_name(mem.base)][0])
        if mem.index:
            r = REG[insn.reg_name(mem.index)][0]
            parts.append(f"{r} * {mem.scale}" if mem.scale > 1 else r)
        disp = mem.disp + extra
        if disp or not parts:
            parts.append(f"({disp})" if disp < 0 else f"0x{disp:X}")
        expr = " + ".join(parts)
        a32 = insn.prefix[3] == 0x67
        return f"(uint32_t)({expr})" if a32 else f"(uint16_t)({expr})"

    def imm(self, seg, insn, op, size) -> str:
        v = op.imm & MASK[size]
        if size == 2 and self.is_reloc_word(seg, insn, insn.size - 2) and insn.imm_offset == insn.size - 2:
            return f"(uint16_t)(0x{v:X} + kLoadSeg)"
        if size == 4 and self.is_reloc_word(seg, insn, insn.imm_offset) and insn.imm_offset == insn.size - 4:
            # Segment value in the low word of a 32-bit immediate.
            return f"(uint32_t)(0x{v:X}u + kLoadSeg)"
        return f"({CTYPE[size]})0x{v:X}u"

    def rd(self, seg, insn, op, size=None) -> str:
        size = size or op.size
        if op.type == X86_OP_REG:
            return REG[insn.reg_name(op.reg)][0]
        if op.type == X86_OP_IMM:
            return self.imm(seg, insn, op, size)
        if op.type == X86_OP_MEM:
            return f"rd{BITS[size]}(m, {self.seg_of(insn, op)}, {self.ea(insn, op)})"
        raise LiftError(f"operand type {op.type}")

    def wr(self, insn, op, val: str, size=None) -> str:
        size = size or op.size
        if op.type == X86_OP_REG:
            return f"{REG[insn.reg_name(op.reg)][0]} = ({CTYPE[size]})({val});"
        if op.type == X86_OP_MEM:
            return f"wr{BITS[size]}(m, {self.seg_of(insn, op)}, {self.ea(insn, op)}, ({CTYPE[size]})({val}));"
        raise LiftError(f"write to operand type {op.type}")

    def jump(self, fn: Function, seg: int, target: int, insn) -> str:
        poll = "m.poll_tick(); " if target <= insn.address else ""
        if target in fn.body:
            return f"{{ {poll}goto L_{target:04X}; }}"
        target_fn = self.functions.get(seg * 16 + target)
        if target_fn:
            return f"{{ {poll}{fname(target_fn.seg, target_fn.off)}(m); return; }}"
        return f"{{ {poll}m.call_near(c.cs, 0x{target:04X}); return; }}"

    # --------------------------------------------------------- instructions
    def emit_insn(self, fn: Function, seg: int, insn):
        if isinstance(insn, _FpuEmu):
            return [f'm.fpu_emulated(0x{insn.int_no:02X}, 0x{seg:04X}, 0x{insn.address:04X}, "{insn.fpu.mnemonic} {insn.fpu.op_str}");'], True
        m = base_mnemonic(insn)
        ops = insn.operands
        nxt = (insn.address + insn.size) & 0xFFFF
        o0 = ops[0] if ops else None
        o1 = ops[1] if len(ops) > 1 else None
        S = o0.size if o0 is not None and o0.type != X86_OP_IMM else 2
        T = CTYPE.get(S, "uint16_t")
        rd = lambda op, size=None: self.rd(seg, insn, op, size)  # noqa: E731
        wr = lambda op, v, size=None: self.wr(insn, op, v, size)  # noqa: E731
        poll_back = lambda t: "m.poll_tick(); " if t <= insn.address else ""  # noqa: E731

        if m == "nop" or m == "wait" or m == "fwait":
            return [], True
        if m == "mov":
            if o0.type == X86_OP_REG and insn.reg_name(o0.reg) in ("cs",):
                raise LiftError("mov cs")
            return [wr(o0, rd(o1, o0.size))], True
        if m in ALU2:
            return [f"{{ {T} t = {ALU2[m]}<{T}>(c, {rd(o0)}, {rd(o1, S)}); {wr(o0, 't')} }}"], True
        if m == "cmp":
            return [f"op_sub<{T}>(c, {rd(o0)}, {rd(o1, S)});"], True
        if m == "test":
            return [f"op_and<{T}>(c, {rd(o0)}, {rd(o1, S)});"], True
        if m in ("inc", "dec", "neg"):
            return [f"{{ {T} t = op_{m}<{T}>(c, {rd(o0)}); {wr(o0, 't')} }}"], True
        if m == "not":
            return [wr(o0, f"~{rd(o0)}")], True
        if m in SHIFTS:
            cnt = "1" if o1 is None else (rd(o1, 1) if o1.type != X86_OP_IMM else str(o1.imm & 0xFF))
            return [f"{{ {T} t = {SHIFTS[m]}<{T}>(c, {rd(o0)}, {cnt}); {wr(o0, 't')} }}"], True
        if m in ("shld", "shrd"):
            o2 = ops[2]
            cnt = rd(o2, 1) if o2.type != X86_OP_IMM else str(o2.imm & 0xFF)
            return [f"{{ {T} t = op_{m}<{T}>(c, {rd(o0)}, {rd(o1)}, {cnt}); {wr(o0, 't')} }}"], True
        if m in ("movzx", "movsx"):
            src = rd(o1)
            if m == "movsx":
                src = f"({CTYPE[o0.size]})({STYPE[o0.size]})({STYPE[o1.size]})({src})"
            return [wr(o0, src)], True
        if m == "lea":
            return [wr(o0, self.ea(insn, o1))], True
        if m in ("les", "lds", "lss", "lfs", "lgs"):
            sreg = "c." + m[1:]
            return [f"{{ {T} o = rd{BITS[S]}(m, {self.seg_of(insn, o1)}, {self.ea(insn, o1)}); "
                    f"uint16_t s = rd16(m, {self.seg_of(insn, o1)}, {self.ea(insn, o1, S)}); {wr(o0, 'o')} {sreg} = s; }}"], True
        if m == "xchg":
            return [f"{{ {T} a = {rd(o0)}; {T} b = {rd(o1)}; {wr(o0, 'b')} {wr(o1, 'a')} }}"], True
        if m == "push":
            if o0.type == X86_OP_IMM:
                size = 4 if insn.prefix[2] == 0x66 else 2
                return [f"push{BITS[size]}(m, {self.imm(seg, insn, o0, size)});"], True
            return [f"push{BITS[o0.size]}(m, {rd(o0)});"], True
        if m == "pop":
            return [f"{{ {T} t = pop{BITS[S]}(m); {wr(o0, 't')} }}"], True
        if m in ("pushf", "pushfd"):
            return [f"push{'32' if m == 'pushfd' else '16'}(m, c.flags16());"], True
        if m in ("popf", "popfd"):
            return [f"c.set_flags16((uint16_t)pop{'32' if m == 'popfd' else '16'}(m));"], True
        if m in ("pusha", "popa", "pushal", "popal", "pushaw", "popaw"):
            op = "pusha" if m in ("pusha", "pushaw") else ("popa" if m in ("popa", "popaw") else m)
            return [f"{op}(m);"], True
        if m == "enter":
            if (ops[1].imm & 0xFF) != 0:
                raise LiftError("enter level>0")
            return [f"push16(m, c.bp.w); c.bp.w = c.sp.w; c.sp.w = (uint16_t)(c.sp.w - 0x{ops[0].imm & 0xFFFF:X});"], True
        if m == "leave":
            return ["c.sp.w = c.bp.w; c.bp.w = pop16(m);"], True
        if m in ("cbw", "cwde"):
            if insn.prefix[2] == 0x66:
                return ["c.ax.e = (uint32_t)(int32_t)(int16_t)c.ax.w;"], True
            return ["c.ax.w = (uint16_t)(int16_t)(int8_t)c.ax.b.l;"], True
        if m in ("cwd", "cdq"):
            if insn.prefix[2] == 0x66:
                return ["c.dx.e = (c.ax.e & 0x80000000u) ? 0xFFFFFFFFu : 0;"], True
            return ["c.dx.w = (c.ax.w & 0x8000) ? 0xFFFF : 0;"], True
        if m in ("mul", "imul", "div", "idiv") and len(ops) == 1:
            return [f"op_{m}{BITS[S]}(m, {rd(o0)});"], True
        if m == "imul":
            a, b = (rd(o0), rd(o1)) if len(ops) == 2 else (rd(o1), rd(ops[2], S))
            return [f"{{ {T} t = op_imul2<{T}>(c, {a}, {b}); {wr(o0, 't')} }}"], True
        if m in ("bt", "bts", "btr", "btc"):
            cnt = rd(o1, S) if o1.type != X86_OP_IMM else str(o1.imm & 0xFF)
            if m == "bt":
                return [f"op_bt<{T}>(c, {rd(o0)}, {cnt});"], True
            return [f"{{ {T} t = op_{m}<{T}>(c, {rd(o0)}, {cnt}); {wr(o0, 't')} }}"], True
        if m in ("bsf", "bsr"):
            return [f"{{ {T} t = {rd(o0)}; if (op_{m}<{T}>(c, {rd(o1)}, t)) {{ {wr(o0, 't')} }} }}"], True
        if m in SETCC:
            return [wr(o0, f"{SETCC[m]} ? 1 : 0")], True
        if m in ("clc", "stc", "cmc", "cld", "std", "cli", "sti"):
            return [{"clc": "c.cf = 0;", "stc": "c.cf = 1;", "cmc": "c.cf ^= 1;", "cld": "c.df = 0;",
                     "std": "c.df = 1;", "cli": "c.ifl = 0;", "sti": "c.ifl = 1; m.poll_tick();"}[m]], True
        if m == "lahf":
            return ["c.ax.b.h = (uint8_t)c.flags16();"], True
        if m == "sahf":
            return ["c.set_flags16((uint16_t)((c.flags16() & 0xFF00) | c.ax.b.h));"], True
        if m in ("daa", "das", "aaa", "aas"):
            return [f"op_{m}(c);"], True
        if m in ("aam", "aad"):
            base = ops[0].imm & 0xFF if ops else 10
            return [f"op_{m}(m, {base});"], True
        if m == "xlatb":
            sg = SEG_PREFIX.get(insn.prefix[1], "c.ds")
            return [f"c.ax.b.l = rd8(m, {sg}, (uint16_t)(c.bx.w + c.ax.b.l));"], True
        if m == "in":
            port = rd(o1) if o1.type == X86_OP_REG else f"0x{o1.imm & 0xFF:X}"
            return [f"{REG[insn.reg_name(o0.reg)][0]} = m.in{BITS[o0.size]}({port});"], True
        if m == "out":
            port = rd(o0) if o0.type == X86_OP_REG else f"0x{o0.imm & 0xFF:X}"
            return [f"m.out{BITS[o1.size]}({port}, {rd(o1)});"], True
        if m == "int":
            return [f"m.interrupt(0x{o0.imm & 0xFF:X}, 0x{seg:04X}, 0x{insn.address:04X});"], True
        if m == "int3":
            return [f"m.interrupt(3, 0x{seg:04X}, 0x{insn.address:04X});"], True
        if m == "hlt":
            return ["m.idle();"], True
        if m in STRING_OPS or m[:-1] in STRING_OPS:
            return [self.string_op(insn, m)], True
        if m in ("lgdt", "lidt", "lldt", "ltr"):
            return [f"// {m} ignored in flat real-mode runtime"], True
        if m.startswith("f"):
            return [f'm.fpu_emulated(0, 0x{seg:04X}, 0x{insn.address:04X}, "{insn.mnemonic} {insn.op_str}");'], True

        # ---- control flow
        if m in COND:
            return [f"if ({COND[m]}) {self.jump(fn, seg, o0.imm & 0xFFFF, insn)}"], True
        if m in ("jcxz", "jecxz"):
            r = "c.cx.e" if m == "jecxz" else "c.cx.w"
            return [f"if ({r} == 0) {self.jump(fn, seg, o0.imm & 0xFFFF, insn)}"], True
        if m in ("loop", "loope", "loopne"):
            r = "c.cx.e" if insn.prefix[3] == 0x67 else "c.cx.w"
            cond = {"loop": "", "loope": " && c.zf", "loopne": " && !c.zf"}[m]
            return [f"if (--{r} != 0{cond}) {self.jump(fn, seg, o0.imm & 0xFFFF, insn)}"], True
        if m == "jmp":
            if o0.type == X86_OP_IMM:
                return [self.jump(fn, seg, o0.imm & 0xFFFF, insn)], False
            first_insn = fn.body.get(fn.off)
            if first_insn and first_insn.mnemonic == "pop" and len(first_insn.operands) > 0 and o0.type == X86_OP_MEM:
                f0 = first_insn.operands[0]
                if f0.type == X86_OP_MEM and f0.mem.disp == o0.mem.disp and f0.mem.base == o0.mem.base:
                    return ["return;  // indirect return via saved return address"], False
            targets = fn.tables.get(insn.address)
            cases = targets if targets else sorted(fn.labels | {fn.off})
            sw = " ".join(f"case 0x{t:04X}: goto L_{t:04X};" for t in sorted(set(cases)) if t in fn.body)
            for t in sorted(set(cases)):
                fn.labels.add(t) if t in fn.body else None
            return [f"{{ uint16_t t = {rd(o0, 2)}; m.poll_tick(); switch (t) {{ {sw} default: m.call_near(c.cs, t); return; }} }}"], False
        if m == "ljmp":
            if len(ops) == 2 and o0.type == X86_OP_IMM:
                s, o = o0.imm, o1.imm
                if self.is_reloc_word(seg, insn, 3):
                    target_fn = self.functions.get(s * 16 + o)
                    if target_fn:
                        return [f"c.cs = (uint16_t)(0x{s:X} + kLoadSeg); {fname(target_fn.seg, target_fn.off)}(m); return;"], False
                    return [f"c.cs = (uint16_t)(0x{s:X} + kLoadSeg); m.call_far(c.cs, 0x{o:X}); return;"], False
                return [f"c.cs = 0x{s:X}; m.call_far(0x{s:X}, 0x{o:X}); return;"], False
            sg, ea = self.seg_of(insn, o0), self.ea(insn, o0)
            return [f"{{ uint16_t o = rd16(m, {sg}, {ea}); uint16_t s = rd16(m, {sg}, {self.ea(insn, o0, 2)}); c.cs = s; m.call_far(s, o); return; }}"], False
        if m == "call":
            if o0.type == X86_OP_IMM:
                t = o0.imm & 0xFFFF
                target_fn = self.functions.get(seg * 16 + t)
                if target_fn:
                    return [f"push16(m, 0x{nxt:04X}); {fname(target_fn.seg, target_fn.off)}(m);"], True
                return [f"push16(m, 0x{nxt:04X}); m.call_near(c.cs, 0x{t:04X});"], True
            return [f"{{ uint16_t t = {rd(o0, 2)}; push16(m, 0x{nxt:04X}); m.call_near(c.cs, t); }}"], True
        if m == "lcall":
            if len(ops) == 2 and o0.type == X86_OP_IMM:
                s, o = o0.imm, o1.imm
                if self.is_reloc_word(seg, insn, 3):
                    target_fn = self.functions.get(s * 16 + o)
                    if target_fn:
                        return [f"push16(m, c.cs); push16(m, 0x{nxt:04X}); c.cs = (uint16_t)(0x{s:X} + kLoadSeg); {fname(target_fn.seg, target_fn.off)}(m);"], True
                    return [f"push16(m, c.cs); push16(m, 0x{nxt:04X}); c.cs = (uint16_t)(0x{s:X} + kLoadSeg); m.call_far(c.cs, 0x{o:X});"], True
                return [f"push16(m, c.cs); push16(m, 0x{nxt:04X}); c.cs = 0x{s:X}; m.call_far(0x{s:X}, 0x{o:X});"], True
            sg = self.seg_of(insn, o0)
            return [f"{{ uint16_t o = rd16(m, {sg}, {self.ea(insn, o0)}); uint16_t s = rd16(m, {sg}, {self.ea(insn, o0, 2)}); "
                    f"push16(m, c.cs); push16(m, 0x{nxt:04X}); c.cs = s; m.call_far(s, o); }}"], True
        if m == "ret":
            extra = f" c.sp.w = (uint16_t)(c.sp.w + 0x{o0.imm:X});" if ops else ""
            return [f"(void)pop16(m);{extra} return;"], False
        if m == "retf":
            extra = f" c.sp.w = (uint16_t)(c.sp.w + 0x{o0.imm:X});" if ops else ""
            return [f"(void)pop16(m); c.cs = pop16(m);{extra} return;"], False
        if m == "iret":
            return ["(void)pop16(m); c.cs = pop16(m); c.set_flags16(pop16(m)); return;"], False
        raise LiftError(m)

    def string_op(self, insn, m):
        kind = m[:-1] if m[:-1] in STRING_OPS else m
        suffix = m[-1]
        size = {"b": 1, "w": 2, "d": 4}[suffix]
        rep = {0xF3: 1, 0xF2: 2}.get(insn.prefix[0], 0)
        if not rep:
            # Capstone drops F2 in front of movsd (it reads it as the SSE2 MOVSD prefix).
            for b in insn.bytes:
                if b in (0xF2, 0xF3):
                    rep = 1 if b == 0xF3 else 2
                    break
                if b not in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0):
                    break
        a32 = "true" if insn.prefix[3] == 0x67 else "false"
        sseg = SEG_PREFIX.get(insn.prefix[1], "c.ds")
        if kind in ("stos", "scas", "ins"):
            return f"str_{kind}<{CTYPE[size]}>(m, {rep}, {a32});"
        return f"str_{kind}<{CTYPE[size]}>(m, {sseg}, {rep}, {a32});"


class _FpuEmu:
    """Wrapper for a Borland FPU-emulator interrupt plus its real FPU decoding."""

    def __init__(self, insn, fpu, length, int_no):
        self.insn, self.fpu, self.size, self.int_no = insn, fpu, length, int_no
        self.address = insn.address
        self.mnemonic = "fpu_emu"
        self.op_str = f"int {int_no:#x} => {fpu.mnemonic} {fpu.op_str}"
        self.operands = []
        self.prefix = [0, 0, 0, 0]


def base_mnemonic(insn) -> str:
    m = insn.mnemonic
    for p in ("rep ", "repe ", "repne ", "repz ", "repnz ", "lock ", "bnd "):
        if m.startswith(p):
            m = m[len(p):]
    return m


def insn_text(insn) -> str:
    return f"{insn.mnemonic} {insn.op_str}".strip().replace("\\", "\\\\").replace('"', "'")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", type=Path)
    ap.add_argument("--name", required=True, help="module identifier, e.g. main / chp / pack / chrsys")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--load-seg", type=lambda s: int(s, 0), default=DEFAULT_LOAD_SEG)
    ap.add_argument("--hints", type=Path, help="JSON file: {\"functions\": [\"SEG:OFF\", ...], "
                    "\"inject_after\": {\"SEG:OFF\": \"C++ statement\"}}")
    ap.add_argument("--stats-only", action="store_true")
    args = ap.parse_args()

    hints = json.loads(args.hints.read_text()) if args.hints and args.hints.exists() else {}
    exe = MzExe.load(args.exe)
    lifter = Lifter(exe, args.name, args.load_seg, hints)
    lifter.discover()
    n_insn = sum(len(f.body) for f in lifter.functions.values())
    spec = sum(1 for f in lifter.functions.values() if f.speculative)
    if not args.stats_only:
        files = lifter.emit_all(args.out)
        print(f"wrote {len(files)} files to {args.out}")
    print(f"{exe.path.name}: {len(lifter.functions)} functions ({spec} speculative), {n_insn} instructions, "
          f"{len(lifter.code_segs)} code segments")
    for w in lifter.warnings:
        print("warning:", w)
    if lifter.unimpl:
        print("unimplemented:", dict(lifter.unimpl.most_common()))
    report = {
        "exe": exe.path.name, "functions": len(lifter.functions), "speculative": spec,
        "instructions": n_insn, "unimplemented": dict(lifter.unimpl), "warnings": lifter.warnings,
    }
    if not args.stats_only:
        (args.out / "lift_report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
