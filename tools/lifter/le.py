"""Minimal LE (DOS/4GW linear executable) loader for the static recompiler.

Objects are placed at their preferred base addresses (flat linear memory) and
internal fixups are applied, so the resulting image matches what DOS/4GW maps.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class LeObject:
    index: int
    base: int
    vsize: int
    flags: int
    data: bytearray

    @property
    def executable(self) -> bool:
        return bool(self.flags & 0x4)

    @property
    def big(self) -> bool:
        return bool(self.flags & 0x2000)


@dataclass
class LeExe:
    path: Path
    objects: list[LeObject]
    eip_obj: int
    eip: int
    esp_obj: int
    esp: int
    # Linear addresses holding a fixed-up 32-bit offset -> target linear address.
    fixups: dict[int, int] = field(default_factory=dict)
    # Linear addresses of 16-bit selector fixups -> target object index.
    selector_fixups: dict[int, int] = field(default_factory=dict)

    @property
    def entry(self) -> int:
        return self.objects[self.eip_obj - 1].base + self.eip

    @property
    def initial_esp(self) -> int:
        return self.objects[self.esp_obj - 1].base + self.esp

    def obj_at(self, lin: int) -> LeObject | None:
        for o in self.objects:
            if o.base <= lin < o.base + len(o.data):
                return o
        return None

    def read(self, lin: int, n: int) -> bytes:
        o = self.obj_at(lin)
        if o is None:
            return b""
        i = lin - o.base
        return bytes(o.data[i:i + n])

    def dword(self, lin: int) -> int:
        b = self.read(lin, 4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else 0

    @classmethod
    def load(cls, path: Path) -> "LeExe":
        d = Path(path).read_bytes()
        le = struct.unpack_from("<I", d, 0x3C)[0]
        h = memoryview(d)[le:]
        if bytes(h[:2]) != b"LE":
            raise ValueError("not an LE executable")
        page_size = struct.unpack_from("<I", h, 0x28)[0]
        last_page = struct.unpack_from("<I", h, 0x2C)[0]
        npages = struct.unpack_from("<I", h, 0x14)[0]
        eip_obj, eip, esp_obj, esp = struct.unpack_from("<IIII", h, 0x18)
        objtab, nobj = struct.unpack_from("<II", h, 0x40)
        pagetab = struct.unpack_from("<I", h, 0x48)[0]
        fixpage, fixrec = struct.unpack_from("<II", h, 0x68)
        datapages = struct.unpack_from("<I", h, 0x80)[0]

        # Page map: logical page (1-based) -> file offset
        def page_off(n):  # n 1-based
            return datapages + (n - 1) * page_size

        objects = []
        for i in range(nobj):
            vs, base, fl, pi, pc, _ = struct.unpack_from("<IIIIII", h, objtab + i * 24)
            data = bytearray(vs)
            for p in range(pc):
                pn = pi + p
                size = last_page if pn == npages else page_size
                chunk = d[page_off(pn):page_off(pn) + size]
                data[p * page_size:p * page_size + len(chunk)] = chunk[:max(0, min(len(chunk), vs - p * page_size))]
            objects.append(LeObject(i + 1, base, vs, fl, data))
        exe = cls(Path(path), objects, eip_obj, eip, esp_obj, esp)

        # Fixups: per page (1-based global page index across objects)
        for o in objects:
            pi = struct.unpack_from("<IIIIII", h, objtab + (o.index - 1) * 24)[3]
            pc = struct.unpack_from("<IIIIII", h, objtab + (o.index - 1) * 24)[4]
            for p in range(pc):
                g = pi + p - 1
                start, end = struct.unpack_from("<II", h, fixpage + g * 4)
                pos = fixrec + start
                endp = fixrec + end
                while pos < endp:
                    src = h[pos]; flags = h[pos + 1]; pos += 2
                    stype = src & 0x0F
                    if src & 0x20:
                        cnt = h[pos]; pos += 1
                        srcoffs = None
                    else:
                        srcoff = struct.unpack_from("<h", h, pos)[0]; pos += 2
                        cnt = 1
                        srcoffs = [srcoff]
                    if flags & 0x03 != 0:
                        raise ValueError(f"unsupported fixup target type {flags:#x}")
                    if flags & 0x40:
                        tobj = struct.unpack_from("<H", h, pos)[0]; pos += 2
                    else:
                        tobj = h[pos]; pos += 1
                    toff = 0
                    if stype != 0x02:
                        if flags & 0x10:
                            toff = struct.unpack_from("<I", h, pos)[0]; pos += 4
                        else:
                            toff = struct.unpack_from("<H", h, pos)[0]; pos += 2
                    if srcoffs is None:
                        srcoffs = [struct.unpack_from("<h", h, pos + 2 * k)[0] for k in range(cnt)]
                        pos += 2 * cnt
                    target = objects[tobj - 1].base + toff
                    for so in srcoffs:
                        lin_page = o.base + p * page_size
                        at = lin_page + so
                        exe._apply(stype, at, target, tobj)
        return exe

    def _apply(self, stype, at, target, tobj):
        o = self.obj_at(at)
        if stype == 0x07:  # 32-bit offset
            self._put(at, struct.pack("<I", target))
            self.fixups[at] = target
        elif stype == 0x08:  # 32-bit self-relative
            self._put(at, struct.pack("<I", (target - (at + 4)) & 0xFFFFFFFF))
        elif stype == 0x02:  # 16-bit selector
            self._put(at, struct.pack("<H", self.selector(tobj)))
            self.selector_fixups[at] = tobj
        elif stype == 0x06:  # 16:32 pointer
            self._put(at, struct.pack("<IH", target, self.selector(tobj)))
            self.fixups[at] = target
        elif stype == 0x05:  # 16-bit offset
            self._put(at, struct.pack("<H", target & 0xFFFF))
        else:
            raise ValueError(f"unsupported fixup source type {stype:#x} at {at:#x}")

    @staticmethod
    def selector(tobj: int) -> int:
        # Flat model: every object shares the flat code/data selectors.
        return 0x0168 if tobj == 1 else 0x0170

    def _put(self, at, b):
        # A fixup may straddle a page/object end: write what fits.
        for k, v in enumerate(b):
            o = self.obj_at(at + k)
            if o is not None:
                o.data[at + k - o.base] = v
