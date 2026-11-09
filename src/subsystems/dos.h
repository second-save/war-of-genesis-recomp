// DOS subsystem: virtual filesystem, file handles, memory management, and INT 21h / INT 2Fh.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>

namespace gen {

class Machine;

struct FileHandle {
    std::fstream stream;
    std::string path;
    bool is_open = false;
};

struct XmsBlock {
    uint32_t handle = 0;
    uint32_t size_bytes = 0;
    uint32_t offset = 0;
    bool in_use = false;
};

class DosSubsystem {
public:
    DosSubsystem(Machine& machine);
    ~DosSubsystem();

    void set_base_dir(const std::filesystem::path& base_dir);
    const std::filesystem::path& base_dir() const { return base_dir_; }

    // Path resolution (case-insensitive, backslash normalization)
    std::filesystem::path resolve_path(const std::string& dos_path) const;

    // Interrupt handlers
    void handle_int21();
    void handle_int2f();
    void handle_xms_call();

    // Memory allocator (DOS paragraphs)
    uint16_t dos_alloc_paras(uint16_t paras);
    bool dos_free_paras(uint16_t seg);
    bool dos_realloc_paras(uint16_t seg, uint16_t new_paras);

    uint16_t heap_next_seg() const { return heap_next_seg_; }
    void set_heap_next_seg(uint16_t seg) { heap_next_seg_ = seg; }

private:
    Machine& machine_;
    std::filesystem::path base_dir_;

    // File descriptors (0..4 are standard I/O)
    static constexpr size_t kMaxHandles = 64;
    FileHandle handles_[kMaxHandles];

    // Disk Transfer Area (DTA) address
    uint16_t dta_seg_ = 0;
    uint16_t dta_off_ = 0x80;

    // Dynamic heap allocation for DOS malloc (starts after program image)
    uint16_t heap_next_seg_ = 0x6000;

    // XMS management (extended memory allocated at high RAM offsets)
    static constexpr size_t kMaxXmsBlocks = 32;
    XmsBlock xms_blocks_[kMaxXmsBlocks];
    uint32_t xms_next_offset_ = 0x200000; // 2 MB offset in machine RAM

    // Helper to read null-terminated string from memory
    std::string read_dos_string(uint16_t seg, uint16_t off) const;
};

}  // namespace gen
