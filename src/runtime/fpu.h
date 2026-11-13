// x87 helpers called from lifted code (real FPU instructions and the
// Borland emulator's INT 34h..3Dh forms both lower to these).
#pragma once

#include "machine.h"
#include "memory.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace gen {

inline long double& fst(Machine& m, int i) { return m.fpu.st[(m.fpu.top + i) & 7]; }
inline void fpush(Machine& m, long double v) {
    m.fpu.top = (m.fpu.top - 1) & 7;
    m.fpu.st[m.fpu.top] = v;
    m.fpu.tag &= static_cast<uint8_t>(~(1u << m.fpu.top));
}
inline void fpop(Machine& m) {
    m.fpu.tag |= static_cast<uint8_t>(1u << m.fpu.top);
    m.fpu.top = (m.fpu.top + 1) & 7;
}

inline uint64_t frd64(Machine& m, uint16_t seg, uint32_t off) {
    return rd32(m, seg, off) | (static_cast<uint64_t>(rd32(m, seg, off + 4)) << 32);
}
inline void fwr64(Machine& m, uint16_t seg, uint32_t off, uint64_t v) {
    wr32(m, seg, off, static_cast<uint32_t>(v));
    wr32(m, seg, off + 4, static_cast<uint32_t>(v >> 32));
}

// Memory operand loads.
inline long double fld_m16i(Machine& m, uint16_t s, uint32_t o) { return static_cast<int16_t>(rd16(m, s, o)); }
inline long double fld_m32i(Machine& m, uint16_t s, uint32_t o) { return static_cast<int32_t>(rd32(m, s, o)); }
inline long double fld_m64i(Machine& m, uint16_t s, uint32_t o) { return static_cast<int64_t>(frd64(m, s, o)); }
inline long double fld_m32(Machine& m, uint16_t s, uint32_t o) {
    uint32_t b = rd32(m, s, o); float f; std::memcpy(&f, &b, 4); return f;
}
inline long double fld_m64(Machine& m, uint16_t s, uint32_t o) {
    uint64_t b = frd64(m, s, o); double d; std::memcpy(&d, &b, 8); return d;
}
inline long double fld_m80(Machine& m, uint16_t s, uint32_t o) {
    uint64_t mant = frd64(m, s, o);
    uint16_t se = rd16(m, s, o + 8);
    int exp = se & 0x7FFF;
    long double v;
    if (exp == 0 && mant == 0) v = 0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = std::ldexp(static_cast<long double>(mant), exp - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}

// Integer conversion with the control word's rounding mode.
inline long double fround(Machine& m, long double v) {
    switch ((m.fpu.cw >> 10) & 3) {
    case 0: return std::nearbyint(v);  // host default: to nearest, ties to even
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}
template <typename T>
inline T fto_int(Machine& m, long double v) {
    long double r = fround(m, v);
    if (!(r >= static_cast<long double>(std::numeric_limits<T>::min()) &&
          r <= static_cast<long double>(std::numeric_limits<T>::max())))
        return std::numeric_limits<T>::min();  // "integer indefinite"
    return static_cast<T>(r);
}

inline void fst_m16i(Machine& m, uint16_t s, uint32_t o, long double v) { wr16(m, s, o, static_cast<uint16_t>(fto_int<int16_t>(m, v))); }
inline void fst_m32i(Machine& m, uint16_t s, uint32_t o, long double v) { wr32(m, s, o, static_cast<uint32_t>(fto_int<int32_t>(m, v))); }
inline void fst_m64i(Machine& m, uint16_t s, uint32_t o, long double v) { fwr64(m, s, o, static_cast<uint64_t>(fto_int<int64_t>(m, v))); }
inline void fst_m32(Machine& m, uint16_t s, uint32_t o, long double v) {
    float f = static_cast<float>(v); uint32_t b; std::memcpy(&b, &f, 4); wr32(m, s, o, b);
}
inline void fst_m64(Machine& m, uint16_t s, uint32_t o, long double v) {
    double d = static_cast<double>(v); uint64_t b; std::memcpy(&b, &d, 8); fwr64(m, s, o, b);
}
inline void fst_m80(Machine& m, uint16_t s, uint32_t o, long double v) {
    uint16_t se = std::signbit(v) ? 0x8000 : 0;
    long double a = std::fabs(v);
    uint64_t mant = 0;
    if (std::isnan(a)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (std::isinf(a)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (a != 0) {
        int e;
        long double f = std::frexp(a, &e);           // a = f * 2^e, f in [0.5, 1)
        mant = static_cast<uint64_t>(std::ldexp(f, 64));
        se |= static_cast<uint16_t>(e - 1 + 16383);
    }
    fwr64(m, s, o, mant);
    wr16(m, s, o + 8, se);
}

inline void fcompare(Machine& m, long double a, long double b) {
    Fpu& f = m.fpu;
    if (std::isnan(a) || std::isnan(b)) { f.c0 = f.c2 = f.c3 = 1; return; }
    f.c2 = 0;
    f.c0 = a < b;
    f.c3 = a == b;
}

inline void fxam(Machine& m) {
    Fpu& f = m.fpu;
    long double v = fst(m, 0);
    f.c1 = std::signbit(v);
    if (m.fpu.tag & (1u << f.top)) { f.c3 = 1; f.c2 = 0; f.c0 = 1; return; }  // empty
    if (std::isnan(v)) { f.c3 = 0; f.c2 = 0; f.c0 = 1; }
    else if (std::isinf(v)) { f.c3 = 0; f.c2 = 1; f.c0 = 1; }
    else if (v == 0) { f.c3 = 1; f.c2 = 0; f.c0 = 0; }
    else { f.c3 = 0; f.c2 = 1; f.c0 = 0; }
}

}  // namespace gen
