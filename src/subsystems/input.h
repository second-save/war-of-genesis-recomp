// Input subsystem: keyboard buffer, scancode mapping, and mouse emulation.
#pragma once

#include <cstdint>
#include <queue>

namespace gen {

class Machine;

class InputSubsystem {
public:
    InputSubsystem(Machine& machine);
    ~InputSubsystem();

    void pump_events();

    // Port 0x60 keyboard I/O
    uint8_t read_port_60();

    // BIOS INT 16h Keyboard
    void handle_int16();

    // Mouse INT 33h
    void handle_int33();

    // Mouse coordinates in game resolution
    uint16_t mouse_x() const { return mouse_x_; }
    uint16_t mouse_y() const { return mouse_y_; }
    uint16_t mouse_buttons() const { return mouse_buttons_; }

    struct MouseEvent {
        uint16_t code;
        uint16_t x;
        uint16_t y;
    };

    bool has_mouse_event() const { return !mouse_queue_.empty(); }
    bool pop_mouse_event(MouseEvent& out) {
        if (mouse_queue_.empty()) return false;
        out = mouse_queue_.front();
        mouse_queue_.pop();
        return true;
    }
    void clear_mouse_events() {
        while (!mouse_queue_.empty()) mouse_queue_.pop();
    }

    bool is_key_down(uint8_t scancode) const {
        return key_down_[scancode] != 0;
    }

    void on_key_down(uint32_t sdl_key, uint16_t sdl_mod);
    void on_key_up(uint32_t sdl_key, uint16_t sdl_mod);
    void on_mouse_move(int window_x, int window_y, int win_w, int win_h);
    void on_mouse_button(uint8_t sdl_button, bool down);
    void move_mouse_relative(int dx, int dy);

private:
    Machine& machine_;

    // Keyboard state
    uint8_t last_scancode_ = 0;
    uint8_t key_down_[256] = {0};
    std::queue<uint16_t> key_queue_; // (scancode << 8) | ascii

    // Mouse state
    bool mouse_visible_ = false;
    uint16_t mouse_x_ = 320;
    uint16_t mouse_y_ = 100;
    uint16_t mouse_min_x_ = 0;
    uint16_t mouse_max_x_ = 639;
    uint16_t mouse_min_y_ = 0;
    uint16_t mouse_max_y_ = 199;
    uint16_t mouse_buttons_ = 0; // bit 0: left, bit 1: right, bit 2: middle
    std::queue<MouseEvent> mouse_queue_;
};

}  // namespace gen
