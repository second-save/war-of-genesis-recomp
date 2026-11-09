// x86 arithmetic logic unit (ALU) operations and flag emulation.
#pragma once

#include "cpu.h"
#include <cstdint>
#include <type_traits>

namespace gen {

inline uint8_t parity8(uint8_t v) {
    // Parity flag is 1 if number of set bits in lowest 8 bits is even
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (~v) & 1;
}

template <typename T>
inline uint8_t sign_flag(T val) {
    constexpr int shift = sizeof(T) * 8 - 1;
    return static_cast<uint8_t>((val >> shift) & 1);
}

// ---------------------------------------------------------------------------
// ADD / ADC
// ---------------------------------------------------------------------------
template <typename T>
inline T op_add(Cpu& c, T a, T b) {
    using UT = typename std::make_unsigned<T>::type;
    using Wide = typename std::conditional<sizeof(T) == 1, uint16_t,
                 typename std::conditional<sizeof(T) == 2, uint32_t, uint64_t>::type>::type;

    Wide res = static_cast<Wide>(static_cast<UT>(a)) + static_cast<Wide>(static_cast<UT>(b));
    T r = static_cast<T>(res);

    constexpr int bits = sizeof(T) * 8;
    c.cf = static_cast<uint8_t>((res >> bits) & 1);
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = (((a ^ b ^ r) & 0x10) != 0) ? 1 : 0;
    c.of = (((a ^ r) & (b ^ r) & (static_cast<UT>(1) << (bits - 1))) != 0) ? 1 : 0;
    return r;
}

template <typename T>
inline T op_adc(Cpu& c, T a, T b) {
    using UT = typename std::make_unsigned<T>::type;
    using Wide = typename std::conditional<sizeof(T) == 1, uint16_t,
                 typename std::conditional<sizeof(T) == 2, uint32_t, uint64_t>::type>::type;

    Wide carry = c.cf;
    Wide res = static_cast<Wide>(static_cast<UT>(a)) + static_cast<Wide>(static_cast<UT>(b)) + carry;
    T r = static_cast<T>(res);

    constexpr int bits = sizeof(T) * 8;
    c.cf = static_cast<uint8_t>((res >> bits) & 1);
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = (((a ^ b ^ r) & 0x10) != 0) ? 1 : 0;
    c.of = (((a ^ r) & (b ^ r) & (static_cast<UT>(1) << (bits - 1))) != 0) ? 1 : 0;
    return r;
}

// ---------------------------------------------------------------------------
// SUB / SBB
// ---------------------------------------------------------------------------
template <typename T>
inline T op_sub(Cpu& c, T a, T b) {
    using UT = typename std::make_unsigned<T>::type;
    using Wide = typename std::conditional<sizeof(T) == 1, uint16_t,
                 typename std::conditional<sizeof(T) == 2, uint32_t, uint64_t>::type>::type;

    Wide res = static_cast<Wide>(static_cast<UT>(a)) - static_cast<Wide>(static_cast<UT>(b));
    T r = static_cast<T>(res);

    constexpr int bits = sizeof(T) * 8;
    c.cf = static_cast<uint8_t>((res >> bits) & 1);
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = (((a ^ b ^ r) & 0x10) != 0) ? 1 : 0;
    c.of = (((a ^ b) & (a ^ r) & (static_cast<UT>(1) << (bits - 1))) != 0) ? 1 : 0;
    return r;
}

template <typename T>
inline T op_sbb(Cpu& c, T a, T b) {
    using UT = typename std::make_unsigned<T>::type;
    using Wide = typename std::conditional<sizeof(T) == 1, uint16_t,
                 typename std::conditional<sizeof(T) == 2, uint32_t, uint64_t>::type>::type;

    Wide borrow = c.cf;
    Wide res = static_cast<Wide>(static_cast<UT>(a)) - static_cast<Wide>(static_cast<UT>(b)) - borrow;
    T r = static_cast<T>(res);

    constexpr int bits = sizeof(T) * 8;
    c.cf = static_cast<uint8_t>((res >> bits) & 1);
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = (((a ^ b ^ r) & 0x10) != 0) ? 1 : 0;
    c.of = (((a ^ b) & (a ^ r) & (static_cast<UT>(1) << (bits - 1))) != 0) ? 1 : 0;
    return r;
}

// ---------------------------------------------------------------------------
// AND / OR / XOR
// ---------------------------------------------------------------------------
template <typename T>
inline T op_and(Cpu& c, T a, T b) {
    T r = a & b;
    c.cf = 0;
    c.of = 0;
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = 0;
    return r;
}

template <typename T>
inline T op_or(Cpu& c, T a, T b) {
    T r = a | b;
    c.cf = 0;
    c.of = 0;
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = 0;
    return r;
}

template <typename T>
inline T op_xor(Cpu& c, T a, T b) {
    T r = a ^ b;
    c.cf = 0;
    c.of = 0;
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = 0;
    return r;
}

// ---------------------------------------------------------------------------
// INC / DEC / NEG
// ---------------------------------------------------------------------------
template <typename T>
inline T op_inc(Cpu& c, T a) {
    using UT = typename std::make_unsigned<T>::type;
    T r = static_cast<T>(a + 1);
    constexpr int bits = sizeof(T) * 8;
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = (((a ^ 1 ^ r) & 0x10) != 0) ? 1 : 0;
    c.of = (a == static_cast<UT>((static_cast<UT>(1) << (bits - 1)) - 1)) ? 1 : 0;
    return r;
}

template <typename T>
inline T op_dec(Cpu& c, T a) {
    using UT = typename std::make_unsigned<T>::type;
    T r = static_cast<T>(a - 1);
    constexpr int bits = sizeof(T) * 8;
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = (((a ^ 1 ^ r) & 0x10) != 0) ? 1 : 0;
    c.of = (a == static_cast<UT>(static_cast<UT>(1) << (bits - 1))) ? 1 : 0;
    return r;
}

template <typename T>
inline T op_neg(Cpu& c, T a) {
    using UT = typename std::make_unsigned<T>::type;
    c.cf = (a != 0) ? 1 : 0;
    T r = static_cast<T>(-static_cast<UT>(a));
    constexpr int bits = sizeof(T) * 8;
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(r);
    c.pf = parity8(static_cast<uint8_t>(r));
    c.af = (((a ^ r) & 0x10) != 0) ? 1 : 0;
    c.of = (a == static_cast<UT>(static_cast<UT>(1) << (bits - 1))) ? 1 : 0;
    return r;
}

// ---------------------------------------------------------------------------
// SHIFTS / ROTATES
// ---------------------------------------------------------------------------
template <typename T>
inline T op_shl(Cpu& c, T a, uint8_t count) {
    using UT = typename std::make_unsigned<T>::type;
    count &= 0x1F;
    if (count == 0) return a;
    constexpr int bits = sizeof(T) * 8;
    UT val = static_cast<UT>(a);
    if (count <= bits) {
        c.cf = static_cast<uint8_t>((val >> (bits - count)) & 1);
    } else {
        c.cf = 0;
    }
    UT r = (count < bits) ? static_cast<UT>(val << count) : 0;
    if (count == 1) {
        c.of = static_cast<uint8_t>(((r >> (bits - 1)) & 1) ^ c.cf);
    }
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(static_cast<T>(r));
    c.pf = parity8(static_cast<uint8_t>(r));
    return static_cast<T>(r);
}

template <typename T>
inline T op_shr(Cpu& c, T a, uint8_t count) {
    using UT = typename std::make_unsigned<T>::type;
    count &= 0x1F;
    if (count == 0) return a;
    constexpr int bits = sizeof(T) * 8;
    UT val = static_cast<UT>(a);
    if (count <= bits) {
        c.cf = static_cast<uint8_t>((val >> (count - 1)) & 1);
    } else {
        c.cf = 0;
    }
    UT r = (count < bits) ? static_cast<UT>(val >> count) : 0;
    if (count == 1) {
        c.of = sign_flag<T>(a);
    }
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(static_cast<T>(r));
    c.pf = parity8(static_cast<uint8_t>(r));
    return static_cast<T>(r);
}

template <typename T>
inline T op_sar(Cpu& c, T a, uint8_t count) {
    using ST = typename std::make_signed<T>::type;
    using UT = typename std::make_unsigned<T>::type;
    count &= 0x1F;
    if (count == 0) return a;
    constexpr int bits = sizeof(T) * 8;
    ST val = static_cast<ST>(a);
    if (count <= bits) {
        c.cf = static_cast<uint8_t>((static_cast<UT>(val) >> (count - 1)) & 1);
    } else {
        c.cf = (val < 0) ? 1 : 0;
    }
    ST r = (count < bits) ? static_cast<ST>(val >> count) : (val < 0 ? static_cast<ST>(-1) : 0);
    if (count == 1) {
        c.of = 0;
    }
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(static_cast<T>(r));
    c.pf = parity8(static_cast<uint8_t>(r));
    return static_cast<T>(r);
}

template <typename T>
inline T op_rol(Cpu& c, T a, uint8_t count) {
    using UT = typename std::make_unsigned<T>::type;
    constexpr int bits = sizeof(T) * 8;
    count &= 0x1F;
    count %= bits;
    if (count == 0) return a;
    UT val = static_cast<UT>(a);
    UT r = static_cast<UT>((val << count) | (val >> (bits - count)));
    c.cf = static_cast<uint8_t>(r & 1);
    if (count == 1) {
        c.of = static_cast<uint8_t>(((r >> (bits - 1)) & 1) ^ c.cf);
    }
    return static_cast<T>(r);
}

template <typename T>
inline T op_ror(Cpu& c, T a, uint8_t count) {
    using UT = typename std::make_unsigned<T>::type;
    constexpr int bits = sizeof(T) * 8;
    count &= 0x1F;
    count %= bits;
    if (count == 0) return a;
    UT val = static_cast<UT>(a);
    UT r = static_cast<UT>((val >> count) | (val << (bits - count)));
    c.cf = static_cast<uint8_t>((r >> (bits - 1)) & 1);
    if (count == 1) {
        c.of = static_cast<uint8_t>(((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1);
    }
    return static_cast<T>(r);
}

template <typename T>
inline T op_rcl(Cpu& c, T a, uint8_t count) {
    using UT = typename std::make_unsigned<T>::type;
    constexpr int bits = sizeof(T) * 8;
    count &= 0x1F;
    count %= (bits + 1);
    if (count == 0) return a;
    UT val = static_cast<UT>(a);
    UT r = val;
    for (int i = 0; i < count; ++i) {
        uint8_t old_cf = c.cf;
        c.cf = static_cast<uint8_t>((r >> (bits - 1)) & 1);
        r = static_cast<UT>((r << 1) | old_cf);
    }
    if (count == 1) {
        c.of = static_cast<uint8_t>(((r >> (bits - 1)) & 1) ^ c.cf);
    }
    return static_cast<T>(r);
}

template <typename T>
inline T op_rcr(Cpu& c, T a, uint8_t count) {
    using UT = typename std::make_unsigned<T>::type;
    constexpr int bits = sizeof(T) * 8;
    count &= 0x1F;
    count %= (bits + 1);
    if (count == 0) return a;
    UT val = static_cast<UT>(a);
    UT r = val;
    for (int i = 0; i < count; ++i) {
        uint8_t old_cf = c.cf;
        c.cf = static_cast<uint8_t>(r & 1);
        r = static_cast<UT>((r >> 1) | (static_cast<UT>(old_cf) << (bits - 1)));
    }
    if (count == 1) {
        c.of = static_cast<uint8_t>(((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1);
    }
    return static_cast<T>(r);
}

template <typename T>
inline T op_shld(Cpu& c, T dst, T src, uint8_t count) {
    using UT = typename std::make_unsigned<T>::type;
    count &= 0x1F;
    if (count == 0) return dst;
    constexpr int bits = sizeof(T) * 8;
    UT d = static_cast<UT>(dst), s = static_cast<UT>(src);
    UT r = (count < bits) ? static_cast<UT>((d << count) | (s >> (bits - count))) : static_cast<UT>(s << (count - bits));
    c.cf = static_cast<uint8_t>((d >> (bits - count)) & 1);
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(static_cast<T>(r));
    c.pf = parity8(static_cast<uint8_t>(r));
    return static_cast<T>(r);
}

template <typename T>
inline T op_shrd(Cpu& c, T dst, T src, uint8_t count) {
    using UT = typename std::make_unsigned<T>::type;
    count &= 0x1F;
    if (count == 0) return dst;
    constexpr int bits = sizeof(T) * 8;
    UT d = static_cast<UT>(dst), s = static_cast<UT>(src);
    UT r = (count < bits) ? static_cast<UT>((d >> count) | (s << (bits - count))) : static_cast<UT>(s >> (count - bits));
    c.cf = static_cast<uint8_t>((d >> (count - 1)) & 1);
    c.zf = (r == 0) ? 1 : 0;
    c.sf = sign_flag<T>(static_cast<T>(r));
    c.pf = parity8(static_cast<uint8_t>(r));
    return static_cast<T>(r);
}

template <typename T>
inline T op_imul2(Cpu& c, T a, T b) {
    using ST = typename std::make_signed<T>::type;
    using Wide = typename std::conditional<sizeof(T) == 2, int32_t, int64_t>::type;
    Wide prod = static_cast<Wide>(static_cast<ST>(a)) * static_cast<Wide>(static_cast<ST>(b));
    T r = static_cast<T>(prod);
    bool overflow = (prod != static_cast<Wide>(static_cast<ST>(r)));
    c.cf = overflow ? 1 : 0;
    c.of = overflow ? 1 : 0;
    return r;
}

// ---------------------------------------------------------------------------
// BIT TEST / SCAN
// ---------------------------------------------------------------------------
template <typename T>
inline void op_bt(Cpu& c, T base, T bit) {
    constexpr int bits = sizeof(T) * 8;
    bit %= bits;
    c.cf = static_cast<uint8_t>((base >> bit) & 1);
}

template <typename T>
inline T op_bts(Cpu& c, T base, T bit) {
    constexpr int bits = sizeof(T) * 8;
    bit %= bits;
    c.cf = static_cast<uint8_t>((base >> bit) & 1);
    return static_cast<T>(base | (static_cast<T>(1) << bit));
}

template <typename T>
inline T op_btr(Cpu& c, T base, T bit) {
    constexpr int bits = sizeof(T) * 8;
    bit %= bits;
    c.cf = static_cast<uint8_t>((base >> bit) & 1);
    return static_cast<T>(base & ~(static_cast<T>(1) << bit));
}

template <typename T>
inline T op_btc(Cpu& c, T base, T bit) {
    constexpr int bits = sizeof(T) * 8;
    bit %= bits;
    c.cf = static_cast<uint8_t>((base >> bit) & 1);
    return static_cast<T>(base ^ (static_cast<T>(1) << bit));
}

template <typename T>
inline bool op_bsf(Cpu& c, T src, T& dst) {
    if (src == 0) {
        c.zf = 1;
        return false;
    }
    c.zf = 0;
    dst = static_cast<T>(__builtin_ctz(src));
    return true;
}

template <typename T>
inline bool op_bsr(Cpu& c, T src, T& dst) {
    if (src == 0) {
        c.zf = 1;
        return false;
    }
    c.zf = 0;
    constexpr int bits = sizeof(T) * 8;
    if constexpr (sizeof(T) == 2) {
        dst = static_cast<T>(bits - 1 - __builtin_clz(static_cast<uint32_t>(src) << 16));
    } else {
        dst = static_cast<T>(bits - 1 - __builtin_clz(src));
    }
    return true;
}

// ---------------------------------------------------------------------------
// BCD / DAA / DAS / AAA / AAS
// ---------------------------------------------------------------------------
inline void op_daa(Cpu& c) {
    uint8_t al = c.ax.b.l;
    uint8_t old_al = al;
    uint8_t old_cf = c.cf;
    c.cf = 0;
    if ((al & 0x0F) > 9 || c.af) {
        al += 6;
        c.af = 1;
    } else {
        c.af = 0;
    }
    if (old_al > 0x99 || old_cf) {
        al += 0x60;
        c.cf = 1;
    }
    c.ax.b.l = al;
    c.zf = (al == 0) ? 1 : 0;
    c.sf = sign_flag<uint8_t>(al);
    c.pf = parity8(al);
}

inline void op_das(Cpu& c) {
    uint8_t al = c.ax.b.l;
    uint8_t old_al = al;
    uint8_t old_cf = c.cf;
    c.cf = 0;
    if ((al & 0x0F) > 9 || c.af) {
        al -= 6;
        c.af = 1;
    } else {
        c.af = 0;
    }
    if (old_al > 0x99 || old_cf) {
        al -= 0x60;
        c.cf = 1;
    }
    c.ax.b.l = al;
    c.zf = (al == 0) ? 1 : 0;
    c.sf = sign_flag<uint8_t>(al);
    c.pf = parity8(al);
}

inline void op_aaa(Cpu& c) {
    if ((c.ax.b.l & 0x0F) > 9 || c.af) {
        c.ax.b.l = (c.ax.b.l + 6) & 0x0F;
        c.ax.b.h++;
        c.af = 1;
        c.cf = 1;
    } else {
        c.af = 0;
        c.cf = 0;
        c.ax.b.l &= 0x0F;
    }
}

inline void op_aas(Cpu& c) {
    if ((c.ax.b.l & 0x0F) > 9 || c.af) {
        c.ax.b.l = (c.ax.b.l - 6) & 0x0F;
        c.ax.b.h--;
        c.af = 1;
        c.cf = 1;
    } else {
        c.af = 0;
        c.cf = 0;
        c.ax.b.l &= 0x0F;
    }
}

}  // namespace gen
