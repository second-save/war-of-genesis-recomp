// Player settings for the native port (controls, audio, window), stored in a
// small INI-style text file in the user's preferences folder.
#pragma once

#include <SDL.h>
#include <string>
#include <vector>

namespace gen {

struct Settings {
    // Keyboard pointer: drive the game's mouse pointer from the keyboard.
    bool keyboard_pointer = false;
    int pointer_speed = 5;  // pixels per frame (Shift = x3)
    std::vector<SDL_Scancode> key_up{SDL_SCANCODE_UP, SDL_SCANCODE_KP_8};
    std::vector<SDL_Scancode> key_down{SDL_SCANCODE_DOWN, SDL_SCANCODE_KP_2};
    std::vector<SDL_Scancode> key_left{SDL_SCANCODE_LEFT, SDL_SCANCODE_KP_4};
    std::vector<SDL_Scancode> key_right{SDL_SCANCODE_RIGHT, SDL_SCANCODE_KP_6};
    std::vector<SDL_Scancode> key_click{SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_Z};
    std::vector<SDL_Scancode> key_right_click{SDL_SCANCODE_X, SDL_SCANCODE_BACKSPACE};

    // Audio (0-100)
    int music_volume = 100;
    int voice_volume = 100;

    // Language: "ko" (original) or a translation in lang/<code>/ (e.g. "en").
    std::string language = "en";
    std::string lang_dir;  // optional override of the translation folder

    // The player's DOS game folder (copied once to game/ next to this file).
    std::string game_dir;

    // Window
    bool fullscreen = false;
    bool smooth_scaling = false;

    std::string path;

    static Settings& get();
    void load();
    void save() const;
};

}  // namespace gen
