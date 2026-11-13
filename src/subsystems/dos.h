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

    bool host_file_exists(const std::string& dos_name) const;
    int run_shell_command(const std::string& tail);

    // Interrupt handlers
    void handle_int21();
    void handle_int2f();
    void handle_xms_call();

    // Memory allocator (DOS paragraphs). Blocks live in conventional memory
    // [psp, kMemTop); each block has an (unwritten) MCB paragraph before it.
    struct MemBlock { uint16_t seg; uint16_t paras; };
    static constexpr uint16_t kMemTop = 0xA000;
    // Returns segment, or 0 on failure with *largest set to the largest free block.
    uint16_t dos_alloc_paras(uint16_t paras, uint16_t* largest = nullptr);
    bool dos_free_paras(uint16_t seg);
    // On failure *max_paras is set to the largest size the block can grow to.
    bool dos_realloc_paras(uint16_t seg, uint16_t new_paras, uint16_t* max_paras = nullptr);
    // A newly loaded program owns all memory from its PSP to kMemTop.
    void reset_memory(uint16_t psp_seg);

    std::vector<MemBlock> memory_blocks() const { return blocks_; }
    void set_memory_blocks(std::vector<MemBlock> b) { blocks_ = std::move(b); }

private:
    // Pointer argument in DX (EDX for protected-mode programs).
    uint32_t pdx() const;
    Machine& machine_;
    std::filesystem::path base_dir_;

    // File descriptors (0..4 are standard I/O)
    static constexpr size_t kMaxHandles = 64;
    FileHandle handles_[kMaxHandles];

    // Disk Transfer Area (DTA) address
    uint16_t dta_seg_ = 0;
    uint32_t dta_off_ = 0x80;
    int last_exit_code_ = 0;

    // Allocated blocks, sorted by segment.
    std::vector<MemBlock> blocks_;

    // XMS management (extended memory allocated at high RAM offsets)
    static constexpr size_t kMaxXmsBlocks = 32;
    XmsBlock xms_blocks_[kMaxXmsBlocks];
    uint32_t xms_next_offset_ = 0x200000; // 2 MB offset in machine RAM

    // FindFirst/FindNext state (INT 21h AH=4Eh/4Fh)
    struct FindResult { std::string name; uint8_t attr; uint32_t size; };
    std::vector<FindResult> find_results_;
    size_t find_pos_ = 0;
    void find_first(const std::string& dos_pattern, uint16_t attrs);
    void find_next();
    void parse_fcb_name();
    static bool trace_files();

    // Helper to read null-terminated string from memory
    std::string read_dos_string(uint16_t seg, uint32_t off) const;
};

}  // namespace gen
