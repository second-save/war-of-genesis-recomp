#!/usr/bin/env python3
"""Softmax "New Fast&Compress Sprite Ver3.0" files (.SPR .SYF .ICO .CHR ...).

Layout: 64-byte signature text, at 0x40 {u16 0x031E, u16 frame count}, padding
to 0x80, then frames back to back:
  u32 size (bytes after this field... see frames()), u16 w, u16 h, u16 x, u16 y,
  u16 run count, then runs {u16 skip, u8 dwords, u8 bytes, 4*dwords+bytes pixels}.
Runs address a screen with a fixed pitch (320 for mode 13h): skip advances the
destination, then the pixels are copied. Colour 0 areas are simply skipped.

  spr.py png FILE OUTDIR [PALETTE]     every frame as PNG (sheet + frames)
"""
import struct
import sys
from pathlib import Path

SIG = b"New Fast&Compress Sprite"


class Frame:
    def __init__(self, w, h, x, y, runs, pitch):
        self.w, self.h, self.x, self.y, self.runs, self.pitch = w, h, x, y, runs, pitch

    def pixels(self):
        """{(x, y): colour}."""
        out = {}
        pos = 0
        for skip, data in self.runs:
            pos += skip
            for i, c in enumerate(data):
                p = pos + i
                out[(p % self.pitch, p // self.pitch)] = c
            pos += len(data)
        return out


def frames(d: bytes, pitch=320):
    assert d.startswith(SIG), "not a Softmax sprite file"
    n = struct.unpack_from("<H", d, 0x42)[0]
    p = 0x80
    out = []
    for _ in range(n):
        if p + 14 > len(d):
            break
        size, w, h = struct.unpack_from("<IHH", d, p)
        if size == 4 + w * h:  # raw frame: w*h pixels, 0 = transparent
            # header {size, w, h, w, h} or {size, w, h, x, y, w, h}
            hdr = 12 if struct.unpack_from("<HH", d, p + 8) == (w, h) else 16
            raw = d[p + hdr:p + hdr + w * h]
            runs, pos = [], 0
            for yy in range(h):
                row = raw[yy * w:(yy + 1) * w]
                runs.append((yy * pitch - pos, row))
                pos = yy * pitch + w
            out.append((p, p + hdr + w * h, Frame(w, h, 0, 0, runs, pitch)))
            p += hdr + w * h
            continue
        x, y, nruns = struct.unpack_from("<HHH", d, p + 8)
        q = p + 14
        long_header = w * h and not any(d[p + 0x10:p + 0x20])
        if long_header:  # 32-byte header; size counts from +0x20 (run count, runs)
            nruns = struct.unpack_from("<H", d, p + 0x20)[0]
            q = p + 0x22
        runs = []
        for _ in range(nruns):
            if q + 4 > len(d):
                break
            skip, nd, nb = struct.unpack_from("<HBB", d, q)
            q += 4
            n_ = 4 * nd + nb
            runs.append((skip, d[q:q + n_]))
            q += n_
        if long_header:
            q = p + 0x20 + size
        out.append((p, q, Frame(w, h, x, y, runs, pitch)))
        p = q
    return out


def palette(path):
    pal = Path(path).read_bytes()[:768]
    mx = max(pal) or 1
    k = 4 if mx < 64 else 1
    return [min(255, v * k) for v in pal]


def to_png(path, outdir, palpath=None, pitch=320):
    from PIL import Image
    d = Path(path).read_bytes()
    pal = palette(palpath) if palpath else None
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    ims = []
    for i, (_, _, f) in enumerate(frames(d, pitch)):
        px = f.pixels()
        w = max([f.w] + [x + 1 for x, _ in px]) if px else f.w
        h = max([f.h] + [y + 1 for _, y in px]) if px else f.h
        im = Image.new("P", (max(1, min(w, pitch)), max(1, h)), 0)
        if pal:
            im.putpalette(pal)
        for (x, y), c in px.items():
            if x < im.width and y < im.height:
                im.putpixel((x, y), c)
        ims.append(im)
    if not ims:
        return 0
    cols = min(8, len(ims))
    cw = max(i.width for i in ims) + 2
    ch = max(i.height for i in ims) + 2
    sheet = Image.new("RGB", (cols * cw, ((len(ims) + cols - 1) // cols) * ch), (255, 0, 255))
    for i, im in enumerate(ims):
        sheet.paste(im.convert("RGB"), ((i % cols) * cw, (i // cols) * ch))
    sheet.save(outdir / (Path(path).name + ".png"))
    return len(ims)


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "png":
        print(to_png(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None), "frames")
    else:
        sys.exit(__doc__)


# ---------------------------------------------------------------- VSP
# Chapter titles, portraits: u16 run count, then the same runs as above,
# drawn into a linear frame buffer (640 wide in the SVGA scenes).

def vsp_pixels(d: bytes, pitch=640):
    n = struct.unpack_from("<H", d, 0)[0]
    q, pos, out = 2, 0, {}
    for _ in range(n):
        skip, nd, nb = struct.unpack_from("<HBB", d, q)
        q += 4
        k = 4 * nd + nb
        pos += skip
        for i, c in enumerate(d[q:q + k]):
            out[((pos + i) % pitch, (pos + i) // pitch)] = c
        pos += k
        q += k
    return out


def vsp_encode(px: dict, pitch=640) -> bytes:
    """{(x, y): colour} -> VSP bytes (row-major spans of present pixels)."""
    lin = sorted(y * pitch + x for (x, y) in px)
    spans, start, prev = [], None, None
    for p in lin:
        if start is None:
            start = prev = p
        elif p == prev + 1 and (p - start) < 1020:
            prev = p
        else:
            spans.append((start, prev))
            start = prev = p
    if start is not None:
        spans.append((start, prev))
    out, pos = bytearray(struct.pack("<H", len(spans))), 0
    for a, b in spans:
        k = b - a + 1
        skip = a - pos
        while skip > 0xFFFF:  # never in practice: 640x102 rows of gap
            raise ValueError("gap too large")
        out += struct.pack("<HBB", skip, k // 4, k % 4)
        out += bytes(px[(p % pitch, p // pitch)] for p in range(a, b + 1))
        pos = b + 1
    return bytes(out)
