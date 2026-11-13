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
#include <functional>

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
    bool quit_requested = false;  // window closed / Cmd+Q: leave the launcher loop
    int exit_code = 0;
    uint32_t tick_counter = 0;
    uint32_t last_tick_time_ = 0;

    Machine();
    ~Machine();

    // Protected mode (DOS/4GW programs): segment registers hold selectors
    // whose base comes from sel_base (indexed by selector >> 3).
    bool pm = false;
    uint32_t sel_base[8192]{};
    uint32_t sel_limit[8192]{};
    bool sel_used[8192]{};

    // Memory address translation
    inline uint32_t phys_addr(uint16_t seg, uint32_t off) const {
        if (pm) return sel_base[seg >> 3] + off;
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

    // Hardware-style interrupt delivery into lifted handlers. The interrupted
    // CPU state is saved and restored around the handler.
    // call_vector: dispatch through the IVT with an IRET frame (FLAGS, CS, IP).
    bool call_vector(uint8_t int_no);
    // call_handler: call seg:off with an IRET frame (iret_frame) or a far-call
    // frame (CS, IP); setup adjusts registers before the call.
    bool call_handler(uint16_t seg, uint16_t off, bool iret_frame,
                      const std::function<void(Cpu&)>& setup = {});
    // Run due PIT timer ticks and queued keyboard/mouse events.
    void service_interrupts();
    bool in_interrupt() const { return isr_depth_ > 0; }
    // Current position on the PIT timeline (1193182 Hz clocks).
    uint64_t pit_clock_now();

    const LiftedModule* current_module() const { return current_module_; }

    // 32-bit protected-mode (LE) programs, see pm32.cpp.
    void call32(uint32_t addr);
    void invalid_code32(uint32_t addr);
    int run_le(const LiftedModule32* mod, const std::string& cmd_line = "");
    void handle_int31();
    bool pm_interrupt(uint8_t int_no);
    bool call_handler32(uint16_t sel, uint32_t off, bool iret_frame,
                        const std::function<void(Cpu&)>& setup = {});
    uint16_t alloc_selector(uint32_t base, uint32_t limit);
    // DPMI protected-mode interrupt vectors (selector:offset32).
    struct PmVector { uint16_t sel = 0; uint32_t off = 0; };
    PmVector pm_vectors[256];
    std::unordered_map<uint32_t, FunctionPointer> pm_table;
    uint32_t pm_heap_next = 0x00400000;  // linear memory handed out by DPMI 0501h

    // MZ loader & execution
    bool load_mz(const std::string& path, uint16_t load_seg, const std::string& cmd_line = "");
    int run_module(const LiftedModule* mod, const std::string& cmd_line = "");
    int exec_module(const std::string& prog_path, const std::string& cmd_line);

private:
    void install_bios_vectors();

    int isr_depth_ = 0;
    // PIT channel 0 (drives INT 08h).
    uint32_t pit_divisor_ = 65536;
    uint8_t pit_lo_ = 0;
    bool pit_expect_hi_ = false;
    uint64_t pit_base_counter_ = 0;  // performance counter at the first service call
    uint64_t pit_clocks_done_ = 0;   // PIT input clocks already turned into INT 08h ticks

    const LiftedModule* current_module_ = nullptr;
    std::unordered_map<uint32_t, FunctionPointer> dispatch_table_;
    std::unordered_map<uint32_t, FunctionPointer> linear_dispatch_table_;
    std::unordered_map<uint32_t, FunctionPointer> synthetic_dispatch_table_;
};

}  // namespace gen
