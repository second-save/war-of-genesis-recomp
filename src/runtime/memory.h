// Memory and stack operations for the static recompiler runtime.
#pragma once

#include "machine.h"
#include "alu.h"
#include <cstring>
#include <cstdint>

namespace gen {

// ---------------------------------------------------------------------------
// Memory Read / Write
// ---------------------------------------------------------------------------
inline uint8_t rd8(Machine& m, uint16_t seg, uint32_t off) {
    return *m.ptr(seg, off);
}

inline uint16_t rd16(Machine& m, uint16_t seg, uint32_t off) {
    const uint8_t* p = m.ptr(seg, off);
    uint16_t val;
    std::memcpy(&val, p, sizeof(val));
    return val;
}

inline uint32_t rd32(Machine& m, uint16_t seg, uint32_t off) {
    const uint8_t* p = m.ptr(seg, off);
    uint32_t val;
    std::memcpy(&val, p, sizeof(val));
    return val;
}

inline void wr8(Machine& m, uint16_t seg, uint32_t off, uint8_t val) {
    *m.ptr(seg, off) = val;
}

inline void wr16(Machine& m, uint16_t seg, uint32_t off, uint16_t val) {
    uint8_t* p = m.ptr(seg, off);
    std::memcpy(p, &val, sizeof(val));
}

inline void wr32(Machine& m, uint16_t seg, uint32_t off, uint32_t val) {
    uint8_t* p = m.ptr(seg, off);
    std::memcpy(p, &val, sizeof(val));
}

// ---------------------------------------------------------------------------
// Stack Operations
// ---------------------------------------------------------------------------
inline void push16(Machine& m, uint16_t val) {
    m.cpu.sp.w = static_cast<uint16_t>(m.cpu.sp.w - 2);
    wr16(m, m.cpu.ss, m.cpu.sp.w, val);
}

inline uint16_t pop16(Machine& m) {
    uint16_t val = rd16(m, m.cpu.ss, m.cpu.sp.w);
    m.cpu.sp.w = static_cast<uint16_t>(m.cpu.sp.w + 2);
    return val;
}

inline void push32(Machine& m, uint32_t val) {
    m.cpu.sp.w = static_cast<uint16_t>(m.cpu.sp.w - 4);
    wr32(m, m.cpu.ss, m.cpu.sp.w, val);
}

inline uint32_t pop32(Machine& m) {
    uint32_t val = rd32(m, m.cpu.ss, m.cpu.sp.w);
    m.cpu.sp.w = static_cast<uint16_t>(m.cpu.sp.w + 4);
    return val;
}

inline void pusha(Machine& m) {
    uint16_t temp_sp = m.cpu.sp.w;
    push16(m, m.cpu.ax.w);
    push16(m, m.cpu.cx.w);
    push16(m, m.cpu.dx.w);
    push16(m, m.cpu.bx.w);
    push16(m, temp_sp);
    push16(m, m.cpu.bp.w);
    push16(m, m.cpu.si.w);
    push16(m, m.cpu.di.w);
}

inline void popa(Machine& m) {
    m.cpu.di.w = pop16(m);
    m.cpu.si.w = pop16(m);
    m.cpu.bp.w = pop16(m);
    (void)pop16(m); // discard original SP
    m.cpu.bx.w = pop16(m);
    m.cpu.dx.w = pop16(m);
    m.cpu.cx.w = pop16(m);
    m.cpu.ax.w = pop16(m);
}

inline void pushal(Machine& m) {
    uint32_t temp_sp = m.cpu.sp.e;
    push32(m, m.cpu.ax.e);
    push32(m, m.cpu.cx.e);
    push32(m, m.cpu.dx.e);
    push32(m, m.cpu.bx.e);
    push32(m, temp_sp);
    push32(m, m.cpu.bp.e);
    push32(m, m.cpu.si.e);
    push32(m, m.cpu.di.e);
}

inline void popal(Machine& m) {
    m.cpu.di.e = pop32(m);
    m.cpu.si.e = pop32(m);
    m.cpu.bp.e = pop32(m);
    (void)pop32(m); // discard original ESP
    m.cpu.bx.e = pop32(m);
    m.cpu.dx.e = pop32(m);
    m.cpu.cx.e = pop32(m);
    m.cpu.ax.e = pop32(m);
}

// ---------------------------------------------------------------------------
// Multiply / Divide
// ---------------------------------------------------------------------------
inline void op_mul8(Machine& m, uint8_t val) {
    uint16_t res = static_cast<uint16_t>(m.cpu.ax.b.l) * val;
    m.cpu.ax.w = res;
    bool cf = (m.cpu.ax.b.h != 0);
    m.cpu.cf = cf ? 1 : 0;
    m.cpu.of = cf ? 1 : 0;
}

inline void op_mul16(Machine& m, uint16_t val) {
    uint32_t res = static_cast<uint32_t>(m.cpu.ax.w) * val;
    m.cpu.ax.w = static_cast<uint16_t>(res);
    m.cpu.dx.w = static_cast<uint16_t>(res >> 16);
    bool cf = (m.cpu.dx.w != 0);
    m.cpu.cf = cf ? 1 : 0;
    m.cpu.of = cf ? 1 : 0;
}

inline void op_mul32(Machine& m, uint32_t val) {
    uint64_t res = static_cast<uint64_t>(m.cpu.ax.e) * val;
    m.cpu.ax.e = static_cast<uint32_t>(res);
    m.cpu.dx.e = static_cast<uint32_t>(res >> 32);
    bool cf = (m.cpu.dx.e != 0);
    m.cpu.cf = cf ? 1 : 0;
    m.cpu.of = cf ? 1 : 0;
}

inline void op_imul8(Machine& m, uint8_t val) {
    int16_t res = static_cast<int16_t>(static_cast<int8_t>(m.cpu.ax.b.l)) * static_cast<int8_t>(val);
    m.cpu.ax.w = static_cast<uint16_t>(res);
    bool cf = (res != static_cast<int8_t>(res));
    m.cpu.cf = cf ? 1 : 0;
    m.cpu.of = cf ? 1 : 0;
}

inline void op_imul16(Machine& m, uint16_t val) {
    int32_t res = static_cast<int32_t>(static_cast<int16_t>(m.cpu.ax.w)) * static_cast<int16_t>(val);
    m.cpu.ax.w = static_cast<uint16_t>(res);
    m.cpu.dx.w = static_cast<uint16_t>(res >> 16);
    bool cf = (res != static_cast<int16_t>(res));
    m.cpu.cf = cf ? 1 : 0;
    m.cpu.of = cf ? 1 : 0;
}

inline void op_imul32(Machine& m, uint32_t val) {
    int64_t res = static_cast<int64_t>(static_cast<int32_t>(m.cpu.ax.e)) * static_cast<int32_t>(val);
    m.cpu.ax.e = static_cast<uint32_t>(res);
    m.cpu.dx.e = static_cast<uint32_t>(res >> 32);
    bool cf = (res != static_cast<int32_t>(res));
    m.cpu.cf = cf ? 1 : 0;
    m.cpu.of = cf ? 1 : 0;
}

inline void op_div8(Machine& m, uint8_t val) {
    if (val == 0) {
        m.interrupt(0, m.cpu.cs, 0); // Divide by zero
        return;
    }
    uint16_t num = m.cpu.ax.w;
    uint16_t quot = num / val;
    uint8_t rem = static_cast<uint8_t>(num % val);
    if (quot > 0xFF) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    m.cpu.ax.b.l = static_cast<uint8_t>(quot);
    m.cpu.ax.b.h = rem;
}

inline void op_div16(Machine& m, uint16_t val) {
    if (val == 0) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    uint32_t num = (static_cast<uint32_t>(m.cpu.dx.w) << 16) | m.cpu.ax.w;
    uint32_t quot = num / val;
    uint16_t rem = static_cast<uint16_t>(num % val);
    if (quot > 0xFFFF) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    m.cpu.ax.w = static_cast<uint16_t>(quot);
    m.cpu.dx.w = rem;
}

inline void op_div32(Machine& m, uint32_t val) {
    if (val == 0) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    uint64_t num = (static_cast<uint64_t>(m.cpu.dx.e) << 32) | m.cpu.ax.e;
    uint64_t quot = num / val;
    uint32_t rem = static_cast<uint32_t>(num % val);
    if (quot > 0xFFFFFFFFu) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    m.cpu.ax.e = static_cast<uint32_t>(quot);
    m.cpu.dx.e = rem;
}

inline void op_idiv8(Machine& m, uint8_t val) {
    if (val == 0) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    int16_t num = static_cast<int16_t>(m.cpu.ax.w);
    int8_t den = static_cast<int8_t>(val);
    int16_t quot = num / den;
    int8_t rem = static_cast<int8_t>(num % den);
    if (quot < -128 || quot > 127) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    m.cpu.ax.b.l = static_cast<uint8_t>(quot);
    m.cpu.ax.b.h = static_cast<uint8_t>(rem);
}

inline void op_idiv16(Machine& m, uint16_t val) {
    if (val == 0) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    int32_t num = static_cast<int32_t>((static_cast<uint32_t>(m.cpu.dx.w) << 16) | m.cpu.ax.w);
    int16_t den = static_cast<int16_t>(val);
    int32_t quot = num / den;
    int16_t rem = static_cast<int16_t>(num % den);
    if (quot < -32768 || quot > 32767) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    m.cpu.ax.w = static_cast<uint16_t>(quot);
    m.cpu.dx.w = static_cast<uint16_t>(rem);
}

inline void op_idiv32(Machine& m, uint32_t val) {
    if (val == 0) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    int64_t num = static_cast<int64_t>((static_cast<uint64_t>(m.cpu.dx.e) << 32) | m.cpu.ax.e);
    int32_t den = static_cast<int32_t>(val);
    int64_t quot = num / den;
    int32_t rem = static_cast<int32_t>(num % den);
    if (quot < -2147483648LL || quot > 2147483647LL) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    m.cpu.ax.e = static_cast<uint32_t>(quot);
    m.cpu.dx.e = static_cast<uint32_t>(rem);
}

inline void op_aam(Machine& m, uint8_t base) {
    if (base == 0) {
        m.interrupt(0, m.cpu.cs, 0);
        return;
    }
    uint8_t al = m.cpu.ax.b.l;
    m.cpu.ax.b.h = al / base;
    m.cpu.ax.b.l = al % base;
    m.cpu.zf = (m.cpu.ax.b.l == 0) ? 1 : 0;
    m.cpu.sf = sign_flag<uint8_t>(m.cpu.ax.b.l);
    m.cpu.pf = parity8(m.cpu.ax.b.l);
}

inline void op_aad(Machine& m, uint8_t base) {
    m.cpu.ax.b.l = static_cast<uint8_t>(m.cpu.ax.b.l + m.cpu.ax.b.h * base);
    m.cpu.ax.b.h = 0;
    m.cpu.zf = (m.cpu.ax.b.l == 0) ? 1 : 0;
    m.cpu.sf = sign_flag<uint8_t>(m.cpu.ax.b.l);
    m.cpu.pf = parity8(m.cpu.ax.b.l);
}

// ---------------------------------------------------------------------------
// String Operations (MOVS, STOS, LODS, CMPS, SCAS, INS, OUTS)
// ---------------------------------------------------------------------------
template <typename T>
inline void str_movs(Machine& m, uint16_t sseg, int rep, bool a32) {
    auto step = [&]() {
        uint32_t si = a32 ? m.cpu.si.e : m.cpu.si.w;
        uint32_t di = a32 ? m.cpu.di.e : m.cpu.di.w;
        T val;
        if constexpr (sizeof(T) == 1) val = rd8(m, sseg, si);
        else if constexpr (sizeof(T) == 2) val = rd16(m, sseg, si);
        else val = rd32(m, sseg, si);

        if constexpr (sizeof(T) == 1) wr8(m, m.cpu.es, di, val);
        else if constexpr (sizeof(T) == 2) wr16(m, m.cpu.es, di, val);
        else wr32(m, m.cpu.es, di, val);

        int delta = m.cpu.df ? -static_cast<int>(sizeof(T)) : static_cast<int>(sizeof(T));
        if (a32) {
            m.cpu.si.e += delta;
            m.cpu.di.e += delta;
        } else {
            m.cpu.si.w += delta;
            m.cpu.di.w += delta;
        }
    };

    if (rep) {
        while ((a32 ? m.cpu.cx.e : m.cpu.cx.w) != 0) {
            step();
            if (a32) m.cpu.cx.e--;
            else m.cpu.cx.w--;
        }
    } else {
        step();
    }
}

template <typename T>
inline void str_stos(Machine& m, int rep, bool a32) {
    auto step = [&]() {
        uint32_t di = a32 ? m.cpu.di.e : m.cpu.di.w;
        T val;
        if constexpr (sizeof(T) == 1) val = m.cpu.ax.b.l;
        else if constexpr (sizeof(T) == 2) val = m.cpu.ax.w;
        else val = m.cpu.ax.e;

        if constexpr (sizeof(T) == 1) wr8(m, m.cpu.es, di, val);
        else if constexpr (sizeof(T) == 2) wr16(m, m.cpu.es, di, val);
        else wr32(m, m.cpu.es, di, val);

        int delta = m.cpu.df ? -static_cast<int>(sizeof(T)) : static_cast<int>(sizeof(T));
        if (a32) m.cpu.di.e += delta;
        else m.cpu.di.w += delta;
    };

    if (rep) {
        while ((a32 ? m.cpu.cx.e : m.cpu.cx.w) != 0) {
            step();
            if (a32) m.cpu.cx.e--;
            else m.cpu.cx.w--;
        }
    } else {
        step();
    }
}

template <typename T>
inline void str_lods(Machine& m, uint16_t sseg, int rep, bool a32) {
    auto step = [&]() {
        uint32_t si = a32 ? m.cpu.si.e : m.cpu.si.w;
        T val;
        if constexpr (sizeof(T) == 1) {
            val = rd8(m, sseg, si);
            m.cpu.ax.b.l = val;
        } else if constexpr (sizeof(T) == 2) {
            val = rd16(m, sseg, si);
            m.cpu.ax.w = val;
        } else {
            val = rd32(m, sseg, si);
            m.cpu.ax.e = val;
        }

        int delta = m.cpu.df ? -static_cast<int>(sizeof(T)) : static_cast<int>(sizeof(T));
        if (a32) m.cpu.si.e += delta;
        else m.cpu.si.w += delta;
    };

    if (rep) {
        while ((a32 ? m.cpu.cx.e : m.cpu.cx.w) != 0) {
            step();
            if (a32) m.cpu.cx.e--;
            else m.cpu.cx.w--;
        }
    } else {
        step();
    }
}

template <typename T>
inline void str_cmps(Machine& m, uint16_t sseg, int rep, bool a32) {
    auto step = [&]() {
        uint32_t si = a32 ? m.cpu.si.e : m.cpu.si.w;
        uint32_t di = a32 ? m.cpu.di.e : m.cpu.di.w;
        T a, b;
        if constexpr (sizeof(T) == 1) { a = rd8(m, sseg, si); b = rd8(m, m.cpu.es, di); }
        else if constexpr (sizeof(T) == 2) { a = rd16(m, sseg, si); b = rd16(m, m.cpu.es, di); }
        else { a = rd32(m, sseg, si); b = rd32(m, m.cpu.es, di); }

        op_sub<T>(m.cpu, a, b);

        int delta = m.cpu.df ? -static_cast<int>(sizeof(T)) : static_cast<int>(sizeof(T));
        if (a32) { m.cpu.si.e += delta; m.cpu.di.e += delta; }
        else { m.cpu.si.w += delta; m.cpu.di.w += delta; }
    };

    if (rep == 1) { // REPE / REPZ
        while ((a32 ? m.cpu.cx.e : m.cpu.cx.w) != 0) {
            step();
            if (a32) m.cpu.cx.e--;
            else m.cpu.cx.w--;
            if (!m.cpu.zf) break;
        }
    } else if (rep == 2) { // REPNE / REPNZ
        while ((a32 ? m.cpu.cx.e : m.cpu.cx.w) != 0) {
            step();
            if (a32) m.cpu.cx.e--;
            else m.cpu.cx.w--;
            if (m.cpu.zf) break;
        }
    } else {
        step();
    }
}

template <typename T>
inline void str_scas(Machine& m, int rep, bool a32) {
    auto step = [&]() {
        uint32_t di = a32 ? m.cpu.di.e : m.cpu.di.w;
        T a, b;
        if constexpr (sizeof(T) == 1) { a = m.cpu.ax.b.l; b = rd8(m, m.cpu.es, di); }
        else if constexpr (sizeof(T) == 2) { a = m.cpu.ax.w; b = rd16(m, m.cpu.es, di); }
        else { a = m.cpu.ax.e; b = rd32(m, m.cpu.es, di); }

        op_sub<T>(m.cpu, a, b);

        int delta = m.cpu.df ? -static_cast<int>(sizeof(T)) : static_cast<int>(sizeof(T));
        if (a32) m.cpu.di.e += delta;
        else m.cpu.di.w += delta;
    };

    if (rep == 1) { // REPE / REPZ
        while ((a32 ? m.cpu.cx.e : m.cpu.cx.w) != 0) {
            step();
            if (a32) m.cpu.cx.e--;
            else m.cpu.cx.w--;
            if (!m.cpu.zf) break;
        }
    } else if (rep == 2) { // REPNE / REPNZ
        while ((a32 ? m.cpu.cx.e : m.cpu.cx.w) != 0) {
            step();
            if (a32) m.cpu.cx.e--;
            else m.cpu.cx.w--;
            if (m.cpu.zf) break;
        }
    } else {
        step();
    }
}

template <typename T>
inline void str_ins(Machine& m, int rep, bool a32) {
    auto step = [&]() {
        uint32_t di = a32 ? m.cpu.di.e : m.cpu.di.w;
        T val;
        if constexpr (sizeof(T) == 1) val = m.in8(m.cpu.dx.w);
        else if constexpr (sizeof(T) == 2) val = m.in16(m.cpu.dx.w);
        else val = m.in32(m.cpu.dx.w);

        if constexpr (sizeof(T) == 1) wr8(m, m.cpu.es, di, val);
        else if constexpr (sizeof(T) == 2) wr16(m, m.cpu.es, di, val);
        else wr32(m, m.cpu.es, di, val);

        int delta = m.cpu.df ? -static_cast<int>(sizeof(T)) : static_cast<int>(sizeof(T));
        if (a32) m.cpu.di.e += delta;
        else m.cpu.di.w += delta;
    };

    if (rep) {
        while ((a32 ? m.cpu.cx.e : m.cpu.cx.w) != 0) {
            step();
            if (a32) m.cpu.cx.e--;
            else m.cpu.cx.w--;
        }
    } else {
        step();
    }
}

template <typename T>
inline void str_outs(Machine& m, uint16_t sseg, int rep, bool a32) {
    auto step = [&]() {
        uint32_t si = a32 ? m.cpu.si.e : m.cpu.si.w;
        T val;
        if constexpr (sizeof(T) == 1) { val = rd8(m, sseg, si); m.out8(m.cpu.dx.w, val); }
        else if constexpr (sizeof(T) == 2) { val = rd16(m, sseg, si); m.out16(m.cpu.dx.w, val); }
        else { val = rd32(m, sseg, si); m.out32(m.cpu.dx.w, val); }

        int delta = m.cpu.df ? -static_cast<int>(sizeof(T)) : static_cast<int>(sizeof(T));
        if (a32) m.cpu.si.e += delta;
        else m.cpu.si.w += delta;
    };

    if (rep) {
        while ((a32 ? m.cpu.cx.e : m.cpu.cx.w) != 0) {
            step();
            if (a32) m.cpu.cx.e--;
            else m.cpu.cx.w--;
        }
    } else {
        step();
    }
}

}  // namespace gen
