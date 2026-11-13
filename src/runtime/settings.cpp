#include "settings.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gen {

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<SDL_Scancode> parse_keys(const std::string& v) {
    std::vector<SDL_Scancode> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) continue;
        SDL_Scancode sc = SDL_GetScancodeFromName(item.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) {
            std::fprintf(stderr, "[SETTINGS] unknown key name '%s'\n", item.c_str());
            continue;
        }
        out.push_back(sc);
    }
    return out;
}

std::string keys_str(const std::vector<SDL_Scancode>& keys) {
    std::string s;
    for (auto k : keys) {
        if (!s.empty()) s += ", ";
        s += SDL_GetScancodeName(k);
    }
    return s;
}

bool parse_bool(const std::string& v) {
    return v == "1" || v == "on" || v == "yes" || v == "true";
}
}  // namespace

Settings& Settings::get() {
    static Settings s;
    return s;
}

void Settings::load() {
    if (path.empty()) {
        if (const char* env = std::getenv("GENESIS_SETTINGS")) {
            path = env;
        } else if (char* pref = SDL_GetPrefPath("Softmax", "WarOfGenesis")) {
            path = std::string(pref) + "settings.ini";
            SDL_free(pref);
        } else {
            path = "genesis_settings.ini";
        }
    }
    std::ifstream f(path);
    if (!f) {
        save();  // write the defaults so players can find and edit the file
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "keyboard_pointer") keyboard_pointer = parse_bool(v);
        else if (k == "pointer_speed") pointer_speed = std::max(1, std::atoi(v.c_str()));
        else if (k == "key_up") key_up = parse_keys(v);
        else if (k == "key_down") key_down = parse_keys(v);
        else if (k == "key_left") key_left = parse_keys(v);
        else if (k == "key_right") key_right = parse_keys(v);
        else if (k == "key_click") key_click = parse_keys(v);
        else if (k == "key_right_click") key_right_click = parse_keys(v);
        else if (k == "music_volume") music_volume = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "voice_volume") voice_volume = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "language") language = v;
        else if (k == "lang_dir") lang_dir = v;
        else if (k == "game_dir") game_dir = v;
        else if (k == "fullscreen") fullscreen = parse_bool(v);
        else if (k == "smooth_scaling") smooth_scaling = parse_bool(v);
    }
}

void Settings::save() const {
    std::ofstream f(path);
    if (!f) return;
    f << "# The War of Genesis (native port) settings.\n"
         "# Key names are SDL names: Up, Down, Left, Right, Return, Space, Z, X, Backspace, Keypad 8, ...\n\n"
         "[game]\n"
         "# Your DOS game folder. It is copied once into game/ next to this file; the game runs\n"
         "# and saves there. Delete game/ to copy it again.\n"
      << "game_dir = " << game_dir << "\n\n"
      << "[controls]\n"
         "# Move the game's pointer with the keyboard (toggle in game with F12).\n"
         "# When on, the keys below drive the pointer and are not sent to the game.\n"
      << "keyboard_pointer = " << (keyboard_pointer ? "on" : "off") << "\n"
      << "pointer_speed = " << pointer_speed << "\n"
      << "key_up = " << keys_str(key_up) << "\n"
      << "key_down = " << keys_str(key_down) << "\n"
      << "key_left = " << keys_str(key_left) << "\n"
      << "key_right = " << keys_str(key_right) << "\n"
      << "key_click = " << keys_str(key_click) << "\n"
      << "key_right_click = " << keys_str(key_right_click) << "\n\n"
      << "[audio]\n"
      << "# 0-100. Music and sound effects both come from the AdLib (FM) chip.\n"
      << "music_volume = " << music_volume << "\n"
      << "# Digitized sound (Sound Blaster): the Softmax logo voice.\n"
      << "voice_volume = " << voice_volume << "\n\n"
      << "[language]\n"
      << "# en = English translation, ko = original Korean\n"
      << "language = " << language << "\n"
      << (lang_dir.empty() ? "" : "lang_dir = " + lang_dir + "\n") << "\n"
      << "[video]\n"
      << "# Toggle in game with F11 or Alt+Enter.\n"
      << "fullscreen = " << (fullscreen ? "on" : "off") << "\n"
      << "smooth_scaling = " << (smooth_scaling ? "on" : "off") << "\n";
}

}  // namespace gen
