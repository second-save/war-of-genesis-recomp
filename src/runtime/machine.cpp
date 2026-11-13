#include <execinfo.h>
#include <cstdlib>
// Machine implementation: hardware dispatch, interrupt routing, and MZ loading.
#include "machine.h"
#include "subsystems/video.h"
#include "subsystems/audio.h"
#include "subsystems/input.h"
#include "subsystems/dos.h"
#include "memory.h"
#include "modules_registry.h"

#include <iostream>
#include <fstream>
#include <cstring>
#include <SDL.h>

namespace gen {

Machine::Machine() {
    ram = std::make_unique<uint8_t[]>(kRamSize);
    std::memset(ram.get(), 0, kRamSize);

    video = std::make_unique<VideoSubsystem>(*this);
    audio = std::make_unique<AudioSubsystem>(*this);
    input = std::make_unique<InputSubsystem>(*this);
    dos = std::make_unique<DosSubsystem>(*this);

    // Register synthetic XMS driver entry point at 0x0070:0x0010
    register_synthetic_function(0x0070, 0x0010, [](Machine& m) {
        m.dos->handle_xms_call();
        (void)pop16(m);      // Return IP
        m.cpu.cs = pop16(m); // Return CS
    });
    ram[phys_addr(0x0070, 0x0010)] = 0xCB; // RETF opcode

    install_bios_vectors();
}

// Minimal BIOS interrupt handlers so programs that hook a vector and chain to
// the previous handler (pushf; call far [old]) land somewhere sensible.
void Machine::install_bios_vectors() {
    auto iret = [](Machine& m) {
        (void)pop16(m);
        m.cpu.cs = pop16(m);
        m.cpu.set_flags16(pop16(m));
    };
    // INT 08h: advance the BIOS tick count, then run the user tick (INT 1Ch).
    register_synthetic_function(0xF000, 0xFEA5, [](Machine& m) {
        uint32_t t;
        std::memcpy(&t, &m.ram[0x46C], 4);
        if (++t >= 0x1800B0) t = 0;
        std::memcpy(&m.ram[0x46C], &t, 4);
        m.call_vector(0x1C);
        (void)pop16(m);
        m.cpu.cs = pop16(m);
        m.cpu.set_flags16(pop16(m));
    });
    // INT 09h: keys already reach INT 16h through the input subsystem.
    register_synthetic_function(0xF000, 0xE987, iret);
    // INT 1Ch and other defaults: plain IRET.
    register_synthetic_function(0xF000, 0xFF53, iret);
    auto set_vec = [&](uint8_t n, uint16_t off) {
        uint16_t seg = 0xF000;
        std::memcpy(&ram[n * 4], &off, 2);
        std::memcpy(&ram[n * 4 + 2], &seg, 2);
        ram[phys_addr(seg, off)] = 0xCF;  // IRET opcode, for code that inspects it
    };
    set_vec(0x08, 0xFEA5);
    set_vec(0x09, 0xE987);
    set_vec(0x1C, 0xFF53);
}

bool Machine::call_handler(uint16_t seg, uint16_t off, bool iret_frame,
                           const std::function<void(Cpu&)>& setup) {
    FunctionPointer fn = resolve_function(seg, off);
    if (!fn) {
        static int warned = 0;
        if (warned++ < 10)
            std::fprintf(stderr, "[WARN] interrupt handler %04X:%04X is not lifted\n", seg, off);
        return false;
    }
    Cpu saved = cpu;
    if (iret_frame) push16(*this, cpu.flags16());
    push16(*this, cpu.cs);
    push16(*this, cpu.ip);
    cpu.cs = seg;
    cpu.ip = off;
    if (iret_frame) cpu.ifl = 0;
    if (setup) setup(cpu);
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } guard(isr_depth_);
    uint32_t t0 = SDL_GetTicks();
    fn(*this);
    uint32_t dt = SDL_GetTicks() - t0;
    if (dt > 20) std::fprintf(stderr, "[WARN] handler %04X:%04X ran for %u ms\n", seg, off, dt);
    cpu = saved;
    return true;
}

bool Machine::call_vector(uint8_t int_no) {
    if (pm) {
        if (pm_vectors[int_no].sel) return call_handler32(pm_vectors[int_no].sel, pm_vectors[int_no].off, true);
        return false;
    }
    uint16_t off, seg;
    std::memcpy(&off, &ram[int_no * 4], 2);
    std::memcpy(&seg, &ram[int_no * 4 + 2], 2);
    if (seg == 0 && off == 0) return false;
    if (!resolve_function(seg, off)) {
        static bool warned[256];
        if (!warned[int_no]) {
            warned[int_no] = true;
            std::fprintf(stderr, "[WARN] INT %02Xh vector %04X:%04X is not lifted\n", int_no, seg, off);
        }
        return false;
    }
    return call_handler(seg, off, true);
}

uint64_t Machine::pit_clock_now() {
    uint64_t now = SDL_GetPerformanceCounter();
    if (pit_base_counter_ == 0) pit_base_counter_ = now;
    return static_cast<uint64_t>(
        static_cast<__uint128_t>(now - pit_base_counter_) * 1193182u / SDL_GetPerformanceFrequency());
}

static uint32_t s_irq_stats[4];  // serviced, skipped (IF=0), skipped (nested), timer ticks
void Machine::service_interrupts() {
    static const bool trace = std::getenv("GENESIS_TRACE_IRQ") != nullptr;
    if (trace) {
        static uint32_t last = 0;
        uint32_t t = SDL_GetTicks();
        if (t - last >= 2000) {
            last = t;
            std::fprintf(stderr, "[IRQ] serviced %u, skipped IF=0 %u, nested %u, timer ticks %u, divisor %u\n",
                         s_irq_stats[0], s_irq_stats[1], s_irq_stats[2], s_irq_stats[3], pit_divisor_);
        }
    }
    if (isr_depth_ > 0) { s_irq_stats[2]++; return; }
    if (!cpu.ifl) { s_irq_stats[1]++; return; }
    s_irq_stats[0]++;

    // PIT channel 0 -> INT 08h at the programmed rate. Count PIT input clocks
    // (1193182 Hz) since a fixed base so short intervals aren't truncated away.
    // Audio is rendered up to each tick before the tick runs, so the music
    // driver's register writes land on the right sample.
    uint64_t clocks = pit_clock_now();
    int delivered = 0;
    while (clocks - pit_clocks_done_ >= pit_divisor_) {
        pit_clocks_done_ += pit_divisor_;
        s_irq_stats[3]++;
        audio->sync(pit_clocks_done_);
        call_vector(0x08);
        if (++delivered >= 64) {  // fell far behind (e.g. a blocking load): drop the backlog
            pit_clocks_done_ = clocks;
            break;
        }
    }
    audio->sync(clocks);

    // Sound Blaster (end of DMA block).
    int irq = audio->pending_irq();
    if (irq >= 0) call_vector(static_cast<uint8_t>(irq < 8 ? 0x08 + irq : 0x70 + irq - 8));

    input->deliver_irqs();
}

void Machine::register_synthetic_function(uint16_t seg, uint16_t off, FunctionPointer fn) {
    uint32_t key = (static_cast<uint32_t>(seg) << 16) | off;
    synthetic_dispatch_table_[key] = fn;
    uint32_t lin = (static_cast<uint32_t>(seg) << 4) + off;
    synthetic_dispatch_table_[lin] = fn;
}

Machine::~Machine() = default;

void Machine::register_module(const LiftedModule* mod) {
    if (!mod) return;
    current_module_ = mod;
    dispatch_table_.clear();
    linear_dispatch_table_.clear();
    for (size_t i = 0; i < mod->num_functions; ++i) {
        const auto& fn = mod->functions[i];
        // Register with relative segment
        uint32_t key_rel = (static_cast<uint32_t>(fn.seg) << 16) | fn.off;
        dispatch_table_[key_rel] = fn.fn;

        // Register with relocated segment
        uint32_t key_abs = (static_cast<uint32_t>(fn.seg + mod->load_seg) << 16) | fn.off;
        dispatch_table_[key_abs] = fn.fn;

        // Register by linear address
        uint32_t lin_rel = (static_cast<uint32_t>(fn.seg) << 4) + fn.off;
        linear_dispatch_table_[lin_rel] = fn.fn;

        uint32_t lin_abs = (static_cast<uint32_t>(fn.seg + mod->load_seg) << 4) + fn.off;
        linear_dispatch_table_[lin_abs] = fn.fn;
    }
}

FunctionPointer Machine::resolve_function(uint16_t seg, uint16_t off) const {
    uint32_t key = (static_cast<uint32_t>(seg) << 16) | off;
    auto it = dispatch_table_.find(key);
    if (it != dispatch_table_.end()) {
        return it->second;
    }
    // Fall back to linear address lookup
    uint32_t lin = (static_cast<uint32_t>(seg) << 4) + off;
    auto it_lin = linear_dispatch_table_.find(lin);
    if (it_lin != linear_dispatch_table_.end()) {
        return it_lin->second;
    }
    // Check synthetic dispatch table (e.g. XMS driver entry point)
    auto it_syn = synthetic_dispatch_table_.find(key);
    if (it_syn != synthetic_dispatch_table_.end()) {
        return it_syn->second;
    }
    auto it_syn_lin = synthetic_dispatch_table_.find(lin);
    if (it_syn_lin != synthetic_dispatch_table_.end()) {
        return it_syn_lin->second;
    }
    return nullptr;
}

void Machine::call_near(uint16_t cs, uint16_t ip) {
    FunctionPointer fn = resolve_function(cs, ip);
    if (fn) {
        fn(*this);
        return;
    }

    const uint8_t* code = ptr(cs, ip);
    if (code[0] == 0x55 && code[1] == 0xCD) {
        uint8_t int_no = code[2];
        interrupt(int_no, cs, ip);
        (void)pop16(*this);
        return;
    }
    if (code[0] == 0xC3) { // RET
        (void)pop16(*this);
        return;
    }

    unimplemented(cs, ip, "call_near (unresolved target)");
    (void)pop16(*this);
}

void Machine::call_far(uint16_t cs, uint16_t ip) {
    FunctionPointer fn = resolve_function(cs, ip);
    if (fn) {
        fn(*this);
        return;
    }

    const uint8_t* code = ptr(cs, ip);
    // Borland int86/int86x stub: 55 CD <int_no> ...
    if (code[0] == 0x55 && code[1] == 0xCD) {
        uint8_t int_no = code[2];
        interrupt(int_no, cs, ip);
        (void)pop16(*this);
        cpu.cs = pop16(*this);
        return;
    }
    // Direct INT + RETF/IRET: CD <int_no> CB/CF
    if (code[0] == 0xCD) {
        uint8_t int_no = code[1];
        interrupt(int_no, cs, ip);
        (void)pop16(*this);
        cpu.cs = pop16(*this);
        return;
    }
    // RETF (0xCB)
    if (code[0] == 0xCB) {
        (void)pop16(*this);
        cpu.cs = pop16(*this);
        return;
    }

    unimplemented(cs, ip, "call_far (unresolved target)");
    (void)pop16(*this);
    cpu.cs = pop16(*this);
}

uint32_t g_watch_lo = 0, g_watch_hi = 0;
// GENESIS_WATCH=lo-hi arms the watch; GENESIS_WATCH_AFTER=ms delays arming it.
static void update_watch() {
    static bool armed = false;
    if (armed) return;
    const char* w = std::getenv("GENESIS_WATCH");
    if (!w) { armed = true; return; }
    const char* after = std::getenv("GENESIS_WATCH_AFTER");
    if (after && SDL_GetTicks() < static_cast<uint32_t>(std::atoi(after))) return;
    std::sscanf(w, "%x-%x", &g_watch_lo, &g_watch_hi);
    armed = true;
}
void watch_hit(Machine& m, uint32_t lin, int size, uint32_t val) {
    static int hits = 0;
    if (hits++ >= 20) return;
    std::fprintf(stderr, "[WATCH] write %05X size %d val %X cs=%04X ds=%04X es=%04X ss:sp=%04X:%04X\n",
                 lin, size, val, m.cpu.cs, m.cpu.ds, m.cpu.es, m.cpu.ss, m.cpu.sp.w);
    void* frames[12];
    int n = backtrace(frames, 12);
    backtrace_symbols_fd(frames, n, 2);
}

void Machine::invalid_code(uint16_t seg, uint16_t off) {
    std::fprintf(stderr, "[FATAL] Execution jumped to invalid code at %04X:%04X\n", seg, off);
    running = false;
}

void Machine::unimplemented(uint16_t seg, uint16_t off, const char* msg) {
    static int warn_count = 0;
    if (warn_count++ < 25) {
        uint16_t ret_ip = *reinterpret_cast<const uint16_t*>(ptr(cpu.ss, cpu.sp.w));
        uint16_t ret_cs = *reinterpret_cast<const uint16_t*>(ptr(cpu.ss, cpu.sp.w + 2));
        std::fprintf(stderr, "[WARN] Unimplemented at %04X:%04X (called from %04X:%04X, sp=%04X): %s\n",
                     seg, off, ret_cs, ret_ip, cpu.sp.w, msg);
        if (std::getenv("GENESIS_BT")) {
            void* frames[32];
            int n = backtrace(frames, 32);
            backtrace_symbols_fd(frames, n, 2);
        }
    }
}

void Machine::fpu_emulated(uint8_t int_no, uint16_t seg, uint16_t off, const char* msg) {
    // Borland FPU emulator trap (INT 34h..3Dh)
    (void)int_no; (void)seg; (void)off; (void)msg;
}

static bool trace_port(uint16_t port) {
    static const bool t = std::getenv("GENESIS_TRACE_PORTS") != nullptr;
    return t && !(port >= 0x3C0 && port <= 0x3DA) && port != 0x60 && port != 0x388 && port != 0x389;
}

uint8_t Machine::in8(uint16_t port) {
    if (trace_port(port)) {
        static int n = 0;
        if (n++ < 400) std::fprintf(stderr, "[PORT] in  %03X\n", port);
    }
    if (port == 0x60) {
        return input->read_port_60();
    }
    if (audio->handles_port(port)) {
        return audio->read_port(port);
    }
    if (port >= 0x3C0 && port <= 0x3DA) {
        return video->read_port(port);
    }
    if (port == 0x40 || port == 0x42) {
        // PIT Timer counter
        tick_counter += 10;
        return static_cast<uint8_t>(tick_counter);
    }
    return 0xFF;
}

uint16_t Machine::in16(uint16_t port) {
    uint8_t l = in8(port);
    uint8_t h = in8(port + 1);
    return static_cast<uint16_t>(l | (static_cast<uint16_t>(h) << 8));
}

uint32_t Machine::in32(uint16_t port) {
    uint16_t l = in16(port);
    uint16_t h = in16(port + 2);
    return static_cast<uint32_t>(l | (static_cast<uint32_t>(h) << 16));
}

void Machine::out8(uint16_t port, uint8_t val) {
    if (trace_port(port)) {
        static int n = 0;
        if (n++ < 400) std::fprintf(stderr, "[PORT] out %03X %02X\n", port, val);
        static const char* bt = std::getenv("GENESIS_PORT_BT");
        if (bt && std::strtol(bt, nullptr, 16) == port) {
            void* frames[12];
            int k = backtrace(frames, 12);
            backtrace_symbols_fd(frames, k, 2);
        }
    }
    if (port == 0x43) {
        // Channel 0, lobyte/hibyte access: next two writes to 0x40 set the divisor.
        if ((val & 0xC0) == 0x00 && (val & 0x30) == 0x30) pit_expect_hi_ = false;
        return;
    }
    if (port == 0x40) {
        if (!pit_expect_hi_) {
            pit_lo_ = val;
            pit_expect_hi_ = true;
        } else {
            uint32_t d = static_cast<uint32_t>(pit_lo_) | (static_cast<uint32_t>(val) << 8);
            pit_divisor_ = d ? d : 65536;
            pit_expect_hi_ = false;
            if (std::getenv("GENESIS_TRACE_INT"))
                std::fprintf(stderr, "[PIT] divisor %u (%.1f Hz)\n", pit_divisor_, 1193182.0 / pit_divisor_);
        }
        return;
    }
    if (audio->handles_port(port)) {
        audio->write_port(port, val);
        return;
    }
    if (port >= 0x3C0 && port <= 0x3DA) {
        video->write_port(port, val);
        return;
    }
}

void Machine::out16(uint16_t port, uint16_t val) {
    out8(port, static_cast<uint8_t>(val));
    out8(port + 1, static_cast<uint8_t>(val >> 8));
}

void Machine::out32(uint16_t port, uint32_t val) {
    out16(port, static_cast<uint16_t>(val));
    out16(port + 2, static_cast<uint16_t>(val >> 16));
}

void Machine::interrupt(uint8_t int_no, uint16_t seg, uint16_t off) {
    if (pm && pm_interrupt(int_no)) return;
    switch (int_no) {
    case 0x10:
        video->handle_int10();
        break;
    case 0x11: // BIOS Equipment List
        cpu.ax.w = 0x0023; // Math coprocessor present, 80x25 color, 1 diskette
        break;
    case 0x16:
        input->handle_int16();
        break;
    case 0x1A: // Time-of-day BIOS services
        if (cpu.ax.b.h == 0x00) { // Get system clock ticks
            uint32_t ticks = SDL_GetTicks() * 182 / 10000;
            *reinterpret_cast<uint32_t*>(&ram[0x46C]) = ticks;
            cpu.cx.w = static_cast<uint16_t>(ticks >> 16);
            cpu.dx.w = static_cast<uint16_t>(ticks & 0xFFFF);
            cpu.ax.b.l = 0; // Midnight flag
        }
        break;
    case 0x21:
        dos->handle_int21();
        break;
    case 0x2F:
        dos->handle_int2f();
        break;
    case 0x33:
        input->handle_int33();
        break;
    default:
        std::fprintf(stderr, "[INT] Unhandled interrupt INT %02Xh from %04X:%04X (AX=%04X)\n",
                     int_no, seg, off, cpu.ax.w);
        if (std::getenv("GENESIS_BT")) {
            void* frames[16];
            int n = backtrace(frames, 16);
            backtrace_symbols_fd(frames, n, 2);
        }
        break;
    }
}

void Machine::frame_sync() {
    if (isr_depth_ > 0) return;  // never re-enter the main loop from a handler
    input->pump_events();
    if (!running) {
        throw ProcessTerminated{0};
    }
    video->present_frame();
    update_watch();
    service_interrupts();

    uint32_t now = SDL_GetTicks();
    uint32_t elapsed = now - last_tick_time_;
    if (elapsed < 16) { // ~60 FPS cap
        SDL_Delay(16 - elapsed);
        now = SDL_GetTicks();
    }
    last_tick_time_ = now;
}

void Machine::poll_tick() {
    tick_counter++;
    if ((tick_counter & 0x003F) == 0) {
        if (isr_depth_ > 0) return;
        service_interrupts();
        if (SDL_GetTicks() - last_tick_time_ >= 16) {
            frame_sync();
        }
    }
}

void Machine::idle() {
    frame_sync();
    SDL_Delay(1);
}

#pragma pack(push, 1)
struct MzHeaderRaw {
    char sig[2];
    uint16_t last_page;
    uint16_t n_pages;
    uint16_t n_reloc;
    uint16_t hdr_paras;
    uint16_t min_alloc;
    uint16_t max_alloc;
    uint16_t init_ss;
    uint16_t init_sp;
    uint16_t csum;
    uint16_t init_ip;
    uint16_t init_cs;
    uint16_t reloc_off;
    uint16_t ovno;
};
#pragma pack(pop)

bool Machine::load_mz(const std::string& path, uint16_t load_seg, const std::string& cmd_line) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::fprintf(stderr, "[ERR] Failed to open MZ executable: %s\n", path.c_str());
        return false;
    }

    MzHeaderRaw hdr{};
    file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (hdr.sig[0] != 'M' || hdr.sig[1] != 'Z') {
        std::fprintf(stderr, "[ERR] Invalid MZ signature in %s\n", path.c_str());
        return false;
    }

    uint32_t file_size = (hdr.n_pages - 1) * 512 + (hdr.last_page ? hdr.last_page : 512);
    uint32_t image_size = file_size - (hdr.hdr_paras * 16);

    // Load executable image into RAM at load_seg
    uint32_t load_base = static_cast<uint32_t>(load_seg) << 4;
    file.seekg(hdr.hdr_paras * 16);
    file.read(reinterpret_cast<char*>(&ram[load_base]), image_size);

    // Apply relocations
    file.seekg(hdr.reloc_off);
    for (uint16_t i = 0; i < hdr.n_reloc; ++i) {
        uint16_t r_off = 0, r_seg = 0;
        file.read(reinterpret_cast<char*>(&r_off), 2);
        file.read(reinterpret_cast<char*>(&r_seg), 2);

        uint32_t target_addr = phys_addr(r_seg + load_seg, r_off);
        if (target_addr + 1 < kRamSize) {
            uint16_t val;
            std::memcpy(&val, &ram[target_addr], 2);
            val = static_cast<uint16_t>(val + load_seg);
            std::memcpy(&ram[target_addr], &val, 2);
        }
    }

    // Set up Program Segment Prefix (PSP) at load_seg - 0x10
    uint16_t psp_seg = static_cast<uint16_t>(load_seg - 0x10);
    uint32_t psp_base = static_cast<uint32_t>(psp_seg) << 4;
    std::memset(&ram[psp_base], 0, 0x100);

    dos->reset_memory(psp_seg);

    // INT 20h exit instruction at PSP:0
    ram[psp_base + 0] = 0xCD;
    ram[psp_base + 1] = 0x20;
    // Top of memory paragraph
    *reinterpret_cast<uint16_t*>(&ram[psp_base + 2]) = 0x9FFF;

    // Set up Environment Block at psp_seg - 0x20
    uint16_t env_seg = static_cast<uint16_t>(psp_seg - 0x20);
    uint32_t env_base = static_cast<uint32_t>(env_seg) << 4;
    std::memset(&ram[env_base], 0, 0x200);

    std::filesystem::path p(path);
    std::string exe_filename = p.filename().string();
    for (char& ch : exe_filename) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    std::string full_dos_path = "C:\\" + exe_filename;

    uint32_t epos = env_base;
    auto write_env_str = [&](const std::string& str) {
        std::memcpy(&ram[epos], str.c_str(), str.size() + 1);
        epos += static_cast<uint32_t>(str.size() + 1);
    };
    write_env_str("PATH=C:\\");
    write_env_str("PROMPT=$P$G");
    ram[epos++] = 0; // Double null byte terminating environment strings

    // 16-bit word count = 1 (DOS 3.0+ program name)
    *reinterpret_cast<uint16_t*>(&ram[epos]) = 1;
    epos += 2;

    // Program name
    std::memcpy(&ram[epos], full_dos_path.c_str(), full_dos_path.size() + 1);

    // Environment block segment in PSP:2Ch
    *reinterpret_cast<uint16_t*>(&ram[psp_base + 0x2C]) = env_seg;

    // Command line in PSP:80h
    uint8_t cmd_len = static_cast<uint8_t>(cmd_line.size());
    ram[psp_base + 0x80] = cmd_len;
    for (size_t i = 0; i < cmd_line.size() && i < 126; ++i) {
        ram[psp_base + 0x81 + i] = static_cast<uint8_t>(cmd_line[i]);
    }
    ram[psp_base + 0x81 + cmd_len] = '\r';

    // Initialize CPU registers
    cpu.cs = static_cast<uint16_t>(hdr.init_cs + load_seg);
    cpu.ip = hdr.init_ip;
    cpu.ss = static_cast<uint16_t>(hdr.init_ss + load_seg);
    cpu.sp.w = hdr.init_sp;
    cpu.ds = psp_seg;
    cpu.es = psp_seg;
    cpu.ax.w = cpu.bx.w = cpu.cx.w = cpu.dx.w = 0;
    cpu.si.w = cpu.di.w = cpu.bp.w = 0;
    cpu.ifl = 1;

    return true;
}

int Machine::run_module(const LiftedModule* mod, const std::string& cmd_line) {
    if (!mod) return -1;
    register_module(mod);

    // Find module executable file in data path
    std::filesystem::path exe_path = dos->resolve_path(mod->filename);
    if (!load_mz(exe_path.string(), mod->load_seg, cmd_line)) {
        return -1;
    }

    // Execute starting from CS:IP
    running = true;
    exit_code = 0;

    FunctionPointer entry = resolve_function(cpu.cs, cpu.ip);
    if (!entry) {
        std::fprintf(stderr, "[FATAL] Entry point not found for module %s at %04X:%04X\n",
                     mod->name, cpu.cs, cpu.ip);
        return -1;
    }

    try {
        entry(*this);
    } catch (const ProcessTerminated& pt) {
        exit_code = pt.exit_code;
    }
    return exit_code;
}

const LiftedModule* find_module_by_name(const std::string& name) {
    std::string lower;
    for (char ch : name) lower += static_cast<char>(std::tolower(ch));
    if (lower.find("main") != std::string::npos) return &kLifted_main;
    if (lower.find("chp") != std::string::npos) return &kLifted_chp;
    if (lower.find("pack") != std::string::npos) return &kLifted_pack;
    if (lower.find("chrsys") != std::string::npos) return &kLifted_chrsys;
    return nullptr;
}

int Machine::exec_module(const std::string& prog_path, const std::string& cmd_line) {
    const LiftedModule* child_mod = find_module_by_name(prog_path);
    if (!child_mod) {
        std::fprintf(stderr, "[DOS] EXEC failed: unknown module %s\n", prog_path.c_str());
        cpu.cf = 1;
        cpu.ax.w = 2; // File not found
        return -1;
    }

    std::cout << "[DOS] EXEC starting child module: " << child_mod->name << " with args: '" << cmd_line << "'\n";
    const LiftedModule* parent_mod = current_module_;
    // Snapshot conventional RAM (from 0x00000 up to 0xA0000, 640KB)
    std::vector<uint8_t> saved_ram(ram.get(), ram.get() + 0xA0000);
    Cpu saved_cpu = cpu;
    bool was_running = running;
    auto saved_blocks = dos->memory_blocks();

    int child_exit = run_module(child_mod, cmd_line);

    // Restore conventional RAM, heap pointer, and parent module dispatch table
    std::memcpy(ram.get(), saved_ram.data(), saved_ram.size());
    dos->set_memory_blocks(std::move(saved_blocks));
    if (parent_mod) {
        register_module(parent_mod);
    }
    cpu = saved_cpu;
    running = was_running;
    cpu.cf = 0;
    cpu.ax.b.l = static_cast<uint8_t>(child_exit);
    cpu.ax.b.h = 0;
    return child_exit;
}

}  // namespace gen
