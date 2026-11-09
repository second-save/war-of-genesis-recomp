#include <cstdlib>
// DOS subsystem implementation for filesystem virtualization, INT 21h, and XMS driver.
#include "dos.h"
#include "runtime/machine.h"

#include <iostream>
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

std::string DosSubsystem::read_dos_string(uint16_t seg, uint16_t off) const {
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

    // Remove leading slashes
    while (!clean.empty() && clean[0] == '/') {
        clean = clean.substr(1);
    }

    // Remove leading "./"
    while (clean.rfind("./", 0) == 0) {
        clean = clean.substr(2);
    }

    std::filesystem::path cur = base_dir_;
    std::stringstream ss(clean);
    std::string part;

    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            cur = cur.parent_path();
            continue;
        }

        // Case-insensitive search in directory
        bool found = false;
        if (std::filesystem::exists(cur) && std::filesystem::is_directory(cur)) {
            for (const auto& entry : std::filesystem::directory_iterator(cur)) {
                std::string entry_name = entry.path().filename().string();
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
                            return entry.path();
                        }
                    }
                }
            }
        }
    }

    return cur;
}

uint16_t DosSubsystem::dos_alloc_paras(uint16_t paras) {
    uint16_t seg = heap_next_seg_;
    heap_next_seg_ = static_cast<uint16_t>(heap_next_seg_ + paras + 1);
    if (std::getenv("GENESIS_TRACE_ALLOC"))
        std::fprintf(stderr, "[ALLOC] %u paras -> %04X..%04X\n", paras, seg, heap_next_seg_);
    return seg;
}

bool DosSubsystem::dos_free_paras(uint16_t seg) {
    (void)seg;
    return true;
}

bool DosSubsystem::dos_realloc_paras(uint16_t seg, uint16_t new_paras) {
    (void)seg; (void)new_paras;
    return true;
}

void DosSubsystem::handle_int21() {
    uint8_t ah = machine_.cpu.ax.b.h;
    uint8_t al = machine_.cpu.ax.b.l;

    switch (ah) {
    case 0x0E: // Select default drive
        machine_.cpu.ax.b.l = 3; // 3 drives (A, B, C)
        break;
    case 0x19: // Get default drive
        machine_.cpu.ax.b.l = 2; // C:
        break;
    case 0x1A: // Set DTA
        dta_seg_ = machine_.cpu.ds;
        dta_off_ = machine_.cpu.dx.w;
        break;
    case 0x25: // Set interrupt vector
    {
        uint8_t int_no = al;
        uint32_t ivt_addr = static_cast<uint32_t>(int_no) * 4;
        *reinterpret_cast<uint16_t*>(&machine_.ram[ivt_addr]) = machine_.cpu.dx.w;
        *reinterpret_cast<uint16_t*>(&machine_.ram[ivt_addr + 2]) = machine_.cpu.ds;
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
        std::string path_str = read_dos_string(machine_.cpu.ds, machine_.cpu.dx.w);
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
        std::string path_str = read_dos_string(machine_.cpu.ds, machine_.cpu.dx.w);
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
        uint16_t count = machine_.cpu.cx.w;
        uint8_t* dst = machine_.ptr(machine_.cpu.ds, machine_.cpu.dx.w);

        if (h < kMaxHandles && handles_[h].is_open) {
            handles_[h].stream.read(reinterpret_cast<char*>(dst), count);
            std::streamsize bytes_read = handles_[h].stream.gcount();
            machine_.cpu.cf = 0;
            machine_.cpu.ax.w = static_cast<uint16_t>(bytes_read);
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 6;
        }
        break;
    }
    case 0x40: // Write to File
    {
        uint16_t h = machine_.cpu.bx.w;
        uint16_t count = machine_.cpu.cx.w;
        const uint8_t* src = machine_.ptr(machine_.cpu.ds, machine_.cpu.dx.w);

        if (h < kMaxHandles && handles_[h].is_open) {
            handles_[h].stream.write(reinterpret_cast<const char*>(src), count);
            machine_.cpu.cf = 0;
            machine_.cpu.ax.w = count;
        } else {
            machine_.cpu.cf = 1;
            machine_.cpu.ax.w = 6;
        }
        break;
    }
    case 0x41: // Delete File (UNLINK)
    {
        std::string path_str = read_dos_string(machine_.cpu.ds, machine_.cpu.dx.w);
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
        uint16_t paras = machine_.cpu.bx.w;
        uint16_t seg = dos_alloc_paras(paras);
        machine_.cpu.cf = 0;
        machine_.cpu.ax.w = seg;
        break;
    }
    case 0x49: // Free Memory
        dos_free_paras(machine_.cpu.es);
        machine_.cpu.cf = 0;
        break;
    case 0x4A: // Reallocate Memory
        dos_realloc_paras(machine_.cpu.es, machine_.cpu.bx.w);
        machine_.cpu.cf = 0;
        break;
    case 0x4B: // Load and Execute Program (EXEC)
    {
        std::string prog = read_dos_string(machine_.cpu.ds, machine_.cpu.dx.w);
        uint16_t tail_off = *reinterpret_cast<const uint16_t*>(machine_.ptr(machine_.cpu.es, machine_.cpu.bx.w + 2));
        uint16_t tail_seg = *reinterpret_cast<const uint16_t*>(machine_.ptr(machine_.cpu.es, machine_.cpu.bx.w + 4));

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
        machine_.exec_module(prog, cmd_args);
        break;
    }
    case 0x4C: // Terminate Process
        std::cout << "[DOS] Process exit with code " << static_cast<int>(al) << "\n";
        machine_.running = false;
        machine_.exit_code = al;
        throw ProcessTerminated{al};
    case 0x4D: // Get Return Code of Child Process
        machine_.cpu.ax.b.l = 0; // Return code 0
        machine_.cpu.ax.b.h = 0; // Normal exit
        break;
    default:
        machine_.cpu.cf = 0;
        break;
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
