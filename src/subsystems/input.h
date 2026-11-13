// Input subsystem: keyboard buffer, scancode mapping, and mouse emulation.
#pragma once

#include <cstdint>
#include <queue>
#include <array>
#include <SDL_scancode.h>

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

    bool key_available() const { return !key_queue_.empty(); }

    bool is_key_down(uint8_t scancode) const {
        return key_down_[scancode] != 0;
    }

    void on_key_down(uint32_t sdl_key, uint16_t sdl_mod);
    void on_key_up(uint32_t sdl_key, uint16_t sdl_mod);
    void on_mouse_move(int window_x, int window_y, int win_w, int win_h);
    void on_mouse_button(uint8_t sdl_button, bool down);
    void move_mouse_relative(int dx, int dy);
    void toggle_keyboard_pointer();
    void toggle_fullscreen();

    // Deliver queued keyboard (INT 09h) and mouse (INT 33h AX=0Ch) events to
    // the program's handlers. Called by Machine when interrupts may run.
    void deliver_irqs();
    void reset_mouse_handler() { mouse_handler_seg_ = 0; mouse_handler_off_ = 0; mouse_handler_mask_ = 0; }

private:
    // Keyboard-pointer keys (when enabled): returns true if the key was consumed.
    bool handle_pointer_key(SDL_Scancode sc, bool down, bool repeat);
    void queue_mouse_irq(uint16_t mask);
    std::array<bool, SDL_NUM_SCANCODES> held_{};  // keyboard-pointer direction keys held
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

    // Pending hardware events and the INT 33h AX=0Ch user handler.
    struct MouseIrq { uint16_t mask, buttons, x, y; };
    std::queue<uint8_t> irq_scancodes_;
    std::queue<MouseIrq> mouse_irqs_;
    uint16_t mouse_handler_seg_ = 0;
    uint32_t mouse_handler_off_ = 0;
    uint16_t mouse_handler_mask_ = 0;
};

}  // namespace gen
