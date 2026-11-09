// Korean Johab font engine and Variable-Width Font (VWF) implementation.
#include "font_vwf.h"

#include <fstream>
#include <cstring>
#include <iostream>

namespace gen {

static const uint8_t kChoMap[32] = {
    0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t kJungMap[32] = {
    0, 0, 0, 1, 2, 3, 4, 5, 0, 0, 6, 7, 8, 9, 10, 11, 0, 0, 12, 13, 14, 15, 16, 17, 0, 0, 18, 19, 20, 21, 0, 0
};
static const uint8_t kJongMap[32] = {
    0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 0, 0
};

static const uint8_t kJungSetNoJongByCho[20] = {
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1
};
static const uint8_t kJungSetJongByCho[20] = {
    0, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 3, 3, 3
};
static const uint8_t kJongSetByJung[22] = {
    0, 0, 2, 0, 2, 1, 2, 1, 2, 3, 0, 2, 1, 3, 3, 1, 2, 1, 3, 3, 1, 1
};
static const uint8_t kChoSetNoJongByJung[22] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 3, 3, 1, 2, 4, 4, 4, 2, 1, 3, 0
};
static const uint8_t kChoSetJongByJung[22] = {
    0, 5, 5, 5, 5, 5, 5, 5, 5, 6, 7, 7, 7, 6, 6, 7, 7, 7, 6, 6, 7, 5
};

FontVwfSubsystem::FontVwfSubsystem() {
    init_default_vwf_widths();
}

FontVwfSubsystem::~FontVwfSubsystem() = default;

void FontVwfSubsystem::init_default_vwf_widths() {
    for (int i = 0; i < 256; ++i) {
        vwf_widths_[i] = 8; // Default fixed 8px
    }

    vwf_widths_[' '] = 4;
    vwf_widths_['!'] = 3;
    vwf_widths_['"'] = 4;
    vwf_widths_['\''] = 3;
    vwf_widths_['('] = 4;
    vwf_widths_[')'] = 4;
    vwf_widths_[','] = 3;
    vwf_widths_['-'] = 5;
    vwf_widths_['.'] = 3;
    vwf_widths_['/'] = 6;
    vwf_widths_[':'] = 3;
    vwf_widths_[';'] = 3;
    vwf_widths_['I'] = 4;
    vwf_widths_['i'] = 3;
    vwf_widths_['j'] = 4;
    vwf_widths_['l'] = 3;
    vwf_widths_['m'] = 8;
    vwf_widths_['M'] = 8;
    vwf_widths_['w'] = 8;
    vwf_widths_['W'] = 8;
    vwf_widths_['r'] = 5;
    vwf_widths_['t'] = 5;
    vwf_widths_['f'] = 5;
}

bool FontVwfSubsystem::load_fonts(const std::string& hangul_path, const std::string& ascii_path) {
    std::ifstream hfile(hangul_path, std::ios::binary);
    if (hfile.is_open()) {
        hangul_fnt_.assign(std::istreambuf_iterator<char>(hfile), std::istreambuf_iterator<char>());
    }

    std::ifstream afile(ascii_path, std::ios::binary);
    if (afile.is_open()) {
        ascii_fnt_.assign(std::istreambuf_iterator<char>(afile), std::istreambuf_iterator<char>());
    }

    return (!hangul_fnt_.empty() && !ascii_fnt_.empty());
}

bool FontVwfSubsystem::compose_johab(uint8_t b1, uint8_t b2, uint8_t* out_bitmap_32b) const {
    if (hangul_fnt_.size() < 11520) {
        return false;
    }

    uint8_t c = (b1 & 0x7C) >> 2;
    uint8_t j = ((b1 & 0x03) << 3) | (b2 >> 5);
    uint8_t k = b2 & 0x1F;

    uint8_t cho = kChoMap[c];
    uint8_t jung = kJungMap[j];
    uint8_t jong = kJongMap[k];

    uint8_t cho_set = 0;
    uint8_t jung_set = 0;
    uint8_t jong_set = 0;

    if (jong == 0) {
        cho_set = kChoSetNoJongByJung[jung];
        jung_set = kJungSetNoJongByCho[cho];
    } else {
        cho_set = kChoSetJongByJung[jung];
        jung_set = kJungSetJongByCho[cho];
    }
    jong_set = kJongSetByJung[jung];

    std::memset(out_bitmap_32b, 0, 32);

    // Chosung: 8 sets * 20 * 32B at offset 0
    if (cho != 0) {
        uint32_t cho_off = static_cast<uint32_t>(cho_set) * (20 * 32) + static_cast<uint32_t>(cho) * 32;
        if (cho_off + 32 <= hangul_fnt_.size()) {
            for (int i = 0; i < 32; ++i) out_bitmap_32b[i] |= hangul_fnt_[cho_off + i];
        }
    }

    // Jungsung: 4 sets * 22 * 32B at offset 0x1400 (5120)
    if (jung != 0) {
        uint32_t jung_off = 5120 + static_cast<uint32_t>(jung_set) * (22 * 32) + static_cast<uint32_t>(jung) * 32;
        if (jung_off + 32 <= hangul_fnt_.size()) {
            for (int i = 0; i < 32; ++i) out_bitmap_32b[i] |= hangul_fnt_[jung_off + i];
        }
    }

    // Jongsung: 4 sets * 28 * 32B at offset 0x1F00 (7936)
    if (jong != 0) {
        uint32_t jong_off = 7936 + static_cast<uint32_t>(jong_set) * (28 * 32) + static_cast<uint32_t>(jong) * 32;
        if (jong_off + 32 <= hangul_fnt_.size()) {
            for (int i = 0; i < 32; ++i) out_bitmap_32b[i] |= hangul_fnt_[jong_off + i];
        }
    }

    return true;
}

uint8_t FontVwfSubsystem::get_glyph_width(uint8_t ch) const {
    if (!vwf_enabled_) return 8;
    return vwf_widths_[ch];
}

int FontVwfSubsystem::measure_string_width(const std::string& str) const {
    int w = 0;
    size_t i = 0;
    while (i < str.size()) {
        uint8_t b1 = static_cast<uint8_t>(str[i]);
        if (b1 >= 0x80) {
            w += 16;
            i += 2;
        } else {
            w += get_glyph_width(b1);
            i += 1;
        }
    }
    return w;
}

void FontVwfSubsystem::load_translation(const std::string& json_path) {
    (void)json_path;
}

const std::string* FontVwfSubsystem::lookup_translation(const std::string& original_johab) const {
    auto it = translations_.find(original_johab);
    if (it != translations_.end()) {
        return &it->second;
    }
    return nullptr;
}

}  // namespace gen
