#include "../runtime/memory.h"
#include <cstdlib>
// DOS subsystem implementation for filesystem virtualization, INT 21h, and XMS driver.
#include "dos.h"
#include "runtime/machine.h"
#include "subsystems/input.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace gen {

DosSubsystem::DosSubsystem(Machine& machine) : machine_(machine) {
    base_dir_ = std::filesystem::current_path() / "The-War-of-Genesis_DOS_KO";
}

DosSubsystem::~DosSubsystem() = default;

void DosSubsystem::set_base_dir(const std::filesystem::path& base_dir) {
    base_dir_ = base_dir;
}

uint32_t DosSubsystem::pdx() const { return machine_.pm ? machine_.cpu.dx.e : machine_.cpu.dx.w; }

std::string DosSubsystem::read_dos_string(uint16_t seg, uint32_t off) const {
    std::string s;
    const uint8_t* p = machine_.ptr(seg, off);
    while (*p != '\0') {
        s.push_back(static_cast<char>(*p++));
    }
    return s;
}

std::filesystem::path DosSubsystem::resolve_path(const std::string& dos_path) const {
    std::string clean = dos_path;
    // Normalize backslashes
    std::replace(clean.begin(), clean.end(), '\\', '/');

    // Remove leading drive letter like "C:" or "c:"
    if (clean.size() >= 2 && std::isalpha(static_cast<unsigned char>(clean[0])) && clean[1] == ':') {
        clean = clean.substr(2);
    }

    // Relative paths start in the current directory (INT 21h 3Bh).
    bool absolute = !clean.empty() && clean[0] == '/';
    while (!clean.empty() && clean[0] == '/') {
        clean = clean.substr(1);
    }
    if (!absolute && !cwd_.empty()) clean = cwd_ + "/" + clean;

    // Remove leading "./"
    while (clean.rfind("./", 0) == 0) {
        clean = clean.substr(2);
    }

    std::filesystem::path cur = base_dir_;
    std::stringstream ss(clean);
    std::string part;

    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        // DOS treats "NAME." as "NAME" (empty extension).
        if (part != ".." && part.size() > 1 && part.back() == '.') part.pop_back();
        if (part == "..") {
            if (cur != base_dir_) cur = cur.parent_path();  // never above the game root
            continue;
        }

        // Case-insensitive search in directory
        bool found = false;
        if (std::filesystem::exists(cur) && std::filesystem::is_directory(cur)) {
            for (const auto& entry : std::filesystem::directory_iterator(cur)) {
                std::string entry_name = entry.path().filename().string();
                // A host file named "NAME." is the same DOS file as "NAME".
                if (entry_name.size() > 1 && entry_name.back() == '.' && entry_name != "..")
                    entry_name.pop_back();
                if (entry_name.size() == part.size()) {
                    bool match = true;
                    for (size_t i = 0; i < part.size(); ++i) {
                        if (std::tolower(static_cast<unsigned char>(part[i])) !=
                            std::tolower(static_cast<unsigned char>(entry_name[i]))) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        cur = entry.path();
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) {
            cur /= part;
        }
    }

    // If file doesn't exist directly, check common subdirectories as fallback
    if (!std::filesystem::exists(cur)) {
        static const std::vector<std::string> kSubdirs = {
            "CHAPDATA", "CHARACTE", "CSTLDATA", "MAGICSPR", "SCENARIO", "SOUND"
        };
        std::string filename = cur.filename().string();
        for (const auto& sub : kSubdirs) {
            std::filesystem::path sub_path = base_dir_ / sub;
            if (std::filesystem::exists(sub_path) && std::filesystem::is_directory(sub_path)) {
                for (const auto& entry : std::filesystem::directory_iterator(sub_path)) {
                    std::string ename = entry.path().filename().string();
                    if (ename.size() == filename.size()) {
                        bool match = true;
                        for (size_t i = 0; i < filename.size(); ++i) {
                            if (std::tolower(static_cast<unsigned char>(filename[i])) !=
                                std::tolower(static_cast<unsigned char>(ename[i]))) {
                                match = false;
                                break;
                            }
                        }
                        if (match) {
                            std::fprintf(stderr, "[DOS] '%s' not found; using %s/%s instead\n",
                                         dos_path.c_str(), sub.c_str(), ename.c_str());
                            return entry.path();
                        }
                    }
                }
            }
        }
    }

    return cur;
}

static bool trace_alloc() {
    static const bool on = std::getenv("GENESIS_TRACE_ALLOC") != nullptr;
    return on;
}

void DosSubsystem::reset_memory(uint16_t psp_seg) {
    blocks_.clear();
    blocks_.push_back({psp_seg, static_cast<uint16_t>(kMemTop - psp_seg)});
}

uint16_t DosSubsystem::dos_alloc_paras(uint16_t paras, uint16_t* largest) {
    // First fit over the gaps after each block. A gap needs one paragraph for
    // the MCB before the new block.
    uint16_t best = 0;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        uint32_t gap_start = static_cast<uint32_t>(blocks_[i].seg) + blocks_[i].paras;
        uint32_t gap_end = (i + 1 < blocks_.size()) ? blocks_[i + 1].seg - 1u : kMemTop;
        if (gap_end <= gap_start + 1) continue;
        uint32_t avail = gap_end - gap_start - 1;
        if (avail >= paras) {
            uint16_t seg = static_cast<uint16_t>(gap_start + 1);
            blocks_.insert(blocks_.begin() + static_cast<long>(i + 1), MemBlock{seg, paras});
            if (trace_alloc())
                std::fprintf(stderr, "[ALLOC] %u paras -> %04X..%04X\n", paras, seg, seg + paras);
            return seg;
        }
        if (avail > best) best = static_cast<uint16_t>(avail);
    }
    if (largest) *largest = best;
    if (trace_alloc()) std::fprintf(stderr, "[ALLOC] %u paras FAILED (largest %u)\n", paras, best);
    return 0;
}

bool DosSubsystem::dos_free_paras(uint16_t seg) {
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].seg == seg) {
            if (trace_alloc()) std::fprintf(stderr, "[FREE] %04X (%u paras)\n", seg, blocks_[i].paras);
            blocks_.erase(blocks_.begin() + static_cast<long>(i));
            return true;
        }
    }
    if (trace_alloc()) std::fprintf(stderr, "[FREE] %04X: no such block\n", seg);
    return false;
}

bool DosSubsystem::dos_realloc_paras(uint16_t seg, uint16_t new_paras, uint16_t* max_paras) {
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].seg != seg) continue;
        uint32_t limit = (i + 1 < blocks_.size()) ? blocks_[i + 1].seg - 1u : kMemTop;
        uint32_t room = limit - seg;
        if (new_paras <= room) {
            if (trace_alloc())
                std::fprintf(stderr, "[RESIZE] %04X %u -> %u paras\n", seg, blocks_[i].paras, new_paras);
            blocks_[i].paras = new_paras;
            return true;
        }
        if (max_paras) *max_paras = static_cast<uint16_t>(room);
        if (trace_alloc())
            std::fprintf(stderr, "[RESIZE] %04X -> %u paras FAILED (max %u)\n", seg, new_paras, room);
        return false;
    }
    if (max_paras) *max_paras = 0;
    if (trace_alloc()) std::fprintf(stderr, "[RESIZE] %04X: no such block\n", seg);
    return false;
}

bool DosSubsystem::trace_files() {
    static const bool on = std::getenv("GENESIS_TRACE_FILES") != nullptr;
    return on;
}

// DOS wildcard match of an 8.3 name against a pattern ("*" and "?"), case-insensitive.
static bool dos_name_match(const std::string& pattern, const std::string& name) {
    auto split = [](const std::string& s, std::string& base, std::string& ext) {
        size_t dot = s.rfind('.');
        if (dot == std::string::npos) { base = s; ext.clear(); }
        else { base = s.substr(0, dot); ext = s.substr(dot + 1); }
    };
    auto part_match = [](const std::string& p, const std::string& n, size_t width) {
        for (size_t i = 0; i < width; ++i) {
            char pc = i < p.size() ? p[i] : ' ';
            char nc = i < n.size() ? n[i] : ' ';
            if (pc == '*') return true;
            if (pc == '?') continue;
            if (std::toupper(static_cast<unsigned char>(pc)) != std::toupper(static_cast<unsigned char>(nc)))
                return false;
        }
        return true;
    };
    std::string pb, pe, nb, ne;
    split(pattern, pb, pe);
    split(name, nb, ne);
    if (nb.size() > 8 || ne.size() > 3) return false;
    return part_match(pb, nb, 8) && part_match(pe, ne, 3);
}

void DosSubsystem::find_first(const std::string& dos_pattern, uint16_t attrs) {
    find_results_.clear();
    find_pos_ = 0;
    std::string pat = dos_pattern;
    std::replace(pat.begin(), pat.end(), '/', '\\');
    size_t slash = pat.rfind('\\');
    std::string dir = (slash == std::string::npos) ? "" : pat.substr(0, slash);
    std::string name_pat = (slash == std::string::npos) ? pat : pat.substr(slash + 1);
    if (dir.size() == 2 && dir[1] == ':') dir.clear();
    if (name_pat.size() >= 2 && name_pat[1] == ':') name_pat = name_pat.substr(2);
    std::filesystem::path real_dir = dir.empty() ? base_dir_ : resolve_path(dir);
    std::error_code ec;
    if (!std::filesystem::is_directory(real_dir, ec)) {
        machine_.cpu.cf = 1;
        machine_.cpu.ax.w = 3;  // path not found
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(real_dir, ec)) {
        std::string n = entry.path().filename().string();
        bool is_dir = entry.is_directory(ec);
        if (is_dir && !(attrs & 0x10)) continue;
        if (n.empty() || n[0] == '.') continue;
        if (!dos_name_match(name_pat, n)) continue;
        FindResult r;
        r.name = n;
        for (char& ch : r.name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        r.attr = is_dir ? 0x10 : 0x20;
        r.size = is_dir ? 0 : static_cast<uint32_t>(entry.file_size(ec));
        find_results_.push_back(r);
    }
    std::sort(find_results_.begin(), find_results_.end(),
              [](const FindResult& a, const FindResult& b) { return a.name < b.name; });
    if (trace_files())
        std::fprintf(stderr, "[FILE] findfirst '%s' -> %zu match(es)\n", dos_pattern.c_str(), find_results_.size());
    find_next();
}

void DosSubsystem::find_next() {
    if (find_pos_ >= find_results_.size()) {
        machine_.cpu.cf = 1;
        machine_.cpu.ax.w = 0x12;  // no more files
        return;
    }
    const FindResult& r = find_results_[find_pos_++];
    uint8_t* dta = machine_.ptr(dta_seg_, dta_off_);
    dta[0x15] = r.attr;
    uint16_t time = 0, date = static_cast<uint16_t>(((1995 - 1980) << 9) | (1 << 5) | 1);
    std::memcpy(dta + 0x16, &time, 2);
    std::memcpy(dta + 0x18, &date, 2);
    std::memcpy(dta + 0x1A, &r.size, 4);
    std::memset(dta + 0x1E, 0, 13);
    std::memcpy(dta + 0x1E, r.name.c_str(), std::min<size_t>(r.name.size(), 12));
    machine_.cpu.cf = 0;
    machine_.cpu.ax.w = 0;
}

// INT 21h AH=29h: parse DS:SI into the FCB at ES:DI (drive byte, 8-char name, 3-char extension).
void DosSubsystem::parse_fcb_name() {
    Cpu& c = machine_.cpu;
    uint8_t flags = c.ax.b.l;
    auto at = [&](uint16_t off) { return static_cast<char>(*machine_.ptr(c.ds, off)); };
    uint16_t si = c.si.w;
    if (flags & 0x01) {
        while (at(si) == ' ' || at(si) == '\t') ++si;
        if (std::strchr(":.;,=+", at(si)) && at(si) != 0) ++si;
        while (at(si) == ' ' || at(si) == '\t') ++si;
    }
    uint8_t* fcb = machine_.ptr(c.es, c.di.w);
    bool wild = false;
    if (at(si) && at(si + 1) == ':') {
        fcb[0] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(at(si))) - 'A' + 1);
        si += 2;
    } else if (!(flags & 0x02)) {
        fcb[0] = 0;
    }
    auto is_term = [](char ch) { return ch == 0 || std::strchr(" \t\r.:;,=+/\"[]<>|", ch) != nullptr; };
    auto fill = [&](int off, int width, bool keep) {
        if (is_term(at(si)) && keep) return;
        int i = 0;
        for (; i < width && !is_term(at(si)); ++i, ++si) {
            char ch = at(si);
            if (ch == '*') { for (; i < width; ++i) fcb[off + i] = '?'; wild = true; ++si; break; }
            if (ch == '?') wild = true;
            fcb[off + i] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(ch)));
        }
        for (; i < width; ++i) fcb[off + i] = ' ';
        while (!is_term(at(si))) ++si;
    };
    fill(1, 8, flags & 0x04);
    if (at(si) == '.') { ++si; fill(9, 3, flags & 0x08); }
    else if (!(flags & 0x08)) std::memset(fcb + 9, ' ', 3);
    c.si.w = si;
    c.ax.b.l = wild ? 1 : 0;
}

bool DosSubsystem::host_file_exists(const std::string& dos_name) const {
    std::error_code ec;
    return std::filesystem::exists(resolve_path(dos_name), ec);
}

// COMMAND.COM /C replacement for the commands the game spawns through system():
// TTL uses `copy SRC DST >$` (the redirect target "$" doubles as the "start the
// game" flag for G.COM).
int DosSubsystem::run_shell_command(const std::string& tail) {
    std::string cmd = tail;
    auto trim = [](std::string x) {
        size_t a = x.find_first_not_of(" \t");
        size_t b = x.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
    };
    cmd = trim(cmd);
    if (cmd.size() >= 2 && (cmd[0] == '/' ) && (cmd[1] == 'c' || cmd[1] == 'C')) cmd = trim(cmd.substr(2));
    std::string redirect;
    size_t gt = cmd.find('>');
    if (gt != std::string::npos) {
        redirect = trim(cmd.substr(gt + 1));
        cmd = trim(cmd.substr(0, gt));
    }
    std::vector<std::string> words;
    {
        std::string w;
        for (char ch : cmd) {
            if (ch == ' ' || ch == '\t') { if (!w.empty()) words.push_back(w), w.clear(); }
            else w += ch;
        }
        if (!w.empty()) words.push_back(w);
    }
    std::string output;
    int rc = 0;
    if (!words.empty()) {
        std::string verb = words[0];
        for (char& ch : verb) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (verb == "copy" && words.size() >= 3) {
            std::error_code ec;
            auto src = resolve_path(words[1]);
            auto dst = resolve_path(words[2]);
            if (std::filesystem::exists(src, ec) &&
                std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec)) {
                output = "        1 file(s) copied\r\n";
            } else {
                output = "File not found - " + words[1] + "\r\n        0 file(s) copied\r\n";
                rc = 1;
            }
            if (trace_files())
                std::fprintf(stderr, "[SHELL] copy %s -> %s: %s\n", src.string().c_str(), dst.string().c_str(),
                             rc ? "failed" : "ok");
        } else {
            std::fprintf(stderr, "[SHELL] unsupported command: %s\n", tail.c_str());
            rc = 1;
        }
    }
    if (!redirect.empty()) {
        std::ofstream f(resolve_path(redirect), std::ios::binary | std::ios::trunc);
        f << output;
    }
    return rc;
}

void DosSubsystem::handle_int21() {
    uint8_t ah = machine_.cpu.ax.b.h;
    uint8_t al = machine_.cpu.ax.b.l;
    static const bool trace_dos = std::getenv("GENESIS_TRACE_DOS") != nullptr;
    if (trace_dos)
        std::fprintf(stderr, "[INT21] AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X\n", machine_.cpu.ax.w,
                     machine_.cpu.bx.w, machine_.cpu.cx.w, machine_.cpu.dx.w, machine_.cpu.ds, machine_.cpu.es);

    switch (ah) {
    case 0x0E: // Select default drive
        machine_.cpu.ax.b.l = 3; // 3 drives (A, B, C)
        break;
    case 0x19: // Get default drive
        machine_.cpu.ax.b.l = 2; // C:
        break;
    case 0x1A: // Set DTA
        dta_seg_ = machine_.cpu.ds;
        dta_off_ = pdx();
        break;
    case 0x25: // Set interrupt vector
    {
        uint8_t int_no = al;
        uint32_t ivt_addr = static_cast<uint32_t>(int_no) * 4;
        *reinterpret_cast<uint16_t*>(&machine_.ram[ivt_addr]) = machine_.cpu.dx.w;
        *reinterpret_cast<uint16_t*>(&machine_.ram[ivt_addr + 2]) = machine_.cpu.ds;
        if (std::getenv("GENESIS_TRACE_INT"))
            std::fprintf(stderr, "[INT] set vector %02X -> %04X:%04X%s\n", int_no, machine_.cpu.ds,
                         machine_.cpu.dx.w,
                         machine_.resolve_function(machine_.cpu.ds, machine_.cpu.dx.w) ? "" : " (NOT LIFTED)");
        break;
    }
    case 0x35: // Get interrupt vector
    {
        uint8_t int_no = al;
        uint32_t ivt_addr = static_cast<uint32_t>(int_no) * 4;
        machine_.cpu.bx.w = *reinterpret_cast<const uint16_t*>(&machine_.ram[ivt_addr]);
        machine_.cpu.es = *reinterpret_cast<const uint16_t*>(&machine_.ram[ivt_addr + 2]);
        break;
    }
    case 0x2A: // Get system date
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm* tm = std::localtime(&t);
        machine_.cpu.cx.w = static_cast<uint16_t>(tm->tm_year + 1900);
        machine_.cpu.dx.b.h = static_cast<uint8_t>(tm->tm_mon + 1);
        machine_.cpu.dx.b.l = static_cast<uint8_t>(tm->tm_mday);
        machine_.cpu.ax.b.l = static_cast<uint8_t>(tm->tm_wday);
        break;
    }
    case 0x2C: // Get system time
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm* tm = std::localtime(&t);
        machine_.cpu.cx.b.h = static_cast<uint8_t>(tm->tm_hour);
        machine_.cpu.cx.b.l = static_cast<uint8_t>(tm->tm_min);
        machine_.cpu.dx.b.h = static_cast<uint8_t>(tm->tm_sec);
        machine_.cpu.dx.b.l = 0;
        break;
    }
    case 0x30: // Get DOS Version
        machine_.cpu.ax.b.l = 6;  // Major (DOS 6)
        machine_.cpu.ax.b.h = 22; // Minor (.22)
        machine_.cpu.bx.w = 0;
        machine_.cpu.cx.w = 0;
        break;
    case 0x3C: // Create File
    {
        std::string path_str = read_dos_string(machine_.cpu.ds, pdx());
        std::filesystem::path real_path = resolve_path(path_str);

        int handle_idx = -1;
        for (size_t i = 5; i < kMaxHandles; ++i) {
            if (!handles_[i].is_open) {
                handle_idx = static_cast<int>(i);
                break;
            }
        }
        if (handle_idx < 0) {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 4; // Too many open files
            return;
        }

        handles_[handle_idx].stream.open(real_path, std::ios::in | std::ios::out | std::ios::trunc | std::ios::binary);
        if (!handles_[handle_idx].stream.is_open()) {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 3; // Path not found
            return;
        }

        handles_[handle_idx].path = real_path.string();
        handles_[handle_idx].is_open = true;
        machine_.cpu.cf = 0;
        machine_.cpu.ax.w = static_cast<uint16_t>(handle_idx);
        break;
    }
    case 0x3D: // Open File
    {
        std::string path_str = read_dos_string(machine_.cpu.ds, pdx());
        std::filesystem::path real_path = resolve_path(path_str);

        int handle_idx = -1;
        for (size_t i = 5; i < kMaxHandles; ++i) {
            if (!handles_[i].is_open) {
                handle_idx = static_cast<int>(i);
                break;
            }
        }
        if (handle_idx < 0) {
            std::fprintf(stderr, "[DOS Open] Too many open files: %s\n", path_str.c_str());
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 4;
            return;
        }

        std::ios_base::openmode mode = std::ios::binary;
        uint8_t access = al & 3;
        // Read-only opens are served from the translation overlay when it has the file.
        if (access == 0 && !overlay_dir_.empty()) {
            std::error_code ec;
            std::filesystem::path rel = std::filesystem::relative(real_path, base_dir_, ec);
            if (!ec && !rel.empty() && rel.native()[0] != '.') {
                std::filesystem::path over = overlay_dir_ / rel;
                if (std::filesystem::is_regular_file(over, ec)) {
                    if (trace_files()) std::fprintf(stderr, "[DOS Open] overlay: %s\n", over.string().c_str());
                    real_path = over;
                }
            }
        }
        if (access == 0) mode |= std::ios::in;
        else if (access == 1) mode |= std::ios::out;
        else mode |= std::ios::in | std::ios::out;

        handles_[handle_idx].stream.open(real_path, mode);
        if (!handles_[handle_idx].stream.is_open()) {
            if (std::getenv("GENESIS_TRACE_FILES"))
                std::fprintf(stderr, "[DOS Open] FAILED: '%s' -> '%s'\n", path_str.c_str(), real_path.string().c_str());
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 2; // File not found
            return;
        }

        if (std::getenv("GENESIS_TRACE_FILES"))
            std::fprintf(stderr, "[DOS Open] OK: '%s' -> '%s' (handle %d)\n", path_str.c_str(), real_path.string().c_str(), handle_idx);
        handles_[handle_idx].path = real_path.string();
        handles_[handle_idx].is_open = true;
        machine_.cpu.cf = 0;
        machine_.cpu.ax.w = static_cast<uint16_t>(handle_idx);
        break;
    }
    case 0x3E: // Close File
    {
        uint16_t h = machine_.cpu.bx.w;
        if (h < kMaxHandles && handles_[h].is_open) {
            handles_[h].stream.close();
            handles_[h].is_open = false;
            machine_.cpu.cf = 0;
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 6; // Invalid handle
        }
        break;
    }
    case 0x3F: // Read from File
    {
        uint16_t h = machine_.cpu.bx.w;
        uint32_t count = machine_.pm ? machine_.cpu.cx.e : machine_.cpu.cx.w;
        uint8_t* dst = machine_.ptr(machine_.cpu.ds, pdx());

        if (h < kMaxHandles && handles_[h].is_open) {
            {
                uint32_t lin = machine_.phys_addr(machine_.cpu.ds, pdx());
                if (g_watch_lo != g_watch_hi && lin + count > g_watch_lo && lin < g_watch_hi)
                    watch_hit(machine_, lin, count, 0x3F);
            }
            handles_[h].stream.read(reinterpret_cast<char*>(dst), count);
            std::streamsize bytes_read = handles_[h].stream.gcount();
            machine_.cpu.cf = 0;
            if (machine_.pm) machine_.cpu.ax.e = static_cast<uint32_t>(bytes_read);
            else machine_.cpu.ax.w = static_cast<uint16_t>(bytes_read);
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 6;
        }
        break;
    }
    case 0x40: // Write to File
    {
        uint16_t h = machine_.cpu.bx.w;
        uint32_t count = machine_.pm ? machine_.cpu.cx.e : machine_.cpu.cx.w;
        const uint8_t* src = machine_.ptr(machine_.cpu.ds, pdx());

        if (h < kMaxHandles && handles_[h].is_open) {
            handles_[h].stream.write(reinterpret_cast<const char*>(src), count);
            machine_.cpu.cf = 0;
            if (machine_.pm) machine_.cpu.ax.e = count;
            else machine_.cpu.ax.w = static_cast<uint16_t>(count);
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 6;
        }
        break;
    }
    case 0x41: // Delete File (UNLINK)
    {
        std::string path_str = read_dos_string(machine_.cpu.ds, pdx());
        std::filesystem::path real_path = resolve_path(path_str);

        std::error_code ec;
        if (std::filesystem::remove(real_path, ec)) {
            machine_.cpu.cf = 0;
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 2; // File not found
        }
        break;
    }
    case 0x42: // Seek File (LSEEK)
    {
        uint16_t h = machine_.cpu.bx.w;
        int32_t offset = static_cast<int32_t>((static_cast<uint32_t>(machine_.cpu.cx.w) << 16) | machine_.cpu.dx.w);
        std::ios::seekdir dir = std::ios::beg;
        if (al == 1) dir = std::ios::cur;
        else if (al == 2) dir = std::ios::end;

        if (h < kMaxHandles && handles_[h].is_open) {
            handles_[h].stream.clear();
            handles_[h].stream.seekg(offset, dir);
            std::streampos pos = handles_[h].stream.tellg();
            machine_.cpu.cf = 0;
            machine_.cpu.dx.w = static_cast<uint16_t>(static_cast<uint32_t>(pos) >> 16);
            machine_.cpu.ax.w = static_cast<uint16_t>(static_cast<uint32_t>(pos) & 0xFFFF);
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 6;
        }
        break;
    }
    case 0x48: // Allocate Memory
    {
        uint16_t largest = 0;
        uint16_t seg = dos_alloc_paras(machine_.cpu.bx.w, &largest);
        if (seg) {
            machine_.cpu.cf = 0;
            machine_.cpu.ax.w = seg;
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 8;  // insufficient memory
            machine_.cpu.bx.w = largest;
        }
        break;
    }
    case 0x49: // Free Memory
        if (dos_free_paras(machine_.cpu.es)) {
            machine_.cpu.cf = 0;
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 9;  // invalid memory block address
        }
        break;
    case 0x4A: // Reallocate Memory
    {
        uint16_t max_paras = 0;
        if (dos_realloc_paras(machine_.cpu.es, machine_.cpu.bx.w, &max_paras)) {
            machine_.cpu.cf = 0;
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 8;
            machine_.cpu.bx.w = max_paras;
        }
        break;
    }
    case 0x4B: // Load and Execute Program (EXEC)
    {
        std::string prog = read_dos_string(machine_.cpu.ds, pdx());
        uint32_t tail_off;
        uint16_t tail_seg;
        if (machine_.pm) {  // DOS/4GW parameter block: env 16:32, command tail 16:32
            std::memcpy(&tail_off, machine_.ptr(machine_.cpu.es, machine_.cpu.bx.e + 6), 4);
            std::memcpy(&tail_seg, machine_.ptr(machine_.cpu.es, machine_.cpu.bx.e + 10), 2);
        } else {
            tail_off = *reinterpret_cast<const uint16_t*>(machine_.ptr(machine_.cpu.es, machine_.cpu.bx.w + 2));
            tail_seg = *reinterpret_cast<const uint16_t*>(machine_.ptr(machine_.cpu.es, machine_.cpu.bx.w + 4));
        }

        std::string cmd_args;
        if (tail_seg != 0) {
            uint8_t len = *machine_.ptr(tail_seg, tail_off);
            for (uint8_t i = 1; i <= len; ++i) {
                char ch = static_cast<char>(*machine_.ptr(tail_seg, tail_off + i));
                if (ch == '\r') break;
                cmd_args += ch;
            }
        }
        std::cout << "[DOS] INT 21h AH=4Bh EXEC: " << prog << " '" << cmd_args << "'\n";
        {
            std::string up = prog;
            for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (up.size() >= 11 && up.compare(up.size() - 11, 11, "COMMAND.COM") == 0) {
                last_exit_code_ = run_shell_command(cmd_args);
                machine_.cpu.cf = 0;
                break;
            }
        }
        {
            // spawn*() reads the child's exit code with AH=4Dh afterwards: MAIN
            // decides where to go next (town, battle, quit) from CHP's code.
            int code = machine_.exec_module(prog, cmd_args);
            if (code >= 0) last_exit_code_ = code;
        }
        break;
    }
    case 0x4C: // Terminate Process
        std::cout << "[DOS] Process exit with code " << static_cast<int>(al) << "\n";
        machine_.running = false;
        machine_.exit_code = al;
        throw ProcessTerminated{al};
    case 0x4D: // Get Return Code of Child Process
        machine_.cpu.ax.b.l = static_cast<uint8_t>(last_exit_code_);
        machine_.cpu.ax.b.h = 0; // Normal exit
        break;
    case 0x01: // Read character with echo
    case 0x07: // Direct character input, no echo
    case 0x08: // Character input, no echo
    {
        Cpu saved = machine_.cpu;
        machine_.cpu.ax.b.h = 0x00;
        machine_.input->handle_int16();  // waits for a key
        uint8_t ch = machine_.cpu.ax.b.l;
        machine_.cpu = saved;
        machine_.cpu.ax.b.l = ch;
        break;
    }
    case 0x0B: // Check standard input status
        machine_.cpu.ax.b.l = machine_.input->key_available() ? 0xFF : 0x00;
        break;
    case 0x29: // Parse filename into FCB
        parse_fcb_name();
        break;
    case 0x2F: // Get DTA
        machine_.cpu.es = dta_seg_;
        machine_.cpu.bx.w = dta_off_;
        break;
    case 0x43: // Get/set file attributes
    {
        std::string path_str = read_dos_string(machine_.cpu.ds, pdx());
        std::filesystem::path real_path = resolve_path(path_str);
        std::error_code ec;
        if (!std::filesystem::exists(real_path, ec)) {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 2;  // file not found
        } else {
            machine_.cpu.cf = 0;
            if (al == 0x00) machine_.cpu.cx.w = std::filesystem::is_directory(real_path, ec) ? 0x10 : 0x20;
        }
        if (trace_files())
            std::fprintf(stderr, "[FILE] attrib '%s' -> %s\n", path_str.c_str(), machine_.cpu.cf ? "missing" : "exists");
        break;
    }
    case 0x44: // IOCTL
        if (al == 0x00) {  // get device information
            uint16_t h = machine_.cpu.bx.w;
            if (h <= 4) {
                machine_.cpu.dx.w = (h == 0) ? 0x80D3 : (h <= 2 ? 0x80D3 : 0x80C0);  // character device
                machine_.cpu.cf = 0;
            } else if (h < kMaxHandles && handles_[h].is_open) {
                machine_.cpu.dx.w = 0x0002;  // disk file on drive C:
                machine_.cpu.cf = 0;
            } else {
                machine_.cpu.cf = 1;
                machine_.cpu.ax.w = 6;  // invalid handle
            }
        } else {
            machine_.cpu.cf = 0;
        }
        break;
    case 0x47: // Get current directory (without drive or leading backslash)
    {
        std::string d = cwd_;
        std::replace(d.begin(), d.end(), '/', '\\');
        for (char& ch : d) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        uint32_t si = machine_.pm ? machine_.cpu.si.e : machine_.cpu.si.w;
        std::memcpy(machine_.ptr(machine_.cpu.ds, si), d.c_str(), d.size() + 1);
        machine_.cpu.ax.w = 0x0100;
        machine_.cpu.cf = 0;
        break;
    }
    case 0x3B: // Change directory
    {
        std::string path = read_dos_string(machine_.cpu.ds, pdx());
        std::filesystem::path target = resolve_path(path);
        std::error_code ec;
        if (std::filesystem::is_directory(target, ec)) {
            std::filesystem::path rel = std::filesystem::relative(target, base_dir_, ec);
            cwd_ = (ec || rel == ".") ? std::string() : rel.generic_string();
            if (cwd_.rfind("..", 0) == 0) cwd_.clear();
            machine_.cpu.cf = 0;
            if (trace_files()) std::fprintf(stderr, "[FILE] chdir '%s' -> '\\%s'\n", path.c_str(), cwd_.c_str());
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 3;  // path not found
        }
        break;
    }
    case 0x4E: // Find first matching file
        find_first(read_dos_string(machine_.cpu.ds, pdx()), machine_.cpu.cx.w);
        break;
    case 0x4F: // Find next matching file
        find_next();
        break;
    default:
    {
        static bool warned[256];
        if (!warned[ah]) {
            warned[ah] = true;
            std::fprintf(stderr, "[DOS] Unhandled INT 21h AH=%02Xh (AL=%02X)\n", ah, al);
        }
        machine_.cpu.cf = 0;
        break;
    }
    }
}

void DosSubsystem::handle_int2f() {
    uint16_t ax = machine_.cpu.ax.w;
    if (ax == 0x4300) { // XMS Installation Check
        machine_.cpu.ax.b.l = 0x80; // XMS driver installed
    } else if (ax == 0x4310) { // Get XMS Driver Entry Point
        // Return synthetic far call entry point at 0x0070:0x0010
        machine_.cpu.es = 0x0070;
        machine_.cpu.bx.w = 0x0010;
    }
}

void DosSubsystem::handle_xms_call() {
    uint8_t ah = machine_.cpu.ax.b.h;
    switch (ah) {
    case 0x00: // Get XMS Version
        machine_.cpu.ax.w = 0x0300; // XMS 3.0
        machine_.cpu.bx.w = 0x0301; // Driver revision
        machine_.cpu.dx.w = 1;      // HMA exists
        break;
    case 0x03: // Global Enable A20
    case 0x05: // Local Enable A20
        machine_.cpu.ax.w = 1;      // Success
        machine_.cpu.bx.b.l = 0;
        break;
    case 0x04: // Global Disable A20
    case 0x06: // Local Disable A20
        machine_.cpu.ax.w = 1;      // Success
        machine_.cpu.bx.b.l = 0;
        break;
    case 0x07: // Query A20
        machine_.cpu.ax.w = 1;      // A20 is enabled
        machine_.cpu.bx.b.l = 0;
        break;
    case 0x08: // Query Free Extended Memory
        machine_.cpu.ax.w = 0x7FFF; // Total free (in KB) ~32MB
        machine_.cpu.dx.w = 0x7FFF; // Largest free block
        machine_.cpu.bx.b.l = 0;
        break;
    case 0x09: // Allocate Extended Memory Block
    {
        uint16_t size_kb = machine_.cpu.dx.w;
        for (size_t i = 1; i < kMaxXmsBlocks; ++i) {
            if (!xms_blocks_[i].in_use) {
                xms_blocks_[i].handle = static_cast<uint32_t>(i);
                xms_blocks_[i].size_bytes = static_cast<uint32_t>(size_kb) * 1024;
                xms_blocks_[i].offset = xms_next_offset_;
                xms_next_offset_ += xms_blocks_[i].size_bytes;
                xms_blocks_[i].in_use = true;

                machine_.cpu.ax.w = 1; // Success
                machine_.cpu.dx.w = static_cast<uint16_t>(i);
                return;
            }
        }
        machine_.cpu.ax.w = 0; // Failed
        machine_.cpu.bx.b.l = 0xA0; // All extended memory allocated
        break;
    }
    case 0x0A: // Free Extended Memory Block
    {
        uint16_t h = machine_.cpu.dx.w;
        if (h < kMaxXmsBlocks && xms_blocks_[h].in_use) {
            xms_blocks_[h].in_use = false;
            machine_.cpu.ax.w = 1;
        } else {
            machine_.cpu.ax.w = 0;
            machine_.cpu.bx.b.l = 0xA2; // Invalid handle
        }
        break;
    }
    case 0x0B: // Move Extended Memory Block
    {
        uint16_t seg = machine_.cpu.ds;
        uint16_t off = machine_.cpu.si.w;
        const uint8_t* p = machine_.ptr(seg, off);

        uint32_t length = *reinterpret_cast<const uint32_t*>(p + 0);
        uint16_t src_handle = *reinterpret_cast<const uint16_t*>(p + 4);
        uint32_t src_offset = *reinterpret_cast<const uint32_t*>(p + 6);
        uint16_t dst_handle = *reinterpret_cast<const uint16_t*>(p + 10);
        uint32_t dst_offset = *reinterpret_cast<const uint32_t*>(p + 12);

        uint8_t* src_ptr = nullptr;
        if (src_handle == 0) {
            // Conventional memory (seg:off in src_offset)
            uint16_t c_off = static_cast<uint16_t>(src_offset & 0xFFFF);
            uint16_t c_seg = static_cast<uint16_t>(src_offset >> 16);
            src_ptr = machine_.ptr(c_seg, c_off);
        } else if (src_handle < kMaxXmsBlocks && xms_blocks_[src_handle].in_use) {
            src_ptr = &machine_.ram[xms_blocks_[src_handle].offset + src_offset];
        }

        uint8_t* dst_ptr = nullptr;
        if (dst_handle == 0) {
            uint16_t c_off = static_cast<uint16_t>(dst_offset & 0xFFFF);
            uint16_t c_seg = static_cast<uint16_t>(dst_offset >> 16);
            dst_ptr = machine_.ptr(c_seg, c_off);
        } else if (dst_handle < kMaxXmsBlocks && xms_blocks_[dst_handle].in_use) {
            dst_ptr = &machine_.ram[xms_blocks_[dst_handle].offset + dst_offset];
        }

        if (src_ptr && dst_ptr) {
            std::memmove(dst_ptr, src_ptr, length);
            machine_.cpu.ax.w = 1; // Success
        } else {
            machine_.cpu.ax.w = 0;
            machine_.cpu.bx.b.l = 0xA3; // Invalid offset
        }
        break;
    }
    case 0x0C: // Lock Extended Memory Block
    {
        uint16_t h = machine_.cpu.dx.w;
        if (h < kMaxXmsBlocks && xms_blocks_[h].in_use) {
            uint32_t linear = xms_blocks_[h].offset;
            machine_.cpu.dx.w = static_cast<uint16_t>(linear >> 16);
            machine_.cpu.bx.w = static_cast<uint16_t>(linear & 0xFFFF);
            machine_.cpu.ax.w = 1; // Success
        } else {
            machine_.cpu.ax.w = 0;
            machine_.cpu.bx.b.l = 0xA2; // Invalid handle
        }
        break;
    }
    case 0x0D: // Unlock Extended Memory Block
    {
        uint16_t h = machine_.cpu.dx.w;
        if (h < kMaxXmsBlocks && xms_blocks_[h].in_use) {
            machine_.cpu.ax.w = 1; // Success
        } else {
            machine_.cpu.ax.w = 0;
            machine_.cpu.bx.b.l = 0xA2;
        }
        break;
    }
    default:
        std::fprintf(stderr, "[DOS] Unhandled XMS function AH=%02Xh\n", ah);
        machine_.cpu.ax.w = 0;
        machine_.cpu.bx.b.l = 0x80; // Function not implemented
        break;
    }
}

}  // namespace gen
