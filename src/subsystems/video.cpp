// Video subsystem implementation for SDL2 rendering, VGA DAC, and VESA support.
#include "video.h"
#include "runtime/machine.h"
#include "runtime/settings.h"

#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace gen {

VideoSubsystem::VideoSubsystem(Machine& machine) : machine_(machine) {
    rgba_buffer_.resize(640 * 480, 0xFF000000);
    vesa_vram_.resize(1024 * 1024, 0);

    // Initialize default standard VGA 256-color palette
    for (int i = 0; i < 256; ++i) {
        palette_[i] = 0xFF000000 | (i << 16) | (i << 8) | i; // Grayscale fallback
    }
}

VideoSubsystem::~VideoSubsystem() {
    shutdown();
}

bool VideoSubsystem::init(const std::string& title) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        std::fprintf(stderr, "[SDL] Could not initialize video: %s\n", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 960,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window_) {
        std::fprintf(stderr, "[SDL] Could not create window: %s\n", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }

    title_ = title;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, Settings::get().smooth_scaling ? "linear" : "nearest");
    set_mode(VideoMode::Mode13h_320x200);
    update_title();
    apply_fullscreen();
    return true;
}

void VideoSubsystem::update_title() {
    if (!window_) return;
    std::string t = title_ + (Settings::get().keyboard_pointer ? "  [keyboard pointer on: F12 to switch off]"
                                                                : "  [F12: keyboard pointer, F11: fullscreen]");
    SDL_SetWindowTitle(window_, t.c_str());
}

void VideoSubsystem::apply_fullscreen() {
    if (!window_) return;
    SDL_SetWindowFullscreen(window_, Settings::get().fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void VideoSubsystem::shutdown() {
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

void VideoSubsystem::update_texture_size(int w, int h) {
    screen_width_ = w;
    screen_height_ = h;
    rgba_buffer_.resize(w * h, 0xFF000000);

    if (texture_) {
        SDL_DestroyTexture(texture_);
    }
    if (renderer_) {
        SDL_RenderSetLogicalSize(renderer_, w, h);
        texture_ = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            w, h
        );
    }
}

void VideoSubsystem::set_mode(VideoMode mode) {
    mode_ = mode;
    switch (mode) {
    case VideoMode::Mode13h_320x200:
        update_texture_size(320, 200);
        break;
    case VideoMode::Vesa_640x480:
        update_texture_size(640, 480);
        break;
    case VideoMode::Text:
        update_texture_size(320, 200);
        break;
    }
}

uint8_t VideoSubsystem::read_port(uint16_t port) {
    switch (port) {
    case 0x3DA: { // Input Status 1: bit 3 vertical retrace, bit 0 display disabled
        // A 70 Hz frame on the host clock, retrace in the last ~8% of it, so
        // loops that wait for the start of a retrace (TTL's scrolls and fades)
        // run at the original speed.
        static const uint64_t freq = SDL_GetPerformanceFrequency();
        const uint64_t period = freq / 70;
        const uint64_t phase = SDL_GetPerformanceCounter() % period;
        return phase >= period - period / 12 ? 0x09 : 0x00;
    }
    case 0x3C8:
        return dac_write_idx_;
    case 0x3C7:
        return dac_state_;
    case 0x3C9: { // Read DAC data: returns R, G, B (6-bit) then auto-increments the read index
        uint8_t v = dac6_[dac_read_idx_ * 3 + dac_read_state_];
        if (++dac_read_state_ == 3) {
            dac_read_state_ = 0;
            dac_read_idx_++;
        }
        return v;
    }
    default:
        return 0xFF;
    }
}

void VideoSubsystem::write_port(uint16_t port, uint8_t val) {
    switch (port) {
    case 0x3C8: // DAC Write Address Index
        dac_write_idx_ = val;
        dac_state_ = 0;
        break;
    case 0x3C7: // DAC Read Address Index
        dac_read_idx_ = val;
        dac_read_state_ = 0;
        break;
    case 0x3C9: // DAC Data Register (6-bit RGB)
        if (std::getenv("GENESIS_TRACE_DAC")) {
            // Summarise each run of DAC writes: start index, length, non-zero count.
            static uint32_t run_len = 0, run_nz = 0, run_start_idx = 0, last_t = 0, uploads = 0;
            uint32_t now = SDL_GetTicks();
            if (run_len && (now - last_t > 5 || run_len >= 768)) {
                if (++uploads <= 400 && (run_nz || uploads % 16 == 1))
                    std::fprintf(stderr, "[DAC] t=%u upload#%u start=%u len=%u nonzero=%u\n", last_t, uploads, run_start_idx, run_len, run_nz);
                run_len = run_nz = 0;
            }
            if (run_len == 0) run_start_idx = dac_write_idx_;
            ++run_len; if (val) ++run_nz; last_t = now;
        }
        dac6_[dac_write_idx_ * 3 + dac_state_] = val & 0x3F;
        if (dac_state_ == 0) {
            dac_cur_r_ = (val & 0x3F) * 255 / 63;
            dac_state_ = 1;
        } else if (dac_state_ == 1) {
            dac_cur_g_ = (val & 0x3F) * 255 / 63;
            dac_state_ = 2;
        } else if (dac_state_ == 2) {
            uint8_t b = (val & 0x3F) * 255 / 63;
            palette_[dac_write_idx_] = 0xFF000000 | (dac_cur_r_ << 16) | (dac_cur_g_ << 8) | b;
            dac_write_idx_++;
            dac_state_ = 0;
        }
        break;
    }
}

void VideoSubsystem::handle_int10() {
    uint8_t ah = machine_.cpu.ax.b.h;
    uint8_t al = machine_.cpu.ax.b.l;

    if (ah == 0x4F) {
        handle_vesa();
        return;
    }

    switch (ah) {
    case 0x00: // Set Video Mode
        if (al == 0x13) {
            set_mode(VideoMode::Mode13h_320x200);
        } else if (al == 0x03) {
            set_mode(VideoMode::Text);
        }
        break;
    case 0x0F: // Get Current Video Mode
        machine_.cpu.ax.b.l = (mode_ == VideoMode::Mode13h_320x200) ? 0x13 : 0x03;
        machine_.cpu.ax.b.h = 40; // Columns
        machine_.cpu.bx.b.h = 0;  // Active display page
        break;
    case 0x10: // Palette Functions
        if (al == 0x12) { // Set block of DAC registers
            uint16_t first_idx = machine_.cpu.bx.w;
            uint16_t count = machine_.cpu.cx.w;
            uint16_t table_seg = machine_.cpu.es;
            uint16_t table_off = machine_.cpu.dx.w;

            for (uint16_t i = 0; i < count; ++i) {
                uint8_t r = *machine_.ptr(table_seg, table_off + i * 3 + 0);
                uint8_t g = *machine_.ptr(table_seg, table_off + i * 3 + 1);
                uint8_t b = *machine_.ptr(table_seg, table_off + i * 3 + 2);
                uint8_t idx = static_cast<uint8_t>(first_idx + i);
                dac6_[idx * 3 + 0] = r & 0x3F;
                dac6_[idx * 3 + 1] = g & 0x3F;
                dac6_[idx * 3 + 2] = b & 0x3F;

                r = (r & 0x3F) * 255 / 63;
                g = (g & 0x3F) * 255 / 63;
                b = (b & 0x3F) * 255 / 63;
                palette_[idx] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
        break;
    }
}

void VideoSubsystem::set_vesa_bank(uint8_t bank) {
    if (mode_ == VideoMode::Vesa_640x480) {
        // Save current 64KB window into vesa_vram_
        uint8_t* vram_win = &machine_.ram[0xA0000];
        std::memcpy(&vesa_vram_[static_cast<size_t>(vesa_bank_) * 65536], vram_win, 65536);

        vesa_bank_ = bank;

        // Restore target bank window into 0xA000:0000
        std::memcpy(vram_win, &vesa_vram_[static_cast<size_t>(vesa_bank_) * 65536], 65536);
    } else {
        vesa_bank_ = bank;
    }
}

void VideoSubsystem::handle_vesa() {
    uint8_t al = machine_.cpu.ax.b.l;
    switch (al) {
    case 0x00: // Return VESA SuperVGA Information
    {
        uint16_t seg = machine_.cpu.es;
        uint16_t off = machine_.cpu.di.w;
        uint8_t* buf = machine_.ptr(seg, off);

        std::memcpy(buf, "VESA", 4);
        *reinterpret_cast<uint16_t*>(buf + 4) = 0x0200; // VBE 2.0
        *reinterpret_cast<uint32_t*>(buf + 6) = 0;      // OEM string
        *reinterpret_cast<uint32_t*>(buf + 10) = 0;     // Capabilities
        *reinterpret_cast<uint16_t*>(buf + 18) = 16;    // 1MB total memory (16 * 64KB)

        machine_.cpu.ax.w = 0x004F; // VESA Success
        break;
    }
    case 0x01: // Return VESA Mode Information
    {
        uint16_t mode = machine_.cpu.cx.w;
        uint16_t seg = machine_.cpu.es;
        uint16_t off = machine_.cpu.di.w;
        uint8_t* buf = machine_.ptr(seg, off);
        std::memset(buf, 0, 256);

        if ((mode & 0x1FF) == 0x101) { // 640x480 256 colors
            *reinterpret_cast<uint16_t*>(buf + 0) = 0x001B; // Mode attributes
            buf[2] = 0x07; // Window A attributes (Read/Write)
            *reinterpret_cast<uint16_t*>(buf + 4) = 64;   // Granularity 64KB
            *reinterpret_cast<uint16_t*>(buf + 6) = 64;   // Window size 64KB
            *reinterpret_cast<uint16_t*>(buf + 8) = 0xA000; // Window A segment
            *reinterpret_cast<uint16_t*>(buf + 16) = 640;  // Bytes per scanline
            *reinterpret_cast<uint16_t*>(buf + 18) = 640;  // X resolution
            *reinterpret_cast<uint16_t*>(buf + 20) = 480;  // Y resolution
            buf[25] = 8; // Bits per pixel
            buf[27] = 4; // Memory model (Packed Pixel)
        }
        machine_.cpu.ax.w = 0x004F;
        break;
    }
    case 0x02: // Set VESA Video Mode
    {
        uint16_t mode = machine_.cpu.bx.w & 0x1FF;
        if (mode == 0x101) {
            set_mode(VideoMode::Vesa_640x480);
            machine_.cpu.ax.w = 0x004F;
        } else {
            machine_.cpu.ax.w = 0x014F; // Failed
        }
        break;
    }
    case 0x05: // CPU Video Memory Window Control (Bank Switching)
    {
        uint8_t bh = machine_.cpu.bx.b.h;
        if (bh == 0) { // Set window / bank
            set_vesa_bank(static_cast<uint8_t>(machine_.cpu.dx.w));
            machine_.cpu.ax.w = 0x004F;
        } else { // Get window / bank
            machine_.cpu.dx.w = vesa_bank_;
            machine_.cpu.ax.w = 0x004F;
        }
        break;
    }
    default:
        machine_.cpu.ax.w = 0x014F;
        break;
    }
}

void VideoSubsystem::present_frame() {
    if (rgba_buffer_.empty()) return;

    if (mode_ == VideoMode::Mode13h_320x200) {
        // Mode 13h: VRAM at 0xA000:0000 (0xA0000), 64,000 bytes (320x200)
        const uint8_t* vram = &machine_.ram[0xA0000];
        for (int i = 0; i < 320 * 200; ++i) {
            rgba_buffer_[i] = palette_[vram[i]];
        }
    } else if (mode_ == VideoMode::Vesa_640x480) {
        // Flush active window into vesa_vram_
        const uint8_t* vram_win = &machine_.ram[0xA0000];
        std::memcpy(&vesa_vram_[static_cast<size_t>(vesa_bank_) * 65536], vram_win, 65536);

        // Convert full 640x480 screen from dedicated VESA VRAM
        for (int i = 0; i < 640 * 480; ++i) {
            rgba_buffer_[i] = palette_[vesa_vram_[i]];
        }
    }

    // Debug: GENESIS_SHOT=<dir> dumps the framebuffer as BMP roughly every 2 seconds.
    static const char* shot_dir = std::getenv("GENESIS_SHOT");
    if (shot_dir) {
        static uint32_t last_shot = 0;
        static int shot_idx = 0;
        uint32_t now = SDL_GetTicks();
        static const uint32_t every = std::getenv("GENESIS_SHOT_MS") ? std::atoi(std::getenv("GENESIS_SHOT_MS")) : 2000;
        if (now - last_shot >= every) {
            last_shot = now;
            SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
                rgba_buffer_.data(), screen_width_, screen_height_, 32,
                screen_width_ * 4, SDL_PIXELFORMAT_ARGB8888);
            if (s) {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/shot_%04d.bmp", shot_dir, shot_idx++);
                SDL_SaveBMP(s, path);
                SDL_FreeSurface(s);
                // Raw dump: palette (256 x ARGB32) followed by the indexed framebuffer.
                std::snprintf(path, sizeof(path), "%s/shot_%04d.raw", shot_dir, shot_idx - 1);
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(palette_, sizeof(palette_), 1, f);
                    const uint8_t* idx = (mode_ == VideoMode::Vesa_640x480) ? vesa_vram_.data()
                                                                            : &machine_.ram[0xA0000];
                    std::fwrite(idx, 1, static_cast<size_t>(screen_width_) * screen_height_, f);
                    std::fclose(f);
                }
            }
        }
    }

    if (!renderer_ || !texture_) return;
    SDL_UpdateTexture(texture_, nullptr, rgba_buffer_.data(), screen_width_ * sizeof(uint32_t));
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
}

void VideoSubsystem::set_palette_entry(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    palette_[index] = 0xFF000000 | (r << 16) | (g << 8) | b;
}

void VideoSubsystem::set_palette_rgb(const uint8_t* rgb768, bool is_6bit) {
    for (int i = 0; i < 256; ++i) {
        uint8_t r = rgb768[i * 3 + 0];
        uint8_t g = rgb768[i * 3 + 1];
        uint8_t b = rgb768[i * 3 + 2];
        if (is_6bit) {
            r = static_cast<uint8_t>((r * 255) / 63);
            g = static_cast<uint8_t>((g * 255) / 63);
            b = static_cast<uint8_t>((b * 255) / 63);
        }
        palette_[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
        dac6_[i * 3 + 0] = static_cast<uint8_t>(r >> 2);
        dac6_[i * 3 + 1] = static_cast<uint8_t>(g >> 2);
        dac6_[i * 3 + 2] = static_cast<uint8_t>(b >> 2);
    }
}

void VideoSubsystem::display_frame_640x480(const uint8_t* pixels) {
    if (mode_ != VideoMode::Vesa_640x480) {
        set_mode(VideoMode::Vesa_640x480);
    }
    std::memcpy(vesa_vram_.data(), pixels, 640 * 480);
    for (int i = 0; i < 640 * 480; ++i) {
        rgba_buffer_[i] = palette_[pixels[i]];
    }
    if (renderer_ && texture_) {
        SDL_UpdateTexture(texture_, nullptr, rgba_buffer_.data(), screen_width_ * sizeof(uint32_t));
        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }

    static const char* shot_dir = std::getenv("GENESIS_SHOT");
    if (shot_dir) {
        static uint32_t last_shot = 0;
        static int shot_idx = 0;
        uint32_t now = SDL_GetTicks();
        if (now - last_shot >= 800) {
            last_shot = now;
            char path[512];
            std::snprintf(path, sizeof(path), "%s/shot_boot_%04d.bmp", shot_dir, shot_idx++);
            SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
                rgba_buffer_.data(), 640, 480, 32, 640 * sizeof(uint32_t), SDL_PIXELFORMAT_ARGB8888);
            if (surf) {
                SDL_SaveBMP(surf, path);
                SDL_FreeSurface(surf);
                std::fprintf(stderr, "[VIDEO] Saved screenshot: %s\n", path);
            }
        }
    }
}

}  // namespace gen
