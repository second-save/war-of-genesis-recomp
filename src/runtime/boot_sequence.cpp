#include "boot_sequence.h"
#include "machine.h"
#include "subsystems/video.h"
#include "subsystems/audio.h"
#include "subsystems/input.h"
#include "subsystems/font_vwf.h"
#include "modules_registry.h"

#include <SDL.h>
#include <fstream>
#include <vector>
#include <iostream>
#include <cstring>
#include <cstdlib>

namespace gen {

static bool load_pcx_640x480(const std::filesystem::path& path, std::vector<uint8_t>& out_pixels, std::vector<uint8_t>& out_palette) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 128) return false;

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), size);

    // Read PCX header
    uint16_t xmin = buffer[4] | (buffer[5] << 8);
    uint16_t ymin = buffer[6] | (buffer[7] << 8);
    uint16_t xmax = buffer[8] | (buffer[9] << 8);
    uint16_t ymax = buffer[10] | (buffer[11] << 8);
    int width = xmax - xmin + 1;
    int height = ymax - ymin + 1;
    int bpl = buffer[66] | (buffer[67] << 8);

    if (width <= 0 || height <= 0) return false;

    out_pixels.assign(width * height, 0);

    size_t idx = 128;
    int row = 0;
    int col = 0;

    while (row < height && idx < size) {
        uint8_t b = buffer[idx++];
        uint8_t count = 1;
        uint8_t val = b;
        if ((b & 0xC0) == 0xC0) {
            count = b & 0x3F;
            if (idx >= size) break;
            val = buffer[idx++];
        }

        for (uint8_t c = 0; c < count; ++c) {
            if (col < width && row < height) {
                out_pixels[row * width + col] = val;
            }
            col++;
            if (col >= bpl) {
                col = 0;
                row++;
                if (row >= height) break;
            }
        }
    }

    // Check for trailing 256-color palette (byte 0x0C followed by 768 bytes)
    if (size >= 769 && buffer[size - 769] == 12) {
        out_palette.assign(buffer.end() - 768, buffer.end());
    }

    return true;
}

static bool load_palette_768(const std::filesystem::path& path, std::vector<uint8_t>& out_palette) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out_palette.resize(768);
    file.read(reinterpret_cast<char*>(out_palette.data()), 768);
    return file.gcount() == 768;
}

static bool load_voc_audio(const std::filesystem::path& path, std::vector<uint8_t>& out_pcm, uint32_t& out_rate) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 26) return false;

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), size);

    uint16_t header_len = buffer[20] | (buffer[21] << 8);
    size_t pos = header_len;
    out_rate = 22050;

    while (pos < size) {
        uint8_t btype = buffer[pos];
        if (btype == 0) break;
        if (pos + 4 > size) break;
        uint32_t blen = buffer[pos + 1] | (buffer[pos + 2] << 8) | (buffer[pos + 3] << 16);
        if (btype == 9 && pos + 16 <= size) {
            out_rate = buffer[pos + 4] | (buffer[pos + 5] << 8) | (buffer[pos + 6] << 16) | (buffer[pos + 7] << 24);
            size_t data_len = (blen > 12) ? (blen - 12) : 0;
            if (pos + 16 + data_len <= size) {
                out_pcm.assign(buffer.begin() + pos + 16, buffer.begin() + pos + 16 + data_len);
                return true;
            }
        } else if (btype == 1 && pos + 6 <= size) {
            uint8_t sr_div = buffer[pos + 4];
            out_rate = 1000000 / (256 - sr_div);
            size_t data_len = (blen > 2) ? (blen - 2) : 0;
            if (pos + 6 + data_len <= size) {
                out_pcm.assign(buffer.begin() + pos + 6, buffer.begin() + pos + 6 + data_len);
                return true;
            }
        }
        pos += 4 + blen;
    }

    return !out_pcm.empty();
}

static bool wait_for_key_or_timeout(Machine& machine, uint32_t duration_ms) {
    // Flush any leftover events before waiting
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    uint32_t start = SDL_GetTicks();
    while (machine.running && (SDL_GetTicks() - start < duration_ms)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                machine.running = false;
                std::exit(0);
            }
            if (ev.type == SDL_KEYDOWN) {
                if ((ev.key.keysym.sym == SDLK_q && (ev.key.keysym.mod & KMOD_GUI)) ||
                    (ev.key.keysym.sym == SDLK_F4 && (ev.key.keysym.mod & KMOD_ALT))) {
                    machine.running = false;
                    std::exit(0);
                }
                if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_SPACE ||
                    ev.key.keysym.sym == SDLK_ESCAPE) {
                    return true;
                }
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN) {
                return true;
            }
        }
        SDL_Delay(10);
    }
    return false;
}

static void copy_default_save_files(const std::filesystem::path& data_dir) {
    std::error_code ec;
    // Copies necessary starting templates if missing:
    // copy genproc#.def genproc#.swp
    // copy eve_swc.def event.swc
    // copy bboxchr.def blackbox.chr
    // copy bboxitem.def blackbox.itm
    // copy .\chapdata\0.chp .\chapdata\contibak.tmp
    const std::pair<std::string, std::string> copies[] = {
        {"GENPROC#.DEF", "GENPROC#.SWP"},
        {"EVE_SWC.DEF", "EVENT.SWC"},
        {"BBOXCHR.DEF", "BLACKBOX.CHR"},
        {"BBOXITEM.DEF", "BLACKBOX.ITM"},
    };

    for (const auto& [src, dst] : copies) {
        auto src_p = data_dir / src;
        auto dst_p = data_dir / dst;
        if (std::filesystem::exists(src_p)) {
            std::filesystem::copy_file(src_p, dst_p, std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    auto chp0 = data_dir / "CHAPDATA" / "0.CHP";
    auto contibak = data_dir / "CHAPDATA" / "contibak.tmp";
    if (std::filesystem::exists(chp0)) {
        std::filesystem::copy_file(chp0, contibak, std::filesystem::copy_options::overwrite_existing, ec);
    }

    // For New Game, remove continue.sve and CSTLDATA/LOAD. so MAIN.EXE treats this as a fresh game (Chapter 0)
    auto cont_sve = data_dir / "continue.sve";
    std::filesystem::remove(cont_sve, ec);
    auto load_file = data_dir / "CSTLDATA" / "LOAD.";
    std::filesystem::remove(load_file, ec);
}

static bool load_save_slot(const std::filesystem::path& data_dir, int slot) {
    std::error_code ec;
    std::string s_slot = std::to_string(slot);
    auto contibak_sv = data_dir / "CHAPDATA" / ("contibak.sv" + s_slot);
    if (!std::filesystem::exists(contibak_sv)) {
        std::cout << "[LOAD] Save slot " << slot << " does not exist.\n";
        return false;
    }

    auto contibak_tmp = data_dir / "CHAPDATA" / "contibak.tmp";
    std::filesystem::copy_file(contibak_sv, contibak_tmp, std::filesystem::copy_options::overwrite_existing, ec);

    const std::pair<std::string, std::string> save_copies[] = {
        {"genproc#.sv" + s_slot, "GENPROC#.SWP"},
        {"eve_swc.sv" + s_slot, "EVENT.SWC"},
        {"bboxchr.sv" + s_slot, "BLACKBOX.CHR"},
        {"bboxitem.sv" + s_slot, "BLACKBOX.ITM"},
    };

    for (const auto& [src, dst] : save_copies) {
        auto src_p = data_dir / src;
        auto dst_p = data_dir / dst;
        if (std::filesystem::exists(src_p)) {
            std::filesystem::copy_file(src_p, dst_p, std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    // Touch cstldata/LOAD. and cstldata/pass.
    auto load_file = data_dir / "CSTLDATA" / "LOAD.";
    std::ofstream lf(load_file);
    lf << "OK";

    auto pass_file = data_dir / "CSTLDATA" / "pass.";
    std::ofstream pf(pass_file);
    pf << "OK";

    return true;
}

static void render_text_str(uint8_t* fb, int fb_w, int fb_h,
                            FontVwfSubsystem& font, const std::string& str,
                            int start_x, int start_y, uint8_t color, uint8_t shadow_color = 0) {
    int cur_x = start_x;
    int cur_y = start_y;
    size_t i = 0;

    while (i < str.size()) {
        uint8_t b1 = static_cast<uint8_t>(str[i]);
        if (b1 == '\n') {
            cur_x = start_x;
            cur_y += 18;
            i++;
            continue;
        }

        if (b1 >= 0x80) { // Johab Korean character (2 bytes)
            if (i + 1 < str.size()) {
                uint8_t b2 = static_cast<uint8_t>(str[i + 1]);
                uint8_t glyph[32]{};
                if (font.compose_johab(b1, b2, glyph)) {
                    for (int row = 0; row < 16; ++row) {
                        uint16_t row_bits = (static_cast<uint16_t>(glyph[row * 2]) << 8) | glyph[row * 2 + 1];
                        int py = cur_y + row;
                        if (py < 0 || py >= fb_h) continue;

                        for (int col = 0; col < 16; ++col) {
                            if (row_bits & (0x8000 >> col)) {
                                int px = cur_x + col;
                                if (px >= 0 && px < fb_w) {
                                    if (shadow_color != 0 && px + 1 < fb_w && py + 1 < fb_h) {
                                        fb[(py + 1) * fb_w + (px + 1)] = shadow_color;
                                    }
                                    fb[py * fb_w + px] = color;
                                }
                            }
                        }
                    }
                }
                cur_x += 16;
                i += 2;
            } else {
                i++;
            }
        } else { // ASCII / English character (1 byte)
            const auto& ascii = font.ascii_font();
            int glyph_idx = b1;
            if (glyph_idx * 16 + 16 <= static_cast<int>(ascii.size())) {
                const uint8_t* glyph = &ascii[glyph_idx * 16];
                for (int row = 0; row < 16; ++row) {
                    uint8_t row_bits = glyph[row];
                    int py = cur_y + row;
                    if (py < 0 || py >= fb_h) continue;

                    for (int col = 0; col < 8; ++col) {
                        if (row_bits & (0x80 >> col)) {
                            int px = cur_x + col;
                            if (px >= 0 && px < fb_w) {
                                if (shadow_color != 0 && px + 1 < fb_w && py + 1 < fb_h) {
                                    fb[(py + 1) * fb_w + (px + 1)] = shadow_color;
                                }
                                fb[py * fb_w + px] = color;
                            }
                        }
                    }
                }
            }
            cur_x += font.get_glyph_width(b1);
            i += 1;
        }
    }
}

// Interactive Title Screen and Authentic Main Menu
static void run_main_menu(Machine& machine, const std::filesystem::path& data_dir) {
    std::filesystem::path bgstr1_pcx = data_dir / "TTLDATA" / "BGSTR1.PCX";
    std::filesystem::path pass_gr = data_dir / "TTLDATA" / "PASS.GR";

    std::vector<uint8_t> bg_pixels;
    std::vector<uint8_t> bg_palette;
    if (!load_pcx_640x480(bgstr1_pcx, bg_pixels, bg_palette)) {
        std::cerr << "[BOOT] Failed to load BGSTR1.PCX\n";
        return;
    }

    machine.video->set_palette_rgb(bg_palette.data(), false);

    // Composite PASS.GR over the top 240 rows if available
    std::ifstream pass_file(pass_gr, std::ios::binary);
    std::vector<uint8_t> pass_pixels(640 * 240);
    if (pass_file.read(reinterpret_cast<char*>(pass_pixels.data()), 640 * 240)) {
        for (int y = 0; y < 240; ++y) {
            for (int x = 0; x < 640; ++x) {
                uint8_t p = pass_pixels[y * 640 + x];
                if (p != 0) {
                    bg_pixels[y * 640 + x] = p;
                }
            }
        }
    }

    // Load fonts for menu rendering
    FontVwfSubsystem font;
    font.load_fonts((data_dir / "HANGO8.FNT").string(), (data_dir / "NEWASCII.FNT").string());

    // Authentic Softmax menu choices from TTL.EXE
    struct MenuItem {
        std::string johab_text;
        std::string english_hint;
        int action_id;
    };

    // Authentic Korean Johab bytes extracted directly from TTL.EXE
    std::string s_scenario = "\xaf\xa1\x90\x61\x9f\xa1\xb5\xa1 \xad\xa1\x88\x81"; // 시나리오 소개
    std::string s_character = " \xc4\x81\x9f\xa2\xc8\xe1 \xad\xa1\x88\x81";       // 캐릭터 소개
    std::string s_newgame = "\xc0\xe1\xb7\x71\xa6\x81\xc8\xe1 \xd0\x61\x8b\xa1";   // 처음부터 하기
    std::string s_loadgame = "  \x9d\xa1\x97\x61 \x89\x41\xb7\xb1";               // 로드 게임
    std::string s_continue = " \xb7\xa1\xb4\xe1\xac\xe1 \xd0\x61\x8b\xa1";        // 이어서 하기
    std::string s_dos = "   DOS\x9d\xa1";                                          // DOS로

    std::vector<MenuItem> menu_items = {
        { s_newgame,   "New Game (처음부터 하기)",   1 },
        { s_loadgame,  "Load Game (로드 게임)",      2 },
        { s_continue,  "Continue (이어서 하기)",     3 },
        { s_scenario,  "Scenario (시나리오 소개)",    4 },
        { s_character, "Characters (캐릭터 소개)",   5 },
        { s_dos,       "Exit to OS (종료)",         6 }
    };

    int selected_idx = 0;
    bool menu_running = true;

    // Menu layout geometry (right side of title screen)
    const int menu_box_x1 = 475;
    const int menu_box_y1 = 28;
    const int menu_box_x2 = 624;
    const int menu_box_y2 = 175;
    const int item_x = 492;
    const int item_start_y = 42;
    const int item_step_y = 21;

    std::vector<uint8_t> frame_buffer = bg_pixels;

    while (menu_running && machine.running) {
        // Redraw frame from clean background
        frame_buffer = bg_pixels;

        // Draw elegant dark semi-transparent backing box with double border
        for (int y = menu_box_y1; y <= menu_box_y2; ++y) {
            for (int x = menu_box_x1; x <= menu_box_x2; ++x) {
                if (x == menu_box_x1 || x == menu_box_x2 || y == menu_box_y1 || y == menu_box_y2) {
                    frame_buffer[y * 640 + x] = 255; // White border
                } else if (x == menu_box_x1 + 1 || x == menu_box_x2 - 1 || y == menu_box_y1 + 1 || y == menu_box_y2 - 1) {
                    frame_buffer[y * 640 + x] = 0;   // Black inner border
                } else {
                    frame_buffer[y * 640 + x] = 0;   // Black opaque background
                }
            }
        }

        // Render each menu option
        for (size_t i = 0; i < menu_items.size(); ++i) {
            int y = item_start_y + static_cast<int>(i) * item_step_y;
            bool is_sel = (static_cast<int>(i) == selected_idx);

            // Selection indicator & highlight bar
            if (is_sel) {
                for (int by = y - 2; by <= y + 17; ++by) {
                    for (int bx = menu_box_x1 + 3; bx <= menu_box_x2 - 3; ++bx) {
                        frame_buffer[by * 640 + bx] = 47; // Deep blue highlight bar
                    }
                }
                // Arrow cursor: "►"
                render_text_str(frame_buffer.data(), 640, 480, font, ">", menu_box_x1 + 5, y, 16, 0);
            }

            // Normal text color: White (1) or Light Gray (255); Highlighted: Bright Gold (16)
            uint8_t text_col = is_sel ? 16 : 1;
            uint8_t shadow_col = 0; // Black drop shadow
            render_text_str(frame_buffer.data(), 640, 480, font, menu_items[i].johab_text, item_x, y, text_col, shadow_col);
        }

        machine.video->display_frame_640x480(frame_buffer.data());

        // Process inputs
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                machine.running = false;
                std::exit(0);
            }
            if (ev.type == SDL_KEYDOWN) {
                if ((ev.key.keysym.sym == SDLK_q && (ev.key.keysym.mod & KMOD_GUI)) ||
                    (ev.key.keysym.sym == SDLK_F4 && (ev.key.keysym.mod & KMOD_ALT))) {
                    machine.running = false;
                    std::exit(0);
                }

                switch (ev.key.keysym.sym) {
                case SDLK_UP:
                case SDLK_w:
                case SDLK_KP_8:
                    selected_idx = (selected_idx - 1 + static_cast<int>(menu_items.size())) % static_cast<int>(menu_items.size());
                    break;
                case SDLK_DOWN:
                case SDLK_s:
                case SDLK_KP_2:
                    selected_idx = (selected_idx + 1) % static_cast<int>(menu_items.size());
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                    menu_running = false;
                    break;
                case SDLK_ESCAPE:
                    machine.running = false;
                    std::exit(0);
                    break;
                }
            } else if (ev.type == SDL_MOUSEMOTION) {
                int win_w = 1280, win_h = 960;
                if (machine.video->window()) {
                    SDL_GetWindowSize(machine.video->window(), &win_w, &win_h);
                }
                int mx = ev.motion.x * 640 / win_w;
                int my = ev.motion.y * 480 / win_h;

                if (mx >= menu_box_x1 && mx <= menu_box_x2) {
                    for (size_t i = 0; i < menu_items.size(); ++i) {
                        int y = item_start_y + static_cast<int>(i) * item_step_y;
                        if (my >= y - 2 && my <= y + 17) {
                            selected_idx = static_cast<int>(i);
                            break;
                        }
                    }
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    menu_running = false;
                }
            }
        }

        SDL_Delay(16); // 60 FPS update rate
    }

    if (!machine.running) return;

    // Dispatch menu choice
    int action = menu_items[selected_idx].action_id;
    std::cout << "[MENU] Selected option " << action << ": " << menu_items[selected_idx].english_hint << "\n";

    if (action == 1) { // New Game
        copy_default_save_files(data_dir);
    } else if (action == 2 || action == 3) { // Load or Continue Game
        bool loaded = false;
        // Search for highest available save slot (5 down to 1)
        for (int slot = 5; slot >= 1; --slot) {
            if (load_save_slot(data_dir, slot)) {
                std::cout << "[MENU] Resumed save slot " << slot << "\n";
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            std::cout << "[MENU] No existing save slot found; starting new campaign defaults.\n";
            copy_default_save_files(data_dir);
        }
    } else if (action == 6) { // Exit to OS
        std::cout << "[BOOT] Quitting game cleanly.\n";
        machine.running = false;
        std::exit(0);
    } else {
        // Intro scenario or character viewer -> default to campaign
        copy_default_save_files(data_dir);
    }

    // Touch handshake file for MAIN.EXE: pass.
    std::filesystem::path hs_file = data_dir / "CSTLDATA" / "pass.";
    std::ofstream pf(hs_file);
    pf << "OK";
}

void run_boot_sequence(Machine& machine, const std::filesystem::path& data_dir) {
    std::cout << "[BOOT] Starting authentic Softmax boot sequence...\n";

    // 1. Softmax Company Logo & digitized audio
    std::filesystem::path logo_gr = data_dir / "TTLDATA" / "LOGO.GR";
    std::filesystem::path logo_pal = data_dir / "TTLDATA" / "LOGO.PAL";
    std::filesystem::path logo_voc = data_dir / "TTLDATA" / "LOGO.VOC";

    std::vector<uint8_t> logo_pixels;
    std::vector<uint8_t> logo_palette;
    if (load_pcx_640x480(logo_gr, logo_pixels, logo_palette)) {
        std::vector<uint8_t> pal;
        if (load_palette_768(logo_pal, pal)) {
            // LOGO.PAL contains 6-bit DAC values (0..63)
            machine.video->set_palette_rgb(pal.data(), true);
        } else if (!logo_palette.empty()) {
            machine.video->set_palette_rgb(logo_palette.data(), false);
        }
        machine.video->display_frame_640x480(logo_pixels.data());

        // Play digitized voice: "Softmax"
        std::vector<uint8_t> pcm;
        uint32_t sample_rate = 22050;
        if (load_voc_audio(logo_voc, pcm, sample_rate)) {
            std::cout << "[AUDIO] Playing Softmax voice logo (" << pcm.size() << " samples @ " << sample_rate << "Hz)...\n";
            machine.audio->play_pcm_u8(pcm.data(), pcm.size(), sample_rate);
        }

        // Display for ~2.5 seconds or until key press
        wait_for_key_or_timeout(machine, 2500);
    }

    if (!machine.running) return;

    // 2. The War of Genesis Title Logo (GMLOGO.GR)
    std::filesystem::path gmlogo_gr = data_dir / "TTLDATA" / "GMLOGO.GR";
    std::vector<uint8_t> gmlogo_pixels;
    std::vector<uint8_t> gmlogo_palette;
    if (load_pcx_640x480(gmlogo_gr, gmlogo_pixels, gmlogo_palette)) {
        if (!gmlogo_palette.empty()) {
            machine.video->set_palette_rgb(gmlogo_palette.data(), false);
        }
        machine.video->display_frame_640x480(gmlogo_pixels.data());
        wait_for_key_or_timeout(machine, 2200);
    }

    if (!machine.running) return;

    // 3. Interactive Main Menu
    run_main_menu(machine, data_dir);

    std::cout << "[BOOT] Boot sequence completed. Transitioning to game...\n";
}

}  // namespace gen
