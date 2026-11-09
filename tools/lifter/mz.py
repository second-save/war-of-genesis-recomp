"""Minimal DOS MZ executable parser used by the static recompiler.

Only what the lifter needs: header fields, the load image, and the
relocation table (expressed as image-relative byte offsets).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class MzExe:
    path: Path
    raw: bytes
    header_paras: int
    min_alloc: int
    max_alloc: int
    init_ss: int
    init_sp: int
    init_ip: int
    init_cs: int
    image: bytes
    relocs: list[tuple[int, int]] = field(default_factory=list)  # (seg, off)

    @property
    def reloc_offsets(self) -> set[int]:
        """Image-relative byte offsets of every relocated 16-bit word."""
        return {seg * 16 + off for seg, off in self.relocs}

    def word(self, image_off: int) -> int:
        return struct.unpack_from("<H", self.image, image_off)[0]

    @classmethod
    def load(cls, path: str | Path) -> "MzExe":
        path = Path(path)
        raw = path.read_bytes()
        (sig, last_page, n_pages, n_rel, hdr_paras, min_alloc, max_alloc,
         ss, sp, _csum, ip, cs, rel_off, _ovno) = struct.unpack_from("<2sHHHHHHHHHHHHH", raw, 0)
        if sig not in (b"MZ", b"ZM"):
            raise ValueError(f"{path}: not an MZ executable")
        file_size = (n_pages - 1) * 512 + (last_page or 512) if n_pages else len(raw)
        file_size = min(file_size, len(raw))
        image = raw[hdr_paras * 16:file_size]
        relocs = [struct.unpack_from("<HH", raw, rel_off + i * 4)[::-1] for i in range(n_rel)]
        return cls(path, raw, hdr_paras, min_alloc, max_alloc, ss, sp, ip, cs, image, relocs)
