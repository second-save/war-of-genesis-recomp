// Machine encapsulates the CPU, flat RAM, I/O ports, interrupt routing, and subsystems.
#pragma once

#include "cpu.h"
#include "module.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace gen {

class VideoSubsystem;
class AudioSubsystem;
class InputSubsystem;
class DosSubsystem;

class Machine {
public:
    static constexpr size_t kRamSize = 16 * 1024 * 1024; // 16 MB

    Cpu cpu{};
    std::unique_ptr<uint8_t[]> ram;

    // Subsystems
    std::unique_ptr<VideoSubsystem> video;
    std::unique_ptr<AudioSubsystem> audio;
    std::unique_ptr<InputSubsystem> input;
    std::unique_ptr<DosSubsystem> dos;

    bool running = true;
    int exit_code = 0;
    uint32_t tick_counter = 0;
    uint32_t last_tick_time_ = 0;

    Machine();
    ~Machine();

    // Memory address translation
    inline uint32_t phys_addr(uint16_t seg, uint32_t off) const {
        return (static_cast<uint32_t>(seg) << 4) + off;
    }

    inline uint8_t* ptr(uint16_t seg, uint32_t off) {
        return &ram[phys_addr(seg, off) & (kRamSize - 1)];
    }

    inline const uint8_t* ptr(uint16_t seg, uint32_t off) const {
        return &ram[phys_addr(seg, off) & (kRamSize - 1)];
    }

    // Module registration & function dispatch
    void register_module(const LiftedModule* mod);
    void register_synthetic_function(uint16_t seg, uint16_t off, FunctionPointer fn);
    FunctionPointer resolve_function(uint16_t seg, uint16_t off) const;

    // Control flow calls from lifted code
    void call_near(uint16_t cs, uint16_t ip);
    void call_far(uint16_t cs, uint16_t ip);

    // Diagnostics / Fallbacks
    void invalid_code(uint16_t seg, uint16_t off);
    void unimplemented(uint16_t seg, uint16_t off, const char* msg);
    void fpu_emulated(uint8_t int_no, uint16_t seg, uint16_t off, const char* msg);

    // Hardware I/O Ports
    uint8_t in8(uint16_t port);
    uint16_t in16(uint16_t port);
    uint32_t in32(uint16_t port);
    void out8(uint16_t port, uint8_t val);
    void out16(uint16_t port, uint16_t val);
    void out32(uint16_t port, uint32_t val);

    // Interrupts & Events
    void interrupt(uint8_t int_no, uint16_t seg, uint16_t off);
    void poll_tick();
    void frame_sync();
    void idle();

    const LiftedModule* current_module() const { return current_module_; }

    // MZ loader & execution
    bool load_mz(const std::string& path, uint16_t load_seg, const std::string& cmd_line = "");
    int run_module(const LiftedModule* mod, const std::string& cmd_line = "");
    int exec_module(const std::string& prog_path, const std::string& cmd_line);

private:
    const LiftedModule* current_module_ = nullptr;
    std::unordered_map<uint32_t, FunctionPointer> dispatch_table_;
    std::unordered_map<uint32_t, FunctionPointer> linear_dispatch_table_;
    std::unordered_map<uint32_t, FunctionPointer> synthetic_dispatch_table_;
};

}  // namespace gen
