// Video subsystem: SDL2 window, hardware scaler, palette DAC, and VRAM rasterizer.
#pragma once

#include <cstdint>
#include <vector>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace gen {

class Machine;

class VideoSubsystem {
public:
    enum class VideoMode {
        Text,
        Mode13h_320x200,
        Vesa_640x480,
    };

    VideoSubsystem(Machine& machine);
    ~VideoSubsystem();

    bool init(const std::string& title = "The War of Genesis (창세기전)");
    void shutdown();

    void set_mode(VideoMode mode);
    VideoMode current_mode() const { return mode_; }

    // Hardware Port I/O
    uint8_t read_port(uint16_t port);
    void write_port(uint16_t port, uint8_t val);

    // BIOS INT 10h Handler
    void handle_int10();

    // VESA INT 10h AX=4Fxxh
    void handle_vesa();

    // Frame presentation
    void present_frame();

    // Bank switching (VESA 64KB window)
    void set_vesa_bank(uint8_t bank);
    uint8_t vesa_bank() const { return vesa_bank_; }

    SDL_Window* window() const { return window_; }

    // Direct framebuffer / palette access for boot sequences and native overlays
    void set_palette_entry(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    void set_palette_rgb(const uint8_t* rgb768, bool is_6bit = false);
    void display_frame_640x480(const uint8_t* pixels);

private:
    Machine& machine_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;

    VideoMode mode_ = VideoMode::Mode13h_320x200;
    int screen_width_ = 320;
    int screen_height_ = 200;

    // 256-color palette (RGBA32)
    uint32_t palette_[256]{};
    uint8_t dac_state_ = 0; // 0=R, 1=G, 2=B
    uint8_t dac_write_idx_ = 0;
    uint8_t dac_read_idx_ = 0;
    uint8_t dac_cur_r_ = 0, dac_cur_g_ = 0;

    // VESA bank (granularity 64KB)
    uint8_t vesa_bank_ = 0;
    std::vector<uint8_t> vesa_vram_;

    // VSync flag for port 0x3DA
    uint8_t vsync_phase_ = 0;

    // 32-bit RGBA pixel conversion buffer
    std::vector<uint32_t> rgba_buffer_;

    void update_texture_size(int w, int h);
};

}  // namespace gen
