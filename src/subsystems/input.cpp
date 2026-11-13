// Input subsystem implementation for SDL2 keyboard and mouse handling.
#include "input.h"
#include "video.h"
#include "runtime/machine.h"
#include "runtime/settings.h"

#include <algorithm>

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
            machine_.quit_requested = true;
            machine_.running = false;
            throw ProcessTerminated{0};
            break;
        case SDL_KEYDOWN: {
            SDL_Keycode sym = ev.key.keysym.sym;
            SDL_Scancode sc = ev.key.keysym.scancode;
            // Cmd+Q (Mac) or Alt+F4 quit immediately
            if ((sym == SDLK_q && (ev.key.keysym.mod & KMOD_GUI)) ||
                (sym == SDLK_F4 && (ev.key.keysym.mod & KMOD_ALT))) {
                machine_.quit_requested = true;
                machine_.running = false;
                throw ProcessTerminated{0};
            }
            if (sym == SDLK_F11 || (sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                if (!ev.key.repeat) toggle_fullscreen();
                break;
            }
            if (sym == SDLK_F12) {
                if (!ev.key.repeat) toggle_keyboard_pointer();
                break;
            }
            if (handle_pointer_key(sc, true, ev.key.repeat != 0)) break;
            on_key_down(sym, ev.key.keysym.mod);
            break;
        }
        case SDL_KEYUP:
            if (handle_pointer_key(ev.key.keysym.scancode, false, false)) break;
            on_key_up(ev.key.keysym.sym, ev.key.keysym.mod);
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) held_.fill(false);
            break;
        case SDL_MOUSEMOTION:
            on_mouse_move(ev.motion.x, ev.motion.y, win_w, win_h);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (std::getenv("GENESIS_TRACE_MOUSE"))
                std::fprintf(stderr, "[MOUSE] SDL button %d down at %d,%d\n", ev.button.button, ev.button.x, ev.button.y);
            on_mouse_move(ev.button.x, ev.button.y, win_w, win_h);
            on_mouse_button(ev.button.button, true);
            break;
        case SDL_MOUSEBUTTONUP:
            on_mouse_button(ev.button.button, false);
            break;
        }
    }

    // Keyboard pointer: held direction keys move the game's mouse pointer.
    const Settings& cfg = Settings::get();
    static uint32_t last_kb_mouse_time = 0;
    uint32_t now = SDL_GetTicks();
    if (cfg.keyboard_pointer && now - last_kb_mouse_time >= 16) {
        last_kb_mouse_time = now;
        auto held = [&](const std::vector<SDL_Scancode>& keys) {
            return std::any_of(keys.begin(), keys.end(), [&](SDL_Scancode k) { return held_[k]; });
        };
        int step = cfg.pointer_speed;
        if (SDL_GetModState() & KMOD_SHIFT) step *= 3;
        int dx = 0, dy = 0;
        if (held(cfg.key_up)) dy -= step;
        if (held(cfg.key_down)) dy += step;
        if (held(cfg.key_left)) dx -= step;
        if (held(cfg.key_right)) dx += step;
        if (dx || dy) move_mouse_relative(dx, dy);
    }
}

static bool in_keys(const std::vector<SDL_Scancode>& keys, SDL_Scancode sc) {
    return std::find(keys.begin(), keys.end(), sc) != keys.end();
}

bool InputSubsystem::handle_pointer_key(SDL_Scancode sc, bool down, bool repeat) {
    const Settings& cfg = Settings::get();
    if (!cfg.keyboard_pointer) return false;
    if (in_keys(cfg.key_click, sc)) {
        if (!repeat) on_mouse_button(SDL_BUTTON_LEFT, down);
        return true;
    }
    if (in_keys(cfg.key_right_click, sc)) {
        if (!repeat) on_mouse_button(SDL_BUTTON_RIGHT, down);
        return true;
    }
    // Direction keys move the pointer from pump_events and aren't passed to the game.
    if (in_keys(cfg.key_up, sc) || in_keys(cfg.key_down, sc) || in_keys(cfg.key_left, sc) ||
        in_keys(cfg.key_right, sc)) {
        held_[sc] = down;
        return true;
    }
    return false;
}

void InputSubsystem::toggle_keyboard_pointer() {
    Settings& cfg = Settings::get();
    cfg.keyboard_pointer = !cfg.keyboard_pointer;
    cfg.save();
    if (machine_.video) machine_.video->update_title();
    std::fprintf(stderr, "[INPUT] keyboard pointer %s\n", cfg.keyboard_pointer ? "on" : "off");
}

void InputSubsystem::toggle_fullscreen() {
    Settings& cfg = Settings::get();
    cfg.fullscreen = !cfg.fullscreen;
    cfg.save();
    if (machine_.video) machine_.video->apply_fullscreen();
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
    queue_mouse_irq(0x01);

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
        if (irq_scancodes_.size() < 64) irq_scancodes_.push(scancode);
        if (key_queue_.size() < 16) {
            key_queue_.push((static_cast<uint16_t>(scancode) << 8) | ascii);
        }
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
        if (irq_scancodes_.size() < 64) irq_scancodes_.push(static_cast<uint8_t>(scancode | 0x80));
    }
}

void InputSubsystem::on_mouse_move(int sx, int sy, int, int) {
    // SDL converts window coordinates into game-screen pixels (the renderer has a
    // logical size, which also handles letterboxing and HiDPI). Convert screen
    // pixels into the mouse driver's coordinate space the program set up: games
    // that set a 0..199 vertical range on a 400/480-line screen double Y themselves.
    int sw = machine_.video ? machine_.video->screen_width() : 640;
    int sh = machine_.video ? machine_.video->screen_height() : 480;
    int x = (sw <= 320) ? sx * 2 : sx;
    int y = (mouse_max_y_ < 240 && sh >= 400) ? sy / 2 : sy;

    if (x < mouse_min_x_) x = mouse_min_x_;
    if (x > mouse_max_x_) x = mouse_max_x_;
    if (y < mouse_min_y_) y = mouse_min_y_;
    if (y > mouse_max_y_) y = mouse_max_y_;

    if (mouse_x_ != static_cast<uint16_t>(x) || mouse_y_ != static_cast<uint16_t>(y)) {
        mouse_x_ = static_cast<uint16_t>(x);
        mouse_y_ = static_cast<uint16_t>(y);
        queue_mouse_irq(0x01);
    }

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

    if (bit == 0) return;
    // INT 33h event mask: bit1/2 left press/release, bit3/4 right, bit5/6 middle.
    uint16_t press_mask = (bit == 1) ? 0x02 : (bit == 2) ? 0x08 : 0x20;
    if (down && !(mouse_buttons_ & bit)) {
        mouse_buttons_ |= bit;
        queue_mouse_irq(press_mask);
    } else if (!down && (mouse_buttons_ & bit)) {
        mouse_buttons_ &= ~bit;
        queue_mouse_irq(static_cast<uint16_t>(press_mask << 1));
    }
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

void InputSubsystem::queue_mouse_irq(uint16_t mask) {
    if (mouse_irqs_.size() < 64) mouse_irqs_.push({mask, mouse_buttons_, mouse_x_, mouse_y_});
}

void InputSubsystem::deliver_irqs() {
    while (!irq_scancodes_.empty()) {
        last_scancode_ = irq_scancodes_.front();
        irq_scancodes_.pop();
        bool ok = machine_.call_vector(0x09);
        if (std::getenv("GENESIS_TRACE_MOUSE"))
            std::fprintf(stderr, "[KBD] scancode %02X -> INT 09h %s\n", last_scancode_, ok ? "handled" : "not hooked");
    }
    while (!mouse_irqs_.empty()) {
        MouseIrq ev = mouse_irqs_.front();
        mouse_irqs_.pop();
        uint16_t mask = ev.mask & mouse_handler_mask_;
        if (!mask || (mouse_handler_seg_ == 0 && mouse_handler_off_ == 0)) continue;
        if (std::getenv("GENESIS_TRACE_MOUSE"))
            std::fprintf(stderr, "[MOUSE] handler %04X:%04X mask %02X buttons %u at %u,%u\n",
                         mouse_handler_seg_, mouse_handler_off_, mask, ev.buttons, ev.x, ev.y);
        // The mouse driver far-calls the handler with the event in registers.
        auto setup = [&](Cpu& c) {
            c.ax.w = mask;
            c.bx.w = ev.buttons;
            c.cx.w = ev.x;
            c.dx.w = ev.y;
            c.si.w = 0;
            c.di.w = 0;
        };
        if (machine_.pm)
            machine_.call_handler32(mouse_handler_seg_, mouse_handler_off_, false, setup);
        else
            machine_.call_handler(mouse_handler_seg_, static_cast<uint16_t>(mouse_handler_off_), false, setup);
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
    if (std::getenv("GENESIS_TRACE_MOUSE") && machine_.cpu.ax.w != 3)
        std::fprintf(stderr, "[MOUSE] INT 33h AX=%04X CX=%04X DX=%04X\n", machine_.cpu.ax.w, machine_.cpu.cx.w, machine_.cpu.dx.w);
    uint16_t ax = machine_.cpu.ax.w;
    switch (ax) {
    case 0x0000: // Reset mouse / check status
    case 0x0021: // Software reset
        mouse_handler_mask_ = 0;
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
        mouse_handler_mask_ = machine_.cpu.cx.w;
        mouse_handler_seg_ = machine_.cpu.es;
        mouse_handler_off_ = machine_.pm ? machine_.cpu.dx.e : machine_.cpu.dx.w;
        while (!mouse_irqs_.empty()) mouse_irqs_.pop();
        if (std::getenv("GENESIS_TRACE_INT"))
            std::fprintf(stderr, "[INT] mouse handler mask %02X -> %04X:%04X%s\n", machine_.cpu.cx.w,
                         machine_.cpu.es, machine_.cpu.dx.w,
                         machine_.pm || machine_.resolve_function(machine_.cpu.es, machine_.cpu.dx.w) ? "" : " (NOT LIFTED)");
        break;
    default:
        break;
    }
}

}  // namespace gen
