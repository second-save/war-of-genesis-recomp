// Main entry point for The War of Genesis native static recompilation.
#include "runtime/machine.h"
#include "runtime/boot_sequence.h"
#include "subsystems/video.h"
#include "subsystems/audio.h"
#include "subsystems/dos.h"

#include <iostream>
#include <string>
#include <filesystem>
#include <SDL.h>

namespace gen {
extern const LiftedModule kLifted_main;
extern const LiftedModule kLifted_chp;
extern const LiftedModule kLifted_pack;
extern const LiftedModule kLifted_chrsys;
}

#include <csignal>
#include <cstring>
#include <execinfo.h>


// Debug: GENESIS_INPUT="ms:Key;ms:Key;..." injects key presses (SDL key names, e.g.
// Return, Down, Escape, Space) at the given milliseconds after startup. Also accepts
// "ms:click@x,y" (640x480 game coordinates, left button). Used for headless testing.
#include <sstream>
#include <vector>
namespace {
struct ScriptedInput { uint32_t at; std::string what; };
std::vector<ScriptedInput> g_script;
size_t g_script_pos = 0;
uint32_t g_script_start = 0;

// Inserts a release event 150 ms after a press, keeping the script sorted by time.
void schedule_release(uint32_t now, const std::string& what) {
    ScriptedInput r{now + 150, what};
    auto it = g_script.begin() + static_cast<long>(g_script_pos);
    while (it != g_script.end() && it->at <= r.at) ++it;
    g_script.insert(it, r);
}

Uint32 scripted_input_tick(Uint32 interval, void*) {
    uint32_t now = SDL_GetTicks() - g_script_start;
    while (g_script_pos < g_script.size() && g_script[g_script_pos].at <= now) {
        const std::string& w = g_script[g_script_pos++].what;
        SDL_Event ev{};
        if (w.rfind("click@", 0) == 0) {
            int x = 0, y = 0;
            std::sscanf(w.c_str() + 6, "%d,%d", &x, &y);
            ev.type = SDL_MOUSEMOTION;
            ev.motion.x = x * 2; ev.motion.y = y * 2;  // window is 2x the game size
            SDL_PushEvent(&ev);
            ev = SDL_Event{};
            ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.x = x * 2; ev.button.y = y * 2;
            SDL_PushEvent(&ev);
            schedule_release(now, "~mouseup");
            continue;
        }
        if (w == "~mouseup") {
            ev.type = SDL_MOUSEBUTTONUP;
            ev.button.button = SDL_BUTTON_LEFT;
            SDL_PushEvent(&ev);
            continue;
        }
        bool release = w.rfind("~up:", 0) == 0;
        std::string name = release ? w.substr(4) : w;
        SDL_Keycode k = SDL_GetKeyFromName(name.c_str());
        if (k == SDLK_UNKNOWN) { std::fprintf(stderr, "[INPUT] unknown key '%s'\n", name.c_str()); continue; }
        ev.type = release ? SDL_KEYUP : SDL_KEYDOWN;
        ev.key.keysym.sym = k;
        ev.key.keysym.scancode = SDL_GetScancodeFromKey(k);
        ev.key.state = release ? SDL_RELEASED : SDL_PRESSED;
        SDL_PushEvent(&ev);
        if (!release) {
            schedule_release(now, "~up:" + name);
            std::fprintf(stderr, "[INPUT] t=%u key %s\n", now, name.c_str());
        }
    }
    return g_script_pos < g_script.size() ? interval : 0;
}

void start_scripted_input() {
    const char* spec = std::getenv("GENESIS_INPUT");
    if (!spec) return;
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ';')) {
        auto colon = item.find(':');
        if (colon == std::string::npos) continue;
        g_script.push_back({static_cast<uint32_t>(std::stoul(item.substr(0, colon))), item.substr(colon + 1)});
    }
    g_script_start = SDL_GetTicks();
    SDL_AddTimer(20, scripted_input_tick, nullptr);
}
}  // namespace

static gen::Machine* g_machine = nullptr;

static void crash_handler(int sig) {
    std::fprintf(stderr, "\n[FATAL] Caught signal %d (%s)!\n", sig, strsignal(sig));
    if (g_machine) {
        auto& c = g_machine->cpu;
        std::fprintf(stderr, "CPU State: CS:IP=%04X:%04X  DS=%04X  ES=%04X  SS:SP=%04X:%04X  BP=%04X\n",
                     c.cs, c.ip, c.ds, c.es, c.ss, c.sp.w, c.bp.w);
        std::fprintf(stderr, "           AX=%04X  BX=%04X  CX=%04X  DX=%04X  SI=%04X  DI=%04X\n",
                     c.ax.w, c.bx.w, c.cx.w, c.dx.w, c.si.w, c.di.w);
    }
    void* callstack[32];
    int frames = backtrace(callstack, 32);
    backtrace_symbols_fd(callstack, frames, 2);
    std::exit(1);
}

int main(int argc, char* argv[]) {
    stack_t alt_stack;
    alt_stack.ss_sp = std::malloc(SIGSTKSZ * 4);
    alt_stack.ss_size = SIGSTKSZ * 4;
    alt_stack.ss_flags = 0;
    sigaltstack(&alt_stack, nullptr);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);

    std::cout << "========================================================\n";
    std::cout << "  The War of Genesis (창세기전, 1995) Static Recompilation\n";
    std::cout << "  Native Cross-Platform Port (macOS / Windows / Linux)\n";
    std::cout << "========================================================\n";

    std::filesystem::path data_dir = "The-War-of-Genesis_DOS_KO";
    std::string module_to_run = "main";
    std::string cmd_args = "";

    bool skip_boot = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--module" && i + 1 < argc) {
            module_to_run = argv[++i];
        } else if (arg == "--args" && i + 1 < argc) {
            cmd_args = argv[++i];
        } else if (arg == "--skip-boot") {
            skip_boot = true;
        }
    }

    if (!std::filesystem::exists(data_dir)) {
        std::cerr << "[ERR] Data directory not found: " << data_dir << "\n";
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) < 0) {
        std::fprintf(stderr, "[ERR] SDL_Init failed: %s\n", SDL_GetError());
    }

    start_scripted_input();
    gen::Machine machine;
    g_machine = &machine;
    machine.dos->set_base_dir(data_dir);

    if (!machine.video->init("The War of Genesis (창세기전)")) {
        std::cerr << "[ERR] Failed to initialize video subsystem\n";
        return 1;
    }
    machine.audio->init();

    // Run the authentic Softmax boot sequence (Softmax Logo + Voice, Game Title Logo)
    // unless explicitly running a specific submodule directly or --skip-boot requested
    if (!skip_boot && module_to_run == "main") {
        gen::run_boot_sequence(machine, data_dir);
        if (!machine.running) {
            return 0;
        }
    }

    // Setup handshake file for MAIN.EXE: pass.
    std::filesystem::path pass_file = data_dir / "CSTLDATA" / "pass.";
    if (!std::filesystem::exists(pass_file)) {
        std::ofstream pf(pass_file);
        pf << "OK";
    }

    std::cout << "[INFO] Loading recompiled module: " << module_to_run << "\n";
    int exit_code = 0;
    if (module_to_run == "main") {
        exit_code = machine.run_module(&gen::kLifted_main, cmd_args);
    } else if (module_to_run == "chp") {
        exit_code = machine.run_module(&gen::kLifted_chp, cmd_args);
    } else if (module_to_run == "pack") {
        exit_code = machine.run_module(&gen::kLifted_pack, cmd_args);
    } else if (module_to_run == "chrsys") {
        exit_code = machine.run_module(&gen::kLifted_chrsys, cmd_args);
    } else {
        std::cerr << "[ERR] Unknown module: " << module_to_run << "\n";
        exit_code = 1;
    }

    std::cout << "[INFO] Module finished with exit code: " << exit_code << "\n";
    return exit_code;
}
