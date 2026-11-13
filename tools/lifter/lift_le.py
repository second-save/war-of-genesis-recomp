#!/usr/bin/env python3
"""Static recompiler: 32-bit protected-mode x86 (Watcom C, DOS/4GW LE) -> C++.

Same model as lift16.py, adapted to a flat 32-bit program:
  * functions are keyed by linear address (objects stay at their preferred
    bases, see le.py) and named P_<addr>;
  * memory goes through the selector-aware rd/wr helpers (Machine runs in
    protected-mode mode while the program runs, so `ptr(sel, off)` adds the
    descriptor base);
  * near calls push a 32-bit return address; indirect calls/jumps that leave
    the function go through Machine::call32.
  * jump tables are found through the LE fixups (`jmp [reg*4 + table]`).

Usage
-----
    python3 tools/lifter/lift_le.py GAME/TTL.EXE --name ttl --out src/modules/generated/ttl
"""
from __future__ import annotations

import argparse
import collections
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

sys.path.insert(0, str(Path(__file__).resolve().parent))
from le import LeExe  # noqa: E402
from lift16 import (ALU2, BITS, COND, CTYPE, MASK, REG, SEG_PREFIX, SETCC, SHIFTS,  # noqa: E402
                    STRING_OPS, STYPE, LiftError, base_mnemonic, insn_text)

SEGREGS = {"cs", "ds", "es", "ss", "fs", "gs"}


def fname(addr: int) -> str:
    return f"P_{addr:08X}"


@dataclass
class Function:
    addr: int
    speculative: bool = False
    body: dict[int, object] = field(default_factory=dict)
    labels: set[int] = field(default_factory=set)
    tables: dict[int, list[int] | None] = field(default_factory=dict)


class Lifter32:
    def __init__(self, exe: LeExe, name: str, hints: dict):
        self.exe = exe
        self.name = name
        self.hints = hints
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self._cache = {}
        self.functions: dict[int, Function] = {}
        self.unimpl = collections.Counter()
        self.warnings: list[str] = []
        self.code = [o for o in exe.objects if o.executable]
        self.table_slots: set[int] = set()  # fixup addresses consumed by jump tables

    def in_code(self, a: int) -> bool:
        return any(o.base <= a < o.base + o.vsize for o in self.code)

    def decode(self, a: int):
        if a in self._cache:
            return self._cache[a]
        insn = None
        if self.in_code(a):
            insn = next(self.md.disasm(self.exe.read(a, 16), a, count=1), None)
        self._cache[a] = insn
        return insn

    # ------------------------------------------------------------- discovery
    def add_function(self, a, speculative=False):
        if a in self.functions:
            if not speculative:
                self.functions[a].speculative = False
            return False
        if not self.in_code(a):
            return False
        self.functions[a] = Function(a, speculative)
        return True

    def resolve_table(self, insn):
        op = insn.operands[0]
        if op.type != X86_OP_MEM or op.size != 4 or not op.mem.index or op.mem.scale != 4:
            return None
        table = op.mem.disp & 0xFFFFFFFF
        targets = []
        a = table
        while a in self.exe.fixups and self.in_code(self.exe.fixups[a]) and len(targets) < 1024:
            targets.append(self.exe.fixups[a])
            self.table_slots.add(a)
            a += 4
        return targets or None

    def successors(self, fn, insn):
        m = base_mnemonic(insn)
        ops = insn.operands
        intra, calls = [], []
        if m in COND or m in ("jcxz", "jecxz", "loop", "loope", "loopne"):
            intra.append(ops[0].imm & 0xFFFFFFFF)
            return intra, calls, True
        if m == "jmp":
            if ops[0].type == X86_OP_IMM:
                intra.append(ops[0].imm & 0xFFFFFFFF)
            else:
                t = self.resolve_table(insn)
                fn.tables[insn.address] = t
                if t:
                    intra.extend(t)
                else:
                    self.warnings.append(f"{fname(fn.addr)}: unresolved indirect jmp at {insn.address:08X} ({insn.op_str})")
            return intra, calls, False
        if m == "call":
            if ops[0].type == X86_OP_IMM:
                calls.append(ops[0].imm & 0xFFFFFFFF)
            return intra, calls, True
        if m in ("ret", "retf", "iret", "iretd", "ljmp", "hlt") or (m == "int3"):
            return intra, calls, m == "hlt"
        return intra, calls, True

    def explore(self, fn, stop_at_functions):
        work = collections.deque([fn.addr])
        seen = set()
        calls = []
        fn.body.clear(); fn.labels.clear(); fn.tables.clear()
        while work:
            a = work.popleft()
            if a in seen:
                continue
            if stop_at_functions and a != fn.addr and a in self.functions:
                continue
            seen.add(a)
            insn = self.decode(a)
            fn.body[a] = insn
            if insn is None:
                continue
            intra, c, falls = self.successors(fn, insn)
            calls.extend(c)
            if falls:
                work.append(a + insn.size)
            for t in intra:
                fn.labels.add(t)
                work.append(t)
        return calls

    def discover(self):
        self.add_function(self.exe.entry)
        for h in self.hints.get("functions", []):
            self.add_function(int(h, 16))
        changed = True
        while changed:
            changed = False
            for fn in list(self.functions.values()):
                if fn.body:
                    continue
                for t in self.explore(fn, stop_at_functions=False):
                    self.add_function(t)
                changed = True
            if not changed:
                changed = self.add_pointer_functions()
        for fn in self.functions.values():
            self.explore(fn, stop_at_functions=True)
        for a, fn in list(self.functions.items()):
            if fn.speculative and any(i is None for i in fn.body.values()):
                del self.functions[a]

    def add_pointer_functions(self):
        """Fixed-up pointers into code that aren't jump-table slots: callbacks, handlers."""
        added = False
        for at, target in sorted(self.exe.fixups.items()):
            if at in self.table_slots or not self.in_code(target) or target in self.functions:
                continue
            if self.in_code(at) and self._inside_table_insn(at):
                continue
            insn = self.decode(target)
            if insn is None:
                continue
            added |= self.add_function(target, speculative=True)
        return added

    def _inside_table_insn(self, at):
        return False

    # ------------------------------------------------------------------ emit
    def emit_all(self, out_dir: Path):
        out_dir.mkdir(parents=True, exist_ok=True)
        mod = self.name
        fns = sorted(self.functions.values(), key=lambda f: f.addr)
        decl = [f"// Generated by tools/lifter/lift_le.py from {self.exe.path.name}. DO NOT EDIT.",
                "#pragma once", '#include "runtime/recomp.h"', f"namespace gen::mod_{mod} {{"]
        decl += [f"void {fname(f.addr)}(Machine& m);" for f in fns]
        decl.append("}  // namespace")
        (out_dir / f"{mod}_decl.h").write_text("\n".join(decl) + "\n")

        files = []
        chunk = 150
        for k in range(0, len(fns), chunk):
            part = fns[k:k + chunk]
            lines = [f"// Generated by tools/lifter/lift_le.py from {self.exe.path.name}. DO NOT EDIT.",
                     f'#include "{mod}_decl.h"', f"namespace gen::mod_{mod} {{", ""]
            for fn in part:
                lines += self.emit_function(fn)
            lines.append("}  // namespace")
            p = out_dir / f"{mod}_{part[0].addr:08X}.cpp"
            p.write_text("\n".join(lines) + "\n")
            files.append(p.name)

        img = [f"// Generated by tools/lifter/lift_le.py from {self.exe.path.name}. DO NOT EDIT.",
               f'#include "{mod}_decl.h"', '#include "runtime/module.h"', "",
               f"namespace gen::mod_{mod} {{", "static const LiftedFunction32 kFunctions[] = {"]
        for f in fns:
            img.append(f"    {{0x{f.addr:08X}u, &{fname(f.addr)}}},")
        img.append("};")
        # Object contents are not embedded: the runtime loads TTL.EXE from the game folder.
        img += ["}  // namespace", "", "namespace gen {",
                f"extern const LiftedModule32 kLifted_{mod};",
                f"const LiftedModule32 kLifted_{mod} = {{",
                f'    "{mod}", "{self.exe.path.name}",',
                f"    mod_{mod}::kFunctions, sizeof(mod_{mod}::kFunctions) / sizeof(mod_{mod}::kFunctions[0]),",
                f"    0x{self.exe.entry:08X}u, 0x{self.exe.initial_esp:08X}u,",
                "};", "}  // namespace gen"]
        (out_dir / f"{mod}_table.cpp").write_text("\n".join(img) + "\n")
        files.append(f"{mod}_table.cpp")
        (out_dir / "CMakeLists.txt").write_text(
            "# Generated by tools/lifter/lift_le.py. DO NOT EDIT.\n"
            f"add_library(lifted_{mod} STATIC\n" + "".join(f"    {f}\n" for f in files) + ")\n"
            f"target_link_libraries(lifted_{mod} PRIVATE genesis_runtime)\n"
            f"genesis_configure_generated(lifted_{mod})\n")
        return files

    def emit_function(self, fn):
        repl = self.hints.get("replace", {}).get(f"{fn.addr:08X}")
        if repl:
            body = repl if isinstance(repl, list) else [repl]
            return [f"void {fname(fn.addr)}(Machine& m) {{", "    Cpu& c = m.cpu;", "    (void)c;",
                    f"    // Replaced by hints/{self.name}.json"] + ["    " + l for l in body] + ["}", ""]
        offs = sorted(fn.body)
        jumped = set(t for t in fn.labels if t in fn.body)
        blocks = {}
        for idx, a in enumerate(offs):
            insn = fn.body[a]
            if insn is None:
                blocks[a] = [f"m.invalid_code32(0x{a:08X}u); return;"]
                continue
            try:
                stmts, falls = self.emit_insn(fn, insn)
            except LiftError as ex:
                self.unimpl[str(ex)] += 1
                stmts, falls = [f'm.unimplemented(0, 0x{a & 0xFFFF:04X}, "{a:08X}: {insn_text(insn)}"); return;'], False
            stmts = [f"// {a:08X}: {insn_text(insn)}"] + stmts
            inject = self.hints.get("inject_after", {}).get(f"{a:08X}")
            if inject:
                stmts.append(inject)
            if falls:
                nxt = a + insn.size
                following = offs[idx + 1] if idx + 1 < len(offs) else None
                if nxt != following:
                    if nxt in fn.body:
                        stmts.append(f"goto L_{nxt:08X};")
                        jumped.add(nxt)
                    elif nxt in self.functions:
                        stmts.append(f"{fname(nxt)}(m); return;  // falls into next function")
                    else:
                        stmts.append(f"m.call32(0x{nxt:08X}u); return;  // falls into next function")
            blocks[a] = stmts
        jumped |= {t for t in fn.labels if t in fn.body}
        out = [f"void {fname(fn.addr)}(Machine& m) {{", "    Cpu& c = m.cpu;", "    (void)c;"]
        if fn.addr != offs[0]:
            out.append(f"    goto L_{fn.addr:08X};")
            jumped.add(fn.addr)
        for a in offs:
            if a in jumped:
                out.append(f"L_{a:08X}:")
            out += ["    " + s for s in blocks[a]]
        out += ["}", ""]
        return out

    # ------------------------------------------------------------- operands
    def seg_of(self, insn, op) -> str:
        p = insn.prefix[1]
        if p:
            return SEG_PREFIX[p]
        if op.mem.base and insn.reg_name(op.mem.base) in ("ebp", "esp", "bp", "sp"):
            return "c.ss"
        return "c.ds"

    def ea(self, insn, op, extra=0) -> str:
        mem = op.mem
        parts = []
        if mem.base:
            parts.append(REG[insn.reg_name(mem.base)][0])
        if mem.index:
            r = REG[insn.reg_name(mem.index)][0]
            parts.append(f"{r} * {mem.scale}" if mem.scale > 1 else r)
        disp = (mem.disp + extra) & 0xFFFFFFFF
        if disp or not parts:
            parts.append(f"0x{disp:X}u")
        expr = " + ".join(parts)
        a16 = insn.prefix[3] == 0x67
        return f"(uint16_t)({expr})" if a16 else f"(uint32_t)({expr})"

    def imm(self, op, size) -> str:
        return f"({CTYPE[size]})0x{op.imm & MASK[size]:X}u"

    def rd(self, insn, op, size=None) -> str:
        size = size or op.size
        if op.type == X86_OP_REG:
            r, rs = REG[insn.reg_name(op.reg)]
            return r
        if op.type == X86_OP_IMM:
            return self.imm(op, size)
        if op.type == X86_OP_MEM:
            return f"rd{BITS[size]}(m, {self.seg_of(insn, op)}, {self.ea(insn, op)})"
        raise LiftError(f"operand type {op.type}")

    def wr(self, insn, op, val, size=None) -> str:
        size = size or op.size
        if op.type == X86_OP_REG:
            r, rs = REG[insn.reg_name(op.reg)]
            return f"{r} = ({CTYPE[rs]})({val});"
        if op.type == X86_OP_MEM:
            return f"wr{BITS[size]}(m, {self.seg_of(insn, op)}, {self.ea(insn, op)}, ({CTYPE[size]})({val}));"
        raise LiftError(f"write to operand type {op.type}")

    def jump(self, fn, target, insn) -> str:
        poll = "m.poll_tick(); " if target <= insn.address else ""
        if target in fn.body:
            return f"{{ {poll}goto L_{target:08X}; }}"
        if target in self.functions:
            return f"{{ {poll}{fname(target)}(m); return; }}"
        return f"{{ {poll}m.call32(0x{target:08X}u); return; }}"

    # ----------------------------------------------------------- instructions
    def emit_insn(self, fn, insn):
        m = base_mnemonic(insn)
        ops = insn.operands
        nxt = insn.address + insn.size
        o0 = ops[0] if ops else None
        o1 = ops[1] if len(ops) > 1 else None
        S = o0.size if o0 is not None and o0.type != X86_OP_IMM else 4
        T = CTYPE.get(S, "uint32_t")
        rd = lambda op, size=None: self.rd(insn, op, size)  # noqa: E731
        wr = lambda op, v, size=None: self.wr(insn, op, v, size)  # noqa: E731
        o16 = insn.prefix[2] == 0x66

        if m in ("nop", "wait", "fwait"):
            return [], True
        if m == "mov":
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
            sg, ea = self.seg_of(insn, o1), self.ea(insn, o1)
            return [f"{{ {T} o = rd{BITS[S]}(m, {sg}, {ea}); uint16_t s = rd16(m, {sg}, {self.ea(insn, o1, S)}); "
                    f"{wr(o0, 'o')} {sreg} = s; }}"], True
        if m == "xchg":
            return [f"{{ {T} a = {rd(o0)}; {T} b = {rd(o1)}; {wr(o0, 'b')} {wr(o1, 'a')} }}"], True
        if m == "lsl":
            return [f"{{ uint16_t s = (uint16_t){rd(o1, 2)}; if (m.sel_used[s >> 3]) {{ {wr(o0, 'm.sel_limit[s >> 3]')} c.zf = 1; }} else c.zf = 0; }}"], True
        if m == "bswap":
            return [wr(o0, f"__builtin_bswap32({rd(o0)})")], True
        if m == "push":
            if o0.type == X86_OP_IMM:
                size = 2 if o16 else 4
                return [f"push{BITS[size]}(m, {self.imm(o0, size)});"], True
            if o0.type == X86_OP_REG and insn.reg_name(o0.reg) in SEGREGS:
                return [f"push{'16' if o16 else '32'}(m, {rd(o0)});"], True
            return [f"push{BITS[o0.size]}(m, {rd(o0)});"], True
        if m == "pop":
            if o0.type == X86_OP_REG and insn.reg_name(o0.reg) in SEGREGS:
                return [f"{rd(o0)} = (uint16_t)pop{'16' if o16 else '32'}(m);"], True
            return [f"{{ {T} t = pop{BITS[S]}(m); {wr(o0, 't')} }}"], True
        if m in ("pushf", "pushfd"):
            return [f"push{'16' if m == 'pushf' else '32'}(m, c.flags16());"], True
        if m in ("popf", "popfd"):
            return [f"c.set_flags16((uint16_t)pop{'16' if m == 'popf' else '32'}(m));"], True
        if m in ("pushal", "pushad"):
            return ["pushal(m);"], True
        if m in ("popal", "popad"):
            return ["popal(m);"], True
        if m in ("pushaw", "pusha"):
            return ["pusha(m);"], True
        if m in ("popaw", "popa"):
            return ["popa(m);"], True
        if m == "enter":
            if (ops[1].imm & 0xFF) != 0:
                raise LiftError("enter level>0")
            return [f"push32(m, c.bp.e); c.bp.e = c.sp.e; c.sp.e -= 0x{ops[0].imm & 0xFFFF:X}u;"], True
        if m == "leave":
            return ["c.sp.e = c.bp.e; c.bp.e = pop32(m);"], True
        if m == "cbw":
            return ["c.ax.w = (uint16_t)(int16_t)(int8_t)c.ax.b.l;"], True
        if m == "cwde":
            return ["c.ax.e = (uint32_t)(int32_t)(int16_t)c.ax.w;"], True
        if m == "cwd":
            return ["c.dx.w = (c.ax.w & 0x8000) ? 0xFFFF : 0;"], True
        if m == "cdq":
            return ["c.dx.e = (c.ax.e & 0x80000000u) ? 0xFFFFFFFFu : 0;"], True
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
            return [f"op_{m}(m, {ops[0].imm & 0xFF if ops else 10});"], True
        if m == "xlatb":
            sg = SEG_PREFIX.get(insn.prefix[1], "c.ds")
            return [f"c.ax.b.l = rd8(m, {sg}, (uint32_t)(c.bx.e + c.ax.b.l));"], True
        if m == "in":
            port = rd(o1) if o1.type == X86_OP_REG else f"0x{o1.imm & 0xFF:X}"
            return [f"{REG[insn.reg_name(o0.reg)][0]} = m.in{BITS[o0.size]}({port});"], True
        if m == "out":
            port = rd(o0) if o0.type == X86_OP_REG else f"0x{o0.imm & 0xFF:X}"
            return [f"m.out{BITS[o1.size]}({port}, {rd(o1)});"], True
        if m == "int":
            return [f"m.interrupt(0x{o0.imm & 0xFF:X}, 0, 0x{insn.address & 0xFFFF:04X});"], True
        if m == "int3":
            return [f"m.interrupt(3, 0, 0x{insn.address & 0xFFFF:04X});"], False
        if m == "hlt":
            return ["m.idle();"], True
        if m in STRING_OPS or m[:-1] in STRING_OPS:
            return [self.string_op(insn, m)], True
        if m.startswith("f"):
            return [f'm.fpu_emulated(0, 0, 0x{insn.address & 0xFFFF:04X}, "{insn.mnemonic} {insn.op_str}");'], True

        # control flow
        if m in COND:
            return [f"if ({COND[m]}) {self.jump(fn, o0.imm & 0xFFFFFFFF, insn)}"], True
        if m in ("jcxz", "jecxz"):
            r = "c.cx.w" if m == "jcxz" else "c.cx.e"
            return [f"if ({r} == 0) {self.jump(fn, o0.imm & 0xFFFFFFFF, insn)}"], True
        if m in ("loop", "loope", "loopne"):
            r = "c.cx.w" if insn.prefix[3] == 0x67 else "c.cx.e"
            cond = {"loop": "", "loope": " && c.zf", "loopne": " && !c.zf"}[m]
            return [f"if (--{r} != 0{cond}) {self.jump(fn, o0.imm & 0xFFFFFFFF, insn)}"], True
        if m == "jmp":
            if o0.type == X86_OP_IMM:
                return [self.jump(fn, o0.imm & 0xFFFFFFFF, insn)], False
            targets = fn.tables.get(insn.address)
            if targets:
                sw = " ".join(f"case 0x{t:08X}u: goto L_{t:08X};" for t in sorted(set(targets)) if t in fn.body)
                return [f"{{ uint32_t t = {rd(o0, 4)}; m.poll_tick(); switch (t) {{ {sw} default: m.call32(t); return; }} }}"], False
            return [f"{{ uint32_t t = {rd(o0, 4)}; m.call32(t); return; }}"], False
        if m == "call":
            if o0.type == X86_OP_IMM:
                t = o0.imm & 0xFFFFFFFF
                if t in self.functions:
                    return [f"push32(m, 0x{nxt:08X}u); {fname(t)}(m);"], True
                return [f"push32(m, 0x{nxt:08X}u); m.call32(0x{t:08X}u);"], True
            return [f"{{ uint32_t t = {rd(o0, 4)}; push32(m, 0x{nxt:08X}u); m.call32(t); }}"], True
        if m == "ret":
            extra = f" c.sp.e += 0x{o0.imm:X}u;" if ops else ""
            return [f"(void)pop32(m);{extra} return;"], False
        if m == "retf":
            extra = f" c.sp.e += 0x{o0.imm:X}u;" if ops else ""
            return [f"(void)pop32(m); c.cs = (uint16_t)pop32(m);{extra} return;"], False
        if m in ("iretd", "iret"):
            return ["(void)pop32(m); c.cs = (uint16_t)pop32(m); c.set_flags16((uint16_t)pop32(m)); return;"], False
        if m in ("lcall", "ljmp") and o0.type == X86_OP_MEM:
            sg, ea = self.seg_of(insn, o0), self.ea(insn, o0)
            load = (f"uint32_t o = rd32(m, {sg}, {ea}); uint16_t s = rd16(m, {sg}, {self.ea(insn, o0, 4)});")
            if m == "lcall":
                return [f"{{ {load} push32(m, c.cs); push32(m, 0x{nxt:08X}u); c.cs = s; m.call32(m.sel_base[s >> 3] + o); }}"], True
            return [f"{{ {load} c.cs = s; m.call32(m.sel_base[s >> 3] + o); return; }}"], False
        raise LiftError(m)

    def string_op(self, insn, m):
        kind = m[:-1] if m[:-1] in STRING_OPS else m
        size = {"b": 1, "w": 2, "d": 4}[m[-1]]
        rep = {0xF3: 1, 0xF2: 2}.get(insn.prefix[0], 0)
        if not rep:
            # Capstone drops F2 in front of movsd (it reads it as the SSE2 MOVSD prefix).
            for b in insn.bytes:
                if b in (0xF2, 0xF3):
                    rep = 1 if b == 0xF3 else 2
                    break
                if b not in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0):
                    break
        a32 = "false" if insn.prefix[3] == 0x67 else "true"
        sseg = SEG_PREFIX.get(insn.prefix[1], "c.ds")
        if kind in ("stos", "scas", "ins"):
            return f"str_{kind}<{CTYPE[size]}>(m, {rep}, {a32});"
        return f"str_{kind}<{CTYPE[size]}>(m, {sseg}, {rep}, {a32});"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--hints", type=Path)
    ap.add_argument("--stats-only", action="store_true")
    args = ap.parse_args()
    hints = json.loads(args.hints.read_text()) if args.hints and args.hints.exists() else {}
    exe = LeExe.load(args.exe)
    lf = Lifter32(exe, args.name, hints)
    lf.discover()
    n = sum(len(f.body) for f in lf.functions.values())
    spec = sum(1 for f in lf.functions.values() if f.speculative)
    if not args.stats_only:
        files = lf.emit_all(args.out)
        print(f"wrote {len(files)} files to {args.out}")
    print(f"{exe.path.name}: {len(lf.functions)} functions ({spec} speculative), {n} instructions")
    for w in lf.warnings:
        print("warning:", w)
    if lf.unimpl:
        print("unimplemented:", dict(lf.unimpl.most_common()))
    if not args.stats_only:
        (args.out / "lift_report.json").write_text(json.dumps({
            "exe": exe.path.name, "functions": len(lf.functions), "speculative": spec, "instructions": n,
            "unimplemented": dict(lf.unimpl), "warnings": lf.warnings}, indent=2) + "\n")


if __name__ == "__main__":
    main()
