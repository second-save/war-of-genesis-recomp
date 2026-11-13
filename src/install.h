// Locating the player's copy of the game and installing a working copy of it.
#pragma once

#include <filesystem>

namespace gen::install {

// Returns the folder the game runs from: a copy of the player's DOS game folder
// kept next to the settings file ("game/"), made on first launch. The player's
// own folder is only read. Asks for the folder (native picker where available)
// when it can't be found. Returns an empty path if the player gave up.
std::filesystem::path prepare_game_dir();

// "" when dir holds the game version this port was built from, else why not.
std::string check_game_dir(const std::filesystem::path& dir);

// A release ships a translation as TEXT.TXT, STRINGS.TXT and FILES.PAT (the
// rebuilt game files as copy/insert ops against the player's originals, see
// tools/text/make_patch.py). Builds the files into a folder next to the
// settings file (once per patch) and returns it; returns lang_dir unchanged
// when it holds no FILES.PAT (development trees with the files built in place).
std::filesystem::path prepare_translation(const std::filesystem::path& lang_dir,
                                          const std::filesystem::path& game_dir);

}  // namespace gen::install
