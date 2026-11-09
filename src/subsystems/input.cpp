// Input subsystem implementation for SDL2 keyboard and mouse handling.
#include "input.h"
#include "video.h"
#include "runtime/machine.h"

#include <SDL.h>
#include <cstdio>

namespace gen {

InputSubsystem::InputSubsystem(Machine& machine) : machine_(machine) {}

InputSubsystem::~InputSubsystem() = default;

void InputSubsystem::pump_events() {
    int win_w = 1280;
    int win_h = 960;
    if (machine_.video && machine_.video->window()) {
        SDL_GetWindowSize(machine_.video->window(), &win_w, &win_h);
    }

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            machine_.running = false;
            throw ProcessTerminated{0};
            break;
        case SDL_KEYDOWN:
            // Check Cmd+Q (Mac) or Alt+F4 to quit immediately
            if ((ev.key.keysym.sym == SDLK_q && (ev.key.keysym.mod & KMOD_GUI)) ||
                (ev.key.keysym.sym == SDLK_F4 && (ev.key.keysym.mod & KMOD_ALT))) {
                machine_.running = false;
                throw ProcessTerminated{0};
            }
            on_key_down(ev.key.keysym.sym, ev.key.keysym.mod);
            break;
        case SDL_KEYUP:
            on_key_up(ev.key.keysym.sym, ev.key.keysym.mod);
            break;
        case SDL_MOUSEMOTION:
            on_mouse_move(ev.motion.x, ev.motion.y, win_w, win_h);
            break;
        case SDL_MOUSEBUTTONDOWN:
            on_mouse_button(ev.button.button, true);
            break;
        case SDL_MOUSEBUTTONUP:
            on_mouse_button(ev.button.button, false);
            break;
        }
    }

    // Keyboard-based mouse cursor navigation:
    // Rate limit to ~60Hz so holding arrow keys moves smoothly without flooding the queue
    static uint32_t last_kb_mouse_time = 0;
    uint32_t now = SDL_GetTicks();
    if (now - last_kb_mouse_time >= 16) {
        last_kb_mouse_time = now;
        const uint8_t* state = SDL_GetKeyboardState(nullptr);
        int step = 6;
        if (state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT]) {
            step = 14; // Shift accelerates cursor movement
        }
        int k_dx = 0, k_dy = 0;
        if (state[SDL_SCANCODE_UP] || state[SDL_SCANCODE_W] || state[SDL_SCANCODE_KP_8]) k_dy -= step;
        if (state[SDL_SCANCODE_DOWN] || state[SDL_SCANCODE_S] || state[SDL_SCANCODE_KP_2]) k_dy += step;
        if (state[SDL_SCANCODE_LEFT] || state[SDL_SCANCODE_A] || state[SDL_SCANCODE_KP_4]) k_dx -= step;
        if (state[SDL_SCANCODE_RIGHT] || state[SDL_SCANCODE_D] || state[SDL_SCANCODE_KP_6]) k_dx += step;
        if (k_dx != 0 || k_dy != 0) {
            move_mouse_relative(k_dx, k_dy);
        }
    }
}

uint8_t InputSubsystem::read_port_60() {
    return last_scancode_;
}

static void sdl_to_dos_scancode(uint32_t sdl_key, uint8_t& scancode, uint8_t& ascii) {
    scancode = 0;
    ascii = 0;

    switch (sdl_key) {
    case SDLK_ESCAPE:    scancode = 0x01; ascii = 0x1B; break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  scancode = 0x1C; ascii = 0x0D; break;
    case SDLK_SPACE:     scancode = 0x39; ascii = ' ';  break;
    case SDLK_BACKSPACE: scancode = 0x0E; ascii = 0x08; break;
    case SDLK_TAB:       scancode = 0x0F; ascii = 0x09; break;
    case SDLK_UP:
    case SDLK_KP_8:      scancode = 0x48; break;
    case SDLK_DOWN:
    case SDLK_KP_2:      scancode = 0x50; break;
    case SDLK_LEFT:
    case SDLK_KP_4:      scancode = 0x4B; break;
    case SDLK_RIGHT:
    case SDLK_KP_6:      scancode = 0x4D; break;
    case SDLK_HOME:
    case SDLK_KP_7:      scancode = 0x47; break;
    case SDLK_END:
    case SDLK_KP_1:      scancode = 0x4F; break;
    case SDLK_PAGEUP:
    case SDLK_KP_9:      scancode = 0x49; break;
    case SDLK_PAGEDOWN:
    case SDLK_KP_3:      scancode = 0x51; break;
    case SDLK_KP_5:      scancode = 0x4C; break;
    case SDLK_LCTRL:
    case SDLK_RCTRL:     scancode = 0x1D; break;
    case SDLK_LALT:
    case SDLK_RALT:      scancode = 0x38; break;
    case SDLK_LSHIFT:    scancode = 0x2A; break;
    case SDLK_RSHIFT:    scancode = 0x36; break;
    case SDLK_F1:        scancode = 0x3B; break;
    case SDLK_F2:        scancode = 0x3C; break;
    case SDLK_F3:        scancode = 0x3D; break;
    case SDLK_F4:        scancode = 0x3E; break;
    case SDLK_F5:        scancode = 0x3F; break;
    case SDLK_F6:        scancode = 0x40; break;
    case SDLK_F7:        scancode = 0x41; break;
    case SDLK_F8:        scancode = 0x42; break;
    case SDLK_F9:        scancode = 0x43; break;
    case SDLK_F10:       scancode = 0x44; break;
    case SDLK_F11:       scancode = 0x57; break;
    case SDLK_F12:       scancode = 0x58; break;
    default:
        if (sdl_key >= SDLK_a && sdl_key <= SDLK_z) {
            static const uint8_t a_to_z_scancodes[] = {
                0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
            };
            scancode = a_to_z_scancodes[sdl_key - SDLK_a];
            ascii = static_cast<uint8_t>(sdl_key);
        } else if (sdl_key >= SDLK_0 && sdl_key <= SDLK_9) {
            static const uint8_t num_scancodes[] = {
                0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A
            };
            scancode = num_scancodes[sdl_key - SDLK_0];
            ascii = static_cast<uint8_t>(sdl_key);
        }
        break;
    }
}

void InputSubsystem::move_mouse_relative(int dx, int dy) {
    int x = static_cast<int>(mouse_x_) + dx;
    int y = static_cast<int>(mouse_y_) + dy;

    if (x < mouse_min_x_) x = mouse_min_x_;
    if (x > mouse_max_x_) x = mouse_max_x_;
    if (y < mouse_min_y_) y = mouse_min_y_;
    if (y > mouse_max_y_) y = mouse_max_y_;

    if (mouse_x_ == static_cast<uint16_t>(x) && mouse_y_ == static_cast<uint16_t>(y)) {
        return; // No change
    }

    mouse_x_ = static_cast<uint16_t>(x);
    mouse_y_ = static_cast<uint16_t>(y);

    uint16_t code = (mouse_buttons_ > 0) ? (mouse_buttons_ + 5) : 1;
    if (mouse_queue_.size() < 10) {
        mouse_queue_.push({code, mouse_x_, mouse_y_});
    }
}

void InputSubsystem::on_key_down(uint32_t sdl_key, uint16_t sdl_mod) {
    (void)sdl_mod;
    uint8_t scancode = 0;
    uint8_t ascii = 0;
    sdl_to_dos_scancode(sdl_key, scancode, ascii);

    if (scancode != 0) {
        key_down_[scancode] = 1;
        last_scancode_ = scancode;
        if (key_queue_.size() < 16) {
            key_queue_.push((static_cast<uint16_t>(scancode) << 8) | ascii);
        }
    }

    // Keyboard action buttons for mouse emulation:
    // Enter / Space = Left Click
    // Escape / Right Ctrl = Right Click
    // Send click down, and automatically release so loops waiting for button release don't hang
    if (sdl_key == SDLK_RETURN || sdl_key == SDLK_KP_ENTER || sdl_key == SDLK_SPACE) {
        on_mouse_button(SDL_BUTTON_LEFT, true);
    } else if (sdl_key == SDLK_ESCAPE || sdl_key == SDLK_RCTRL) {
        on_mouse_button(SDL_BUTTON_RIGHT, true);
    }
}

void InputSubsystem::on_key_up(uint32_t sdl_key, uint16_t sdl_mod) {
    (void)sdl_mod;
    uint8_t scancode = 0;
    uint8_t ascii = 0;
    sdl_to_dos_scancode(sdl_key, scancode, ascii);

    if (scancode != 0) {
        key_down_[scancode] = 0;
        last_scancode_ = scancode | 0x80;
    }

    if (sdl_key == SDLK_RETURN || sdl_key == SDLK_KP_ENTER || sdl_key == SDLK_SPACE) {
        on_mouse_button(SDL_BUTTON_LEFT, false);
    } else if (sdl_key == SDLK_ESCAPE || sdl_key == SDLK_RCTRL) {
        on_mouse_button(SDL_BUTTON_RIGHT, false);
    }
}

void InputSubsystem::on_mouse_move(int window_x, int window_y, int win_w, int win_h) {
    if (win_w <= 0 || win_h <= 0) return;
    int target_w = 640;
    int target_h = (mouse_max_y_ > 200) ? 480 : 200;

    int x = window_x * target_w / win_w;
    int y = window_y * target_h / win_h;

    if (x < mouse_min_x_) x = mouse_min_x_;
    if (x > mouse_max_x_) x = mouse_max_x_;
    if (y < mouse_min_y_) y = mouse_min_y_;
    if (y > mouse_max_y_) y = mouse_max_y_;

    mouse_x_ = static_cast<uint16_t>(x);
    mouse_y_ = static_cast<uint16_t>(y);

    uint16_t code = (mouse_buttons_ > 0) ? (mouse_buttons_ + 5) : 1;
    if (mouse_queue_.size() < 30) {
        mouse_queue_.push({code, mouse_x_, mouse_y_});
    }
}

void InputSubsystem::on_mouse_button(uint8_t sdl_button, bool down) {
    uint16_t bit = 0;
    if (sdl_button == SDL_BUTTON_LEFT) bit = 1;
    else if (sdl_button == SDL_BUTTON_RIGHT) bit = 2;
    else if (sdl_button == SDL_BUTTON_MIDDLE) bit = 4;

    if (down) {
        mouse_buttons_ |= bit;
        uint16_t code = (mouse_buttons_ <= 2) ? (mouse_buttons_ + 1) : 10;
        if (mouse_queue_.size() < 30) {
            mouse_queue_.push({code, mouse_x_, mouse_y_});
        }
    } else {
        mouse_buttons_ &= ~bit;
        uint16_t code = (bit == 1) ? 8 : 9;
        if (mouse_queue_.size() < 30) {
            mouse_queue_.push({code, mouse_x_, mouse_y_});
        }
    }
}

void InputSubsystem::handle_int16() {
    uint8_t ah = machine_.cpu.ax.b.h;
    switch (ah) {
    case 0x00: // Read character from keyboard
    case 0x10:
        while (key_queue_.empty()) {
            pump_events();
            if (!machine_.running) throw ProcessTerminated{0};
            machine_.idle();
        }
        if (!key_queue_.empty()) {
            uint16_t key = key_queue_.front();
            key_queue_.pop();
            machine_.cpu.ax.w = key;
        }
        break;
    case 0x01: // Check for keystroke
    case 0x11:
        pump_events();
        if (key_queue_.empty()) {
            machine_.cpu.zf = 1;
        } else {
            machine_.cpu.zf = 0;
            machine_.cpu.ax.w = key_queue_.front();
        }
        break;
    }
}

void InputSubsystem::handle_int33() {
    uint16_t ax = machine_.cpu.ax.w;
    switch (ax) {
    case 0x0000: // Reset mouse / check status
    case 0x0021: // Software reset
        machine_.cpu.ax.w = 0xFFFF; // Mouse installed
        machine_.cpu.bx.w = 2;      // 2 buttons
        mouse_min_x_ = 0;
        mouse_max_x_ = 639;
        mouse_min_y_ = 0;
        mouse_max_y_ = 199;
        break;
    case 0x0001: // Show mouse cursor
        mouse_visible_ = true;
        break;
    case 0x0002: // Hide mouse cursor
        mouse_visible_ = false;
        break;
    case 0x0003: // Get mouse position and button status
        machine_.cpu.cx.w = mouse_x_;
        machine_.cpu.dx.w = mouse_y_;
        machine_.cpu.bx.w = mouse_buttons_;
        break;
    case 0x0004: // Set mouse cursor position
        mouse_x_ = machine_.cpu.cx.w;
        mouse_y_ = machine_.cpu.dx.w;
        break;
    case 0x0007: // Set horizontal mouse limits
        mouse_min_x_ = machine_.cpu.cx.w;
        mouse_max_x_ = machine_.cpu.dx.w;
        break;
    case 0x0008: // Set vertical mouse limits
        mouse_min_y_ = machine_.cpu.cx.w;
        mouse_max_y_ = machine_.cpu.dx.w;
        break;
    case 0x000C: // Set interrupt subroutine parameters
        break;
    default:
        break;
    }
}

}  // namespace gen
