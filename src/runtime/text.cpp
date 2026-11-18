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
#include "modules/generated/pack/pack_decl.h"
#include "modules/generated/chrsys/chrsys_decl.h"
#include "subsystems/dos.h"

#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
void far_call(Machine& m, uint16_t seg, FunctionPointer fn, std::initializer_list<uint16_t> args,
              uint16_t load_seg = gen::mod_chp::kLoadSeg) {
    std::vector<uint16_t> a(args);
    for (auto it = a.rbegin(); it != a.rend(); ++it) push16(m, *it);
    uint16_t cs = m.cpu.cs;
    push16(m, cs);
    push16(m, 0);
    m.cpu.cs = static_cast<uint16_t>(seg + load_seg);
    fn(m);
    m.cpu.cs = cs;
    m.cpu.sp.w = static_cast<uint16_t>(m.cpu.sp.w + 2 * a.size());
}

struct Glyph8 {
    uint16_t seg, off;
    int first = 0, last = -1;  // used columns (0 = leftmost); last < first = blank
};

// Text that doesn't fit the game's fixed-size records (town talk, battle
// dialogue) is replaced in the translated files by "@@XYZ" markers (three
// base-36 digits, one per line); the text comes from lang/<code>/TEXT.TXT
// ("<decimal id><TAB><text>", "\\n" = line break).
const std::string* marker_lookup(Machine& m, const std::string& line) {
    static std::unordered_map<long, std::string> table;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(m.dos->overlay_dir() / "TEXT.TXT");
        std::string l;
        while (std::getline(f, l)) {
            size_t tab = l.find('\t');
            if (tab == std::string::npos) continue;
            std::string v;
            for (size_t i = tab + 1; i < l.size(); ++i) {
                if (l[i] == '\\' && i + 1 < l.size() && l[i + 1] == 'n') {
                    v += '\n';
                    ++i;
                } else {
                    v += l[i];
                }
            }
            table[std::strtol(l.c_str(), nullptr, 10)] = v;
        }
    }
    if (line.size() < 5 || line[0] != '@' || line[1] != '@') return nullptr;
    long id = 0;
    for (int i = 2; i < 5; ++i) {
        char ch = line[i];
        int d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 10 : -1;
        if (d < 0) return nullptr;
        id = id * 36 + d;
    }
    auto it = table.find(id);
    return it == table.end() ? nullptr : &it->second;
}

// Replace every marker line of a (possibly multi-line) string.
bool expand_markers(Machine& m, std::string& text) {
    if (text.find("@@") == std::string::npos) return false;
    std::string out;
    bool any = false;
    size_t pos = 0;
    while (true) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (const std::string* t = marker_lookup(m, line)) {
            out += *t;
            any = true;
        } else {
            out += line;
        }
        if (nl == std::string::npos) break;
        out += '\n';
        pos = nl + 1;
    }
    if (any) text = out;
    return any;
}

// Short strings drawn verbatim (names, labels): lang/<code>/STRINGS.TXT holds
// "<Johab text><TAB><replacement>" lines.
const std::string* string_text(Machine& m, const std::string& s, bool* compacted = nullptr) {
    static std::unordered_map<std::string, std::string> table;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(m.dos->overlay_dir() / "STRINGS.TXT", std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            size_t tab = line.find('\t');
            if (tab != std::string::npos) table[line.substr(0, tab)] = line.substr(tab + 1);
        }
    }
    auto it = table.find(s);
    if (it != table.end()) return &it->second;
    // Names are often padded for centring (" 이 올 린", " 듀    란 "): retry
    // without spaces, keeping one leading space if the original had one.
    static std::unordered_map<std::string, std::string> compact;
    static std::unordered_map<std::string, std::string> results;
    if (compact.empty())
        for (const auto& [k, v] : table) {
            std::string ck;
            for (char ch : k) if (ch != ' ') ck += ch;
            compact.emplace(ck, v);
        }
    std::string ck;
    for (char ch : s) if (ch != ' ') ck += ch;
    auto ct = compact.find(ck);
    if (ct == compact.end() || ck.size() == s.size()) return nullptr;
    std::string r = (!s.empty() && s[0] == ' ' ? " " : "") + ct->second;
    if (compacted) *compacted = true;
    return &(results[s] = r);
}

// GENESIS_TRACE_UNTRANSLATED=1: log each distinct Korean string drawn without
// a translation (to find what is left to translate).
void log_untranslated(const std::string& text) {
    static const bool on = std::getenv("GENESIS_TRACE_UNTRANSLATED") != nullptr;
    if (!on) return;
    bool hangul = false;
    for (unsigned char ch : text) hangul |= ch > 0x7F;
    static std::unordered_set<std::string> seen;
    if (hangul && seen.insert(text).second) {
        std::string esc;
        for (char ch : text) esc += ch == '\n' ? std::string("\\n") : std::string(1, ch);
        std::fprintf(stderr, "[UNTR] %s\n", esc.c_str());
    }
}

// Read a null-terminated string and apply the translation tables.
// A replacement starting with '^' (strings.tsv) is centred on the Korean
// string's width even when it is wider: fields where the game centres the
// name itself (item names in equipment slots).
std::string translated_string(Machine& m, uint16_t seg, uint16_t off, size_t* padded_width = nullptr,
                              bool* centre = nullptr) {
    std::string text;
    for (uint16_t p = off; text.size() < 2048; ++p) {
        uint8_t ch = rd8(m, seg, p);
        if (!ch) break;
        text += static_cast<char>(ch);
    }
    if (expand_markers(m, text)) return text;
    bool compacted = false;
    if (const std::string* t = string_text(m, text, &compacted)) {
        // A name padded with spaces for centring (matched without its spaces):
        // report its original width (8 px per byte) so the caller can centre it.
        if (padded_width && compacted) *padded_width = text.size() * 8;
        if (!t->empty() && (*t)[0] == '^') {
            if (padded_width) *padded_width = text.size() * 8;
            if (centre) *centre = true;
            return t->substr(1);
        }
        return *t;
    }
    log_untranslated(text);
    return text;
}

}  // namespace

// CHP 1247:084D / 1247:0994  print_text(x, y, char far* text, right, bottom)
// Shadow at (+1,+1) in colour 0, then the glyph in the current colour [7E1C].
// Past `right` the line wraps (18 px); past `bottom` the box is shown, the game
// waits for a click (11B7:004F) and clears it (12F8:0499) before continuing.
// Shared by CHP's two typesetters, which differ only in the routine that
// clears the text area at a page break.
static void chp_typeset(Machine& m, FunctionPointer original, uint16_t clear_seg, FunctionPointer clear) {
    if (!translation_active()) {
        original(m);
        return;
    }
    using namespace gen::mod_chp;
    Cpu& c = m.cpu;
    const uint16_t sp = c.sp.w;
    auto arg = [&](int i) { return rd16(m, c.ss, static_cast<uint16_t>(sp + 4 + 2 * i)); };
    const int x0 = static_cast<int16_t>(arg(0)), y0 = static_cast<int16_t>(arg(1));
    const std::string text = translated_string(m, arg(3), arg(2));
    if (std::getenv("GENESIS_TRACE_TEXT")) std::fprintf(stderr, "[TEXT] chp '%s'\n", text.c_str());
    uint16_t str_off = 0;  // index into text
    const int right = static_cast<int16_t>(arg(4)), bottom = static_cast<int16_t>(arg(5));
    const uint16_t saved_si = c.si.w, saved_di = c.di.w, saved_bp = c.bp.w;
    const uint8_t colour = rd8(m, c.ds, 0x7E1C);

    auto ch_at = [&](uint16_t i) -> uint8_t { return i < text.size() ? static_cast<uint8_t>(text[i]) : 0; };
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
        far_call(m, clear_seg, clear, {});
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

// 1247:084D: story dialogue (VISUAL scripts); page clear 12F8:0499.
void chp_print_text(Machine& m, FunctionPointer original) {
    chp_typeset(m, original, 0x12F8, gen::mod_chp::F_12F8_0499);
}

// 1247:0994: chapter prologue pages (PE<n>); page clear 139E:03AC.
void chp_print_prologue(Machine& m, FunctionPointer original) {
    chp_typeset(m, original, 0x139E, gen::mod_chp::F_139E_03AC);
}

// draw_string(x, y, char far* text): the graphics library's text routine, linked
// into CHP (1F17:067F, map/menus), PACK (268D:0689, towns) and CHRSYS
// (3778:05F8, battles) at different addresses. Hangul composes a 16x16 glyph,
// ASCII copies 16 font bytes into a glyph descriptor {width-1, height-1,
// bitmap}; both are drawn by a helper in the current draw mode.
// With a translation active: "@@id" markers and the string table are applied,
// and ASCII is set proportionally (each glyph shifted to its first used column,
// the descriptor width set to its advance so opaque modes leave no gaps).
struct LibText {
    uint16_t seg, load_seg;
    FunctionPointer init, colour, draw, finish;
    uint16_t cur_x, cur_y;           // pen position
    uint16_t col0, col1, col2;       // colour/mode arguments to `colour`
    uint16_t desc, glyph, font;      // ASCII descriptor, its bitmap, 8x16 font
    const char* name;
};

static void lib_draw_string(Machine& m, FunctionPointer original, const LibText& L) {
    if (!translation_active()) {
        original(m);
        return;
    }
    Cpu& c = m.cpu;
    const uint16_t sp = c.sp.w;
    auto arg = [&](int i) { return rd16(m, c.ss, static_cast<uint16_t>(sp + 4 + 2 * i)); };
    const int x0 = static_cast<int16_t>(arg(0)), y0 = static_cast<int16_t>(arg(1));
    size_t padded = 0;
    bool centre = false;
    std::string text = translated_string(m, arg(3), arg(2), &padded, &centre);
    bool hangul = false;
    for (unsigned char ch : text) hangul |= ch > 0x7F;
    const uint16_t ds = c.ds;
    if (std::getenv("GENESIS_TRACE_TEXT"))
        std::fprintf(stderr, "[TEXT] %s '%s'\n", L.name, text.c_str());
    if (hangul || rd16(m, ds, L.desc) != 7 || rd16(m, ds, L.desc + 2) != 15) {
        original(m);  // Korean text, or a layout we don't know: the game's own code
        return;
    }
    const uint16_t saved_si = c.si.w, saved_di = c.di.w, saved_bp = c.bp.w;
    auto call = [&](FunctionPointer fn, std::initializer_list<uint16_t> a) { far_call(m, L.seg, fn, a, L.load_seg); };
    wr16(m, ds, L.cur_x, static_cast<uint16_t>(x0));
    wr16(m, ds, L.cur_y, static_cast<uint16_t>(y0));
    call(L.init, {});
    call(L.colour, {rd16(m, ds, L.col0), rd16(m, ds, L.col1), rd16(m, ds, L.col2)});
    // Glyph columns used: [first, last]; digits stay 8 px wide so numbers line up in tables.
    auto metrics = [&](unsigned char ch, int& first, int& last) {
        uint8_t bits = 0;
        for (int r = 0; r < 16; ++r) bits |= rd8(m, ds, static_cast<uint16_t>(L.font + ch * 16 + r));
        first = 0;
        last = -1;
        for (int col = 0; col < 8; ++col)
            if (bits & (0x80 >> col)) {
                if (last < first) first = col;
                last = col;
            }
        if (ch >= '0' && ch <= '9') {
            first = 0;
            last = 6;
        }
        return (ch == ' ' || last < first) ? 4 : (last - first + 2);
    };
    int x = x0, y = y0;
    if (padded && text.find('\n') == std::string::npos) {
        int w = 0;
        for (unsigned char ch : text) {
            int f, l;
            w += metrics(ch, f, l);
        }
        if (static_cast<int>(padded) > w || centre) x += (static_cast<int>(padded) - w) / 2;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == '\n') {
            x = x0;
            y += 16;
            continue;
        }
        if (ch == ' ') {
            // Runs of spaces line text up with fixed graphics (buttons, columns):
            // keep the original 8 px per space there; a lone space is narrow.
            bool run = (i > 0 && text[i - 1] == ' ') || (i + 1 < text.size() && text[i + 1] == ' ');
            x += run ? 8 : 4;
            continue;
        }
        int first, last;
        int adv = metrics(ch, first, last);
        for (int r = 0; r < 16; ++r)
            wr8(m, ds, static_cast<uint16_t>(L.glyph + r),
                static_cast<uint8_t>(rd8(m, ds, static_cast<uint16_t>(L.font + ch * 16 + r)) << first));
        // Stored as width - 1. The bitmap is one byte per row, so a glyph using all
        // 8 columns (advance 9) keeps an 8 px box: a 9 px box makes the helper
        // read 2 bytes per row and scrambles the glyph (the "T" of "Time Interval").
        wr16(m, ds, L.desc, static_cast<uint16_t>(std::min(adv, 8) - 1));
        call(L.draw, {static_cast<uint16_t>(x), static_cast<uint16_t>(y), L.desc, ds});
        x += adv;
    }
    wr16(m, ds, L.desc, 7);
    wr16(m, ds, L.cur_x, static_cast<uint16_t>(x));
    wr16(m, ds, L.cur_y, static_cast<uint16_t>(y));
    call(L.finish, {});
    c.si.w = saved_si;
    c.di.w = saved_di;
    c.bp.w = saved_bp;
    (void)pop16(m);
    c.cs = pop16(m);
}

void pack_draw_string(Machine& m, FunctionPointer original) {
    using namespace gen::mod_pack;
    static const LibText L{0x268D, kLoadSeg, F_268D_0405, F_268D_0363, F_268D_048D, F_268D_0443,
                           0x9D52, 0x9D50, 0x9D4A, 0x9D48, 0x9D46, 0x9D54, 0x9D58, 0x9D8C, "pack"};
    lib_draw_string(m, original, L);
}

void chp_draw_string(Machine& m, FunctionPointer original) {
    using namespace gen::mod_chp;
    static const LibText L{0x1F17, kLoadSeg, F_1F17_03FB, F_1F17_0359, F_1F17_0483, F_1F17_0439,
                           0x94EA, 0x94E8, 0x94E2, 0x94E0, 0x94DE, 0x94EC, 0x94F0, 0x9524, "chp"};
    lib_draw_string(m, original, L);
}

void chrsys_draw_string(Machine& m, FunctionPointer original) {
    using namespace gen::mod_chrsys;
    static const LibText L{0x3778, kLoadSeg, F_3778_03C4, F_3778_034C, F_3778_0447, F_3778_0403,
                           0x83D0, 0x83CE, 0x83C8, 0x83C6, 0x83C4, 0x83D2, 0x83D6, 0x840A, "chrsys"};
    lib_draw_string(m, original, L);
}

// TTL 000107C3  draw_string(x = eax, y = edx, text = ebx), Watcom register
// call: Hangul through 100AB/105B8 (16 px), ASCII through 101DC (glyph
// address) and 10531 (8x16, set pixels only). Translated strings are drawn
// proportionally with the same ASCII routines, centred in the Korean width
// (the menus pad their entries with spaces for centring).
void ttl_draw_string(Machine& m, FunctionPointer original) {
    if (!translation_active()) {
        original(m);
        return;
    }
    Cpu& c = m.cpu;
    std::string ko;
    for (uint32_t p = c.bx.e; ko.size() < 256; ++p) {
        uint8_t ch = rd8(m, c.ds, p);
        if (!ch) break;
        ko += static_cast<char>(ch);
    }
    const std::string* t = string_text(m, ko);
    if (!t) {
        log_untranslated(ko);
        original(m);
        return;
    }
    std::string en = *t;
    if (!en.empty() && en[0] == '^') en.erase(0, 1);
    en.erase(0, en.find_first_not_of(' '));
    if (std::getenv("GENESIS_TRACE_TEXT")) std::fprintf(stderr, "[TEXT] ttl '%s'\n", en.c_str());
    const uint32_t saved_cx = c.cx.e, saved_si = c.si.e, saved_di = c.di.e;
    const uint32_t x0 = c.ax.e, y = c.dx.e;

    struct G { uint32_t addr; int first = 8, last = -1; };
    auto glyph = [&](uint8_t ch) {
        c.ax.e = ch;
        push32(m, 0);
        m.call32(0x000101DC);
        G g{c.ax.e};
        uint8_t bits = 0;
        for (int r = 0; r < 16; ++r) bits |= rd8(m, c.ds, g.addr + r);
        for (int col = 0; col < 8; ++col)
            if (bits & (0x80 >> col)) {
                if (g.last < 0) g.first = col;
                g.last = col;
            }
        return g;
    };
    auto advance = [&](uint8_t ch) {
        if (ch == ' ') return 4;
        G g = glyph(ch);
        return g.last < 0 ? 4 : g.last - g.first + 2;
    };
    int width = 0;
    for (char ch : en) width += advance(static_cast<uint8_t>(ch));
    // Centre on the Korean text itself (entries are left-aligned and padded
    // with leading spaces to look centred).
    const size_t lead = std::min(ko.find_first_not_of(' '), ko.size());
    const size_t end = ko.find_last_not_of(' ') + 1;
    const int centre = static_cast<int>(lead * 8 + (end > lead ? (end - lead) * 8 : 0) / 2);
    uint32_t x = x0 + std::max(0, centre - width / 2);
    for (char ch : en) {
        const uint8_t u = static_cast<uint8_t>(ch);
        if (u == ' ') { x += 4; continue; }
        G g = glyph(u);
        if (g.last < 0) { x += 4; continue; }
        c.ax.e = x - g.first;
        c.dx.e = y;
        c.bx.e = g.addr;
        push32(m, 0);
        m.call32(0x00010531);
        x += g.last - g.first + 2;
    }
    c.cx.e = saved_cx;
    c.si.e = saved_si;
    c.di.e = saved_di;
    (void)pop32(m);  // ret
}

}  // namespace gen::text
