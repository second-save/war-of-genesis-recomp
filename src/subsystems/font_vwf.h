// Korean Johab font engine and Custom Variable-Width Font (VWF) translation system.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace gen {

class Machine;

struct JohabGlyph {
    uint8_t bitmap[32]; // 16x16 1bpp (2 bytes per row, 16 rows)
};

class FontVwfSubsystem {
public:
    FontVwfSubsystem();
    ~FontVwfSubsystem();

    bool load_fonts(const std::string& hangul_path, const std::string& ascii_path);

    // KSSM 5-5-5 Johab glyph composition into 16x16 bitmap
    bool compose_johab(uint8_t b1, uint8_t b2, uint8_t* out_bitmap_32b) const;

    // Custom VWF width table
    void set_vwf_enabled(bool enabled) { vwf_enabled_ = enabled; }
    bool is_vwf_enabled() const { return vwf_enabled_; }
    uint8_t get_glyph_width(uint8_t ch) const;
    int measure_string_width(const std::string& str) const;

    // Translation override table
    void load_translation(const std::string& json_path);
    const std::string* lookup_translation(const std::string& original_johab) const;

    const std::vector<uint8_t>& ascii_font() const { return ascii_fnt_; }
    const std::vector<uint8_t>& hangul_font() const { return hangul_fnt_; }

private:
    bool vwf_enabled_ = true;

    // Raw font buffers
    std::vector<uint8_t> hangul_fnt_; // 11,520 bytes (360 * 32 B)
    std::vector<uint8_t> ascii_fnt_;  // 2,048 bytes (128 * 16 B)

    // VWF widths for ASCII 0..127
    uint8_t vwf_widths_[256]{};

    // External string overrides
    std::unordered_map<std::string, std::string> translations_;

    void init_default_vwf_widths();
};

}  // namespace gen
