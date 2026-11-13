#include "install.h"
#include "runtime/settings.h"

#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace gen::install {
namespace {

// The executables the translated code was generated from (1996 Korean release).
struct Exe { const char* name; uintmax_t size; };
constexpr Exe kExes[] = {
    {"MAIN.EXE", 13136}, {"TTL.EXE", 110500}, {"CHP.EXE", 314216},
    {"PACK.EXE", 321772}, {"CHRSYS.EXE", 400912},
};
constexpr const char* kTitle = "The War of Genesis";

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Case-insensitive lookup of a file directly inside dir.
fs::path find_entry(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (upper(e.path().filename().string()) == name) return e.path();
    return {};
}

int ask(SDL_MessageBoxFlags kind, const std::string& text, std::vector<const char*> buttons) {
    std::vector<SDL_MessageBoxButtonData> b;
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
        uint32_t flags = 0;
        if (i == 0) flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        if (i == static_cast<int>(buttons.size()) - 1) flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        b.push_back({flags, i, buttons[i]});
    }
    SDL_MessageBoxData data{kind, nullptr, kTitle, text.c_str(), static_cast<int>(b.size()), b.data(), nullptr};
    int hit = -1;
    if (SDL_ShowMessageBox(&data, &hit) < 0) {
        std::cerr << text << "\n";
        return static_cast<int>(buttons.size()) - 1;  // no display: behave as "Quit"
    }
    return hit < 0 ? static_cast<int>(buttons.size()) - 1 : hit;
}

std::string run_picker() {
#if defined(__APPLE__)
    const char* cmd =
        "osascript -e 'POSIX path of (choose folder with prompt "
        "\"Choose the folder of your The War of Genesis (DOS) game, the one with MAIN.EXE in it.\")' 2>/dev/null";
#elif defined(__linux__)
    const char* cmd =
        "zenity --file-selection --directory --title='Folder of The War of Genesis (DOS)' 2>/dev/null";
#else
    const char* cmd = nullptr;
#endif
    if (!cmd) return {};
#if defined(_WIN32)
    return {};
#else
    FILE* p = popen(cmd, "r");
    if (!p) return {};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}

uint32_t crc32(const std::vector<uint8_t>& d) {
    static uint32_t table[256] = {};
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Case-insensitive lookup of a '/'-separated relative path.
fs::path find_path(fs::path dir, const std::string& rel) {
    size_t pos = 0;
    while (pos <= rel.size()) {
        size_t slash = rel.find('/', pos);
        std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        dir = find_entry(dir, upper(part));
        if (dir.empty() || slash == std::string::npos) return dir;
        pos = slash + 1;
    }
    return dir;
}

fs::path install_root() {
    return fs::path(Settings::get().path).parent_path() / "game";
}

}  // namespace

std::string check_game_dir(const fs::path& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return "folder not found";
    for (const auto& x : kExes) {
        fs::path p = find_entry(dir, x.name);
        if (p.empty()) return std::string(x.name) + " is missing";
        uintmax_t n = fs::file_size(p, ec);
        if (n != x.size)
            return std::string(x.name) + " is a different version (" + std::to_string(n) + " bytes, expected " +
                   std::to_string(x.size) + ")";
    }
    if (find_entry(dir, "CSTLDATA").empty()) return "the CSTLDATA folder is missing";
    return {};
}

fs::path prepare_game_dir() {
    Settings& st = Settings::get();
    const fs::path root = install_root();
    if (check_game_dir(root).empty()) return root;

    // Where the player's game folder may be.
    std::vector<fs::path> where;
    if (!st.game_dir.empty()) where.emplace_back(st.game_dir);
    where.emplace_back(fs::current_path() / "The-War-of-Genesis_DOS_KO");
    if (char* bp = SDL_GetBasePath()) {
        fs::path b(bp);
        SDL_free(bp);
        where.push_back(b / "The-War-of-Genesis_DOS_KO");
        where.push_back(b / ".." / "The-War-of-Genesis_DOS_KO");               // build/ trees
        where.push_back(b / ".." / ".." / ".." / "The-War-of-Genesis_DOS_KO");  // next to the .app
    }
    fs::path src;
    for (const auto& w : where)
        if (check_game_dir(w).empty()) { src = w; break; }

    while (src.empty()) {
        int b = ask(SDL_MESSAGEBOX_INFORMATION,
                    "This port runs on the files of the original DOS game (Korean release, 1996), "
                    "which are not included.\n\n"
                    "Choose the game's folder (the one with MAIN.EXE and CHRSYS.EXE in it). "
                    "It is copied once to your user folder; your own folder is never changed.",
                    {"Choose folder...", "Quit"});
        if (b != 0) return {};
        std::string picked = run_picker();
        if (picked.empty()) {
#if !defined(__APPLE__) && !defined(__linux__)
            ask(SDL_MESSAGEBOX_INFORMATION,
                "Put the game's folder next to this program, named The-War-of-Genesis_DOS_KO, or set "
                "game_dir in the settings file:\n\n" + st.path,
                {"Quit"});
            return {};
#else
            continue;
#endif
        }
        std::string why = check_game_dir(picked);
        if (why.empty()) {
            src = picked;
        } else {
            ask(SDL_MESSAGEBOX_WARNING, "That folder can't be used: " + why + ".", {"OK"});
        }
    }

    std::cout << "[INFO] Installing game files from " << src.string() << " to " << root.string() << "\n";
    std::error_code ec;
    const fs::path tmp = root.string() + ".tmp";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp.parent_path(), ec);
    fs::copy(src, tmp, fs::copy_options::recursive, ec);
    if (!ec) {
        fs::remove_all(root, ec);
        fs::rename(tmp, root, ec);
    }
    if (ec || !check_game_dir(root).empty()) {
        ask(SDL_MESSAGEBOX_ERROR, "Copying the game files to " + root.string() + " failed: " + ec.message(), {"Quit"});
        return {};
    }
    st.game_dir = fs::absolute(src).lexically_normal().string();
    st.save();
    return root;
}

fs::path prepare_translation(const fs::path& lang_dir, const fs::path& game_dir) {
    const fs::path pat_path = lang_dir / "FILES.PAT";
    std::error_code ec;
    if (!fs::exists(pat_path, ec)) return lang_dir;
    const std::vector<uint8_t> pat = read_file(pat_path);
    const fs::path out = fs::path(Settings::get().path).parent_path() / "lang" / lang_dir.filename();
    const std::string stamp = std::to_string(crc32(pat)) + " " + std::to_string(pat.size());
    {
        std::ifstream f(out / "BUILT");
        std::string have;
        std::getline(f, have);
        if (have == stamp) return out;
    }

    auto u32 = [&](size_t& p) {
        uint32_t v = 0;
        if (p + 4 <= pat.size()) std::memcpy(&v, &pat[p], 4);
        p += 4;
        return v;
    };
    size_t p = 4;
    if (pat.size() < 12 || std::memcmp(pat.data(), "GPAT", 4) != 0 || u32(p) != 1) {
        std::cerr << "[WARN] " << pat_path.string() << " is not a translation patch\n";
        return lang_dir;
    }
    fs::remove_all(out, ec);
    fs::create_directories(out, ec);
    const uint32_t nfiles = u32(p);
    int built = 0, skipped = 0;
    for (uint32_t i = 0; i < nfiles && p < pat.size(); ++i) {
        uint16_t len = static_cast<uint16_t>(pat[p] | (pat[p + 1] << 8));
        p += 2;
        std::string rel(reinterpret_cast<const char*>(&pat[p]), len);
        p += len;
        uint32_t src_size = u32(p), src_crc = u32(p), dst_size = u32(p), dst_crc = u32(p), nops = u32(p);
        fs::path src_path = find_path(game_dir, rel);
        std::vector<uint8_t> src = src_path.empty() ? std::vector<uint8_t>{} : read_file(src_path);
        const bool ok = src.size() == src_size && crc32(src) == src_crc;
        std::vector<uint8_t> dst;
        dst.reserve(dst_size);
        for (uint32_t k = 0; k < nops && p < pat.size(); ++k) {
            uint8_t kind = pat[p++];
            if (kind == 0) {
                uint32_t off = u32(p), n = u32(p);
                if (ok && off + n <= src.size()) dst.insert(dst.end(), src.begin() + off, src.begin() + off + n);
            } else {
                uint32_t n = u32(p);
                if (p + n <= pat.size()) dst.insert(dst.end(), pat.begin() + p, pat.begin() + p + n);
                p += n;
            }
        }
        if (!ok || dst.size() != dst_size || crc32(dst) != dst_crc) {
            ++skipped;  // the player's file differs: that file stays untranslated
            continue;
        }
        fs::path target = out / fs::path(rel);
        fs::create_directories(target.parent_path(), ec);
        std::ofstream(target, std::ios::binary).write(reinterpret_cast<const char*>(dst.data()),
                                                      static_cast<std::streamsize>(dst.size()));
        ++built;
    }
    for (const char* name : {"TEXT.TXT", "STRINGS.TXT"})
        fs::copy_file(lang_dir / name, out / name, fs::copy_options::overwrite_existing, ec);
    std::ofstream(out / "BUILT") << stamp << "\n";
    std::cout << "[INFO] Translation files built: " << built << (skipped ? ", " + std::to_string(skipped) + " skipped (game files differ)" : "") << "\n";
    return out;
}

}  // namespace gen::install
