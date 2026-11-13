// Emulated x86 real-mode CPU state shared by the runtime and lifted code.
#pragma once

#include <cstdint>

namespace gen {

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The register model assumes a little-endian host"
#endif

// A general-purpose register with 8/16/32-bit views (little-endian host).
union Reg {
    uint32_t e;
    uint16_t w;
    struct {
        uint8_t l, h;
    } b;
};

// x87 FPU state. Values are kept as long double (on arm64 macOS that is a
// double, which is enough for this game's arithmetic).
struct Fpu {
    long double st[8]{};
    uint8_t top = 0;
    uint8_t tag = 0xFF;          // bit i set = physical register i empty
    uint16_t cw = 0x037F;
    uint8_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    uint16_t sw() const {
        return static_cast<uint16_t>((c0 << 8) | (c1 << 9) | (c2 << 10) | ((top & 7) << 11) | (c3 << 14));
    }
    void init() { top = 0; tag = 0xFF; cw = 0x037F; c0 = c1 = c2 = c3 = 0; }
};

struct Cpu {
    Reg ax{}, bx{}, cx{}, dx{}, si{}, di{}, bp{}, sp{};
    uint16_t cs = 0, ds = 0, es = 0, ss = 0, fs = 0, gs = 0;
    uint16_t ip = 0;

    // Control and debug registers (for CPU detection routines)
    uint32_t cr0 = 0x00000010;
    uint32_t cr1 = 0, cr2 = 0, cr3 = 0, cr4 = 0;
    uint32_t dr0 = 0, dr1 = 0, dr2 = 0, dr3 = 0, dr4 = 0, dr5 = 0, dr6 = 0, dr7 = 0;

    // Flags are stored unpacked (0/1) for cheap access from lifted code.
    uint8_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0;
    uint8_t df = 0, ifl = 1, tf = 0;

    uint16_t flags16() const {
        return static_cast<uint16_t>(0x0002 | cf | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) |
                                     (tf << 8) | (ifl << 9) | (df << 10) | (of << 11));
    }
    void set_flags16(uint16_t f) {
        cf = f & 1;
        pf = (f >> 2) & 1;
        af = (f >> 4) & 1;
        zf = (f >> 6) & 1;
        sf = (f >> 7) & 1;
        tf = (f >> 8) & 1;
        ifl = (f >> 9) & 1;
        df = (f >> 10) & 1;
        of = (f >> 11) & 1;
    }
};

}  // namespace gen
