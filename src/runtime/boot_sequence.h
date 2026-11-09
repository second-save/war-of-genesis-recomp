#pragma once

#include <filesystem>

namespace gen {

class Machine;

// Plays the authentic boot and intro sequence:
// 1. Softmax company logo (TTLDATA/LOGO.GR, TTLDATA/LOGO.PAL) with authentic digitized voice (TTLDATA/LOGO.VOC).
// 2. The War of Genesis title logo (TTLDATA/GMLOGO.GR).
// 3. Main menu with choices:
//    - New Game (처음부터 하기) -> setup defaults and launches Chapter 0
//    - Load Game (로드 게임) -> resumes save state
//    - Sound Test / Music Player -> plays game music tracks
//    - Exit to DOS -> cleanly shuts down
// Supports full keyboard navigation (Arrows/WASD, Enter/Space, Esc) and mouse.
void run_boot_sequence(Machine& machine, const std::filesystem::path& data_dir);

}  // namespace gen
