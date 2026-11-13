// Main entry point for The War of Genesis native static recompilation.
#include "runtime/machine.h"
#include "runtime/settings.h"
#include "install.h"
#include <fstream>
#include <iterator>
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
extern const LiftedModule32 kLifted_ttl;
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
SDL_Window* g_machine_window = nullptr;
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
        const std::string w = g_script[g_script_pos++].what;  // copy: schedule_release may reallocate
        if (std::getenv("GENESIS_TRACE_SCRIPT")) std::fprintf(stderr, "[SCRIPT] t=%u %s (%zu/%zu)\n", now, w.c_str(), g_script_pos, g_script.size());
        SDL_Event ev{};
        if (w.rfind("click@", 0) == 0 || w.rfind("rclick@", 0) == 0) {
            bool right = w[0] == 'r';
            int x = 0, y = 0;
            std::sscanf(w.c_str() + (right ? 7 : 6), "%d,%d", &x, &y);
            ev.type = SDL_MOUSEMOTION;
            // Window coordinates for game-screen pixel (x, y); SDL maps them back to
            // screen pixels like real events (the window must be at its default 2x size).
            Uint32 wid = g_machine_window ? SDL_GetWindowID(g_machine_window) : 0;
            ev.motion.windowID = wid;
            ev.motion.x = x; ev.motion.y = y;  // game-screen pixels, as SDL reports real events
            SDL_PushEvent(&ev);
            ev = SDL_Event{};
            ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
            ev.button.windowID = wid;
            ev.button.x = x; ev.button.y = y;
            int pushed = SDL_PushEvent(&ev);
            std::fprintf(stderr, "[INPUT] t=%u click %d,%d%s\n", now, x, y, pushed == 1 ? "" : " (push failed)");
            schedule_release(now, right ? "~rmouseup" : "~mouseup");
            continue;
        }
        if (w == "~mouseup" || w == "~rmouseup") {
            ev.type = SDL_MOUSEBUTTONUP;
            ev.button.button = (w == "~rmouseup") ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
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
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // keep log lines ordered with stderr
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
    std::cout << "  Native port " GENESIS_VERSION " (macOS / Windows / Linux)\n";
    std::cout << "========================================================\n";

    std::filesystem::path data_dir;  // --data-dir: run in place (development, tests)
    std::string module_to_run = "main";
    std::string cmd_args = "";

    bool skip_boot = false;
    int battle = -1;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--module" && i + 1 < argc) {
            module_to_run = argv[++i];
        } else if (arg == "--args" && i + 1 < argc) {
            cmd_args = argv[++i];
        } else if (arg == "--battle" && i + 1 < argc) {
            // Debug: start CHRSYS on battle N the way MAIN does
            // (spawnl("chrsys.exe", <16-byte name from BATTLE.IDX record N>, "hacker")).
            battle = std::atoi(argv[++i]);
            module_to_run = "chrsys";
        } else if (arg == "--skip-boot") {
            skip_boot = true;
        }
    }

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) < 0) {
        std::fprintf(stderr, "[ERR] SDL_Init failed: %s\n", SDL_GetError());
    }

    gen::Settings::get().load();
    std::cout << "[INFO] Settings: " << gen::Settings::get().path << "\n";

    if (data_dir.empty()) {
        data_dir = gen::install::prepare_game_dir();
        if (data_dir.empty()) return 1;
    } else if (std::string why = gen::install::check_game_dir(data_dir); !why.empty()) {
        std::cerr << "[ERR] " << data_dir << ": " << why << "\n";
        if (!std::filesystem::is_directory(data_dir)) return 1;
    }
    std::cout << "[INFO] Game folder: " << data_dir.string() << "\n";
    start_scripted_input();
    gen::Machine machine;
    g_machine = &machine;
    machine.dos->set_base_dir(data_dir);
    {
        // Translation overlay: lang/<language>/ next to the binary (or one level up
        // for build/ trees), or settings lang_dir.
        const auto& st = gen::Settings::get();
        if (st.language != "ko" && !st.language.empty()) {
            std::vector<std::filesystem::path> candidates;
            if (!st.lang_dir.empty()) candidates.emplace_back(st.lang_dir);
            if (char* bp = SDL_GetBasePath()) {
                candidates.emplace_back(std::filesystem::path(bp) / "lang" / st.language);
                candidates.emplace_back(std::filesystem::path(bp) / ".." / "lang" / st.language);
                SDL_free(bp);
            }
            candidates.emplace_back(std::filesystem::path("lang") / st.language);
            for (const auto& c : candidates) {
                std::error_code ec;
                if (std::filesystem::is_directory(c, ec)) {
                    machine.dos->set_overlay_dir(
                        gen::install::prepare_translation(std::filesystem::canonical(c, ec), data_dir));
                    std::cout << "[INFO] Language " << st.language << ": " << machine.dos->overlay_dir().string() << "\n";
                    break;
                }
            }
            if (machine.dos->overlay_dir().empty())
                std::cerr << "[WARN] language '" << st.language << "' selected but no lang/" << st.language << " folder found\n";
        }
    }

    if (!machine.video->init("The War of Genesis (창세기전)")) {
        std::cerr << "[ERR] Failed to initialize video subsystem\n";
        return 1;
    }
    machine.audio->init();
    machine.audio->music_volume = gen::Settings::get().music_volume * 256 / 100;
    machine.audio->sfx_volume = gen::Settings::get().voice_volume * 256 / 100;
    g_machine_window = machine.video->window();
    SDL_ShowCursor(SDL_DISABLE);  // the game draws its own pointer

    int exit_code = 0;
    if (module_to_run == "main" && !skip_boot) {
        // G.COM, the game's launcher, is a compiled batch file:
        //   :LOOP / TTL / IF EXIST $ GOTO JUMP / GOTO EXIT / :JUMP / MAIN / GOTO LOOP
        // TTL (title, menus, intro/ending) leaves "$" behind when the game should start.
        for (;;) {
            std::cout << "[INFO] Running TTL.EXE (title)\n";
            exit_code = machine.run_le(&gen::kLifted_ttl, cmd_args);
            if (machine.quit_requested || !machine.dos->host_file_exists("$")) break;
            std::cout << "[INFO] Running MAIN.EXE\n";
            exit_code = machine.run_module(&gen::kLifted_main, cmd_args);
            if (machine.quit_requested) break;
        }
        std::cout << "[INFO] Game exited (code " << exit_code << ")\n";
        return 0;
    }

    if (module_to_run == "main") {
        // --skip-boot: start MAIN directly, as TTL would for "continue".
        std::ofstream(data_dir / "CSTLDATA" / "PASS") << "";
    }

    if (battle >= 0) {
        std::ifstream idx(machine.dos->resolve_path("BATTLE.IDX"), std::ios::binary);
        std::vector<char> d((std::istreambuf_iterator<char>(idx)), std::istreambuf_iterator<char>());
        if (static_cast<size_t>(2 * battle + 2) > d.size()) {
            std::cerr << "[ERR] No battle " << battle << " in BATTLE.IDX\n";
            return 1;
        }
        size_t off = static_cast<uint8_t>(d[2 * battle]) | (static_cast<uint8_t>(d[2 * battle + 1]) << 8);
        std::string name;
        // Names are stored with every byte doubled; MAIN halves them (0259:0008).
        for (size_t k = off + 1; k < off + 17 && k < d.size() && d[k]; ++k)
            name += static_cast<char>(static_cast<uint8_t>(d[k]) >> 1);
        std::cout << "[INFO] Battle " << battle << ": " << name << "\n";
        cmd_args = " " + name + " hacker";
    }
    std::cout << "[INFO] Loading recompiled module: " << module_to_run << "\n";
    if (module_to_run == "ttl") {
        exit_code = machine.run_le(&gen::kLifted_ttl, cmd_args);
    } else if (module_to_run == "main") {
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
