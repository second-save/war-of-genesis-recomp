// Native text layout hooks for translations.
//
// The games's own typesetters advance 16 px per Hangul syllable and 8 px per
// ASCII character, wrapping wherever a line runs out. That suits Korean, but
// English set in the game's 8x16 ASCII font needs proportional spacing and
// word wrap to fit the same boxes. These hooks replace the typesetters (via the
// lifter's "override" hint): with the original language they simply run the
// original code; with a translation they lay out ASCII text proportionally,
// still drawing every glyph with the game's own routines and font.
#include "machine.h"
#include "memory.h"
#include "settings.h"
#include "modules/generated/chp/chp_decl.h"

#include <cstring>
#include <vector>

namespace gen::text {

namespace {

bool translation_active() {
    const std::string& l = Settings::get().language;
    return !l.empty() && l != "ko";
}

// Far-call a lifted function in (relative) segment `seg`, with 16-bit
// arguments pushed right to left; it returns with RETF. CS must be the callee's
// segment while it runs: its own near calls resolve against it.
void far_call(Machine& m, uint16_t seg, FunctionPointer fn, std::initializer_list<uint16_t> args) {
    std::vector<uint16_t> a(args);
    for (auto it = a.rbegin(); it != a.rend(); ++it) push16(m, *it);
    uint16_t cs = m.cpu.cs;
    push16(m, cs);
    push16(m, 0);
    m.cpu.cs = static_cast<uint16_t>(seg + gen::mod_chp::kLoadSeg);
    fn(m);
    m.cpu.cs = cs;
    m.cpu.sp.w = static_cast<uint16_t>(m.cpu.sp.w + 2 * a.size());
}

struct Glyph8 {
    uint16_t seg, off;
    int first = 0, last = -1;  // used columns (0 = leftmost); last < first = blank
};

}  // namespace

// CHP 1247:084D  print_text(x, y, char far* text, right, bottom)
// Shadow at (+1,+1) in colour 0, then the glyph in the current colour [7E1C].
// Past `right` the line wraps (18 px); past `bottom` the box is shown, the game
// waits for a click (11B7:004F) and clears it (12F8:0499) before continuing.
void chp_print_text(Machine& m, FunctionPointer original) {
    if (!translation_active()) {
        original(m);
        return;
    }
    using namespace gen::mod_chp;
    Cpu& c = m.cpu;
    const uint16_t sp = c.sp.w;
    auto arg = [&](int i) { return rd16(m, c.ss, static_cast<uint16_t>(sp + 4 + 2 * i)); };
    const int x0 = static_cast<int16_t>(arg(0)), y0 = static_cast<int16_t>(arg(1));
    uint16_t str_off = arg(2);
    const uint16_t str_seg = arg(3);
    const int right = static_cast<int16_t>(arg(4)), bottom = static_cast<int16_t>(arg(5));
    const uint16_t saved_si = c.si.w, saved_di = c.di.w, saved_bp = c.bp.w;
    const uint8_t colour = rd8(m, c.ds, 0x7E1C);

    auto ch_at = [&](uint16_t off) { return rd8(m, str_seg, off); };
    auto glyph8 = [&](uint8_t ch) {
        far_call(m, 0x1247, F_1247_0220, {ch});
        Glyph8 g{c.dx.w, c.ax.w};
        uint8_t bits = 0;
        for (int r = 0; r < 16; ++r) bits |= rd8(m, g.seg, static_cast<uint16_t>(g.off + r));
        for (int col = 0; col < 8; ++col)
            if (bits & (0x80 >> col)) {
                if (g.last < g.first) g.first = col;
                g.last = col;
            }
        return g;
    };
    auto advance8 = [&](uint8_t ch) {
        if (ch == ' ') return 4;
        Glyph8 g = glyph8(ch);
        return g.last < g.first ? 4 : (g.last - g.first + 2);
    };

    int x = x0, y = y0;
    auto page_break = [&]() {
        far_call(m, 0x121A, F_121A_01D4, {});
        far_call(m, 0x11B7, F_11B7_004F, {});
        far_call(m, 0x12F8, F_12F8_0499, {});
        y = y0;
    };
    auto new_line = [&]() {
        x = x0;
        y += 18;
        if (y > bottom) page_break();
    };
    auto draw = [&](FunctionPointer put, uint16_t gseg, uint16_t goff, int px, int py) {
        far_call(m, 0x1247, F_1247_04C4, {0});
        far_call(m, 0x1247, put, {static_cast<uint16_t>(px + 1), static_cast<uint16_t>(py + 1), goff, gseg});
        far_call(m, 0x1247, F_1247_04C4, {colour});
        far_call(m, 0x1247, put, {static_cast<uint16_t>(px), static_cast<uint16_t>(py), goff, gseg});
    };

    for (;;) {
        uint8_t ch = ch_at(str_off);
        if (ch == 0) break;
        if (ch == '\n') {  // explicit line break in translated text
            new_line();
            ++str_off;
            continue;
        }
        if (ch == '\f') {  // explicit page break
            page_break();
            x = x0;
            ++str_off;
            continue;
        }
        if (ch > 0x7F) {  // Hangul syllable: unchanged from the original
            uint16_t code = static_cast<uint16_t>((ch << 8) | ch_at(static_cast<uint16_t>(str_off + 1)));
            far_call(m, 0x1247, F_1247_0104, {code});
            uint16_t gseg = c.dx.w, goff = c.ax.w;
            if (x + 16 > right + 16) new_line();
            draw(F_1247_060E, gseg, goff, x, y);
            x += 16;
            str_off = static_cast<uint16_t>(str_off + 2);
            continue;
        }
        if (ch == ' ') {
            if (x != x0) x += 4;  // no leading spaces on a wrapped line
            ++str_off;
            continue;
        }
        // An ASCII word: wrap before it if it doesn't fit.
        int w = 0;
        uint16_t e = str_off;
        while (ch_at(e) > ' ' && ch_at(e) <= 0x7F) w += advance8(ch_at(e++));
        if (x != x0 && x + w - 1 > right) new_line();
        for (uint16_t p = str_off; p != e; ++p) {
            Glyph8 g = glyph8(ch_at(p));
            if (g.last >= g.first) {
                if (x + (g.last - g.first) > right) new_line();  // a word wider than the line
                draw(F_1247_053C, g.seg, g.off, x - g.first, y);
                x += g.last - g.first + 2;
            } else {
                x += 4;
            }
        }
        str_off = e;
    }
    far_call(m, 0x121A, F_121A_01D4, {});
    far_call(m, 0x11B7, F_11B7_004F, {});

    c.si.w = saved_si;
    c.di.w = saved_di;
    c.bp.w = saved_bp;
    (void)pop16(m);
    c.cs = pop16(m);
}

}  // namespace gen::text
