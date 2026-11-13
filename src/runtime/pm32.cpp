// 32-bit protected-mode support for the DOS/4GW title program (TTL.EXE).
//
// The program is lifted by tools/lifter/lift_le.py. Here we provide what
// DOS/4GW would: the flat memory layout (objects at their preferred linear
// addresses, first megabyte identity-mapped), selectors, the DPMI host
// (INT 31h), the DOS/4GW extensions of INT 21h, and 32-bit interrupt
// delivery into the program's handlers.
#include "machine.h"
#include "memory.h"
#include "subsystems/dos.h"
#include "subsystems/input.h"
#include "subsystems/video.h"

#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace gen {

namespace {
constexpr uint16_t kCodeSel = 0x0168;
constexpr uint16_t kDataSel = 0x0170;
constexpr uint16_t kPspSeg = 0x0900;   // real-mode PSP (below the LE objects)
constexpr uint16_t kEnvSeg = 0x0800;
// Default protected-mode interrupt handlers live at kDefaultVec + n (BIOS area).
constexpr uint32_t kDefaultVec = 0x000FFE00;

void default_iret(Machine& m) {
    (void)pop32(m);
    m.cpu.cs = static_cast<uint16_t>(pop32(m));
    m.cpu.set_flags16(static_cast<uint16_t>(pop32(m)));
}

// INT 08h without a program handler: BIOS tick count, then INT 1Ch.
void default_int08(Machine& m) {
    uint32_t t;
    std::memcpy(&t, &m.ram[0x46C], 4);
    if (++t >= 0x1800B0) t = 0;
    std::memcpy(&m.ram[0x46C], &t, 4);
    m.call_vector(0x1C);
    default_iret(m);
}
bool trace_dpmi() {
    static const bool t = std::getenv("GENESIS_TRACE_DPMI") != nullptr;
    return t;
}
}  // namespace

// Load an LE executable's objects at their preferred linear addresses and
// apply its internal fixups (what DOS/4GW does; see tools/lifter/le.py).
// Returns the end of the highest object, or 0 on failure.
static uint32_t load_le(Machine& m, const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return static_cast<uint32_t>(d[o] | (d[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (d.size() < 0x40) return 0;
    size_t le = u32(0x3C);
    if (le + 0x88 > d.size() || d[le] != 'L' || d[le + 1] != 'E') return 0;
    uint32_t page_size = u32(le + 0x28), last_page = u32(le + 0x2C), npages = u32(le + 0x14);
    uint32_t objtab = u32(le + 0x40), nobj = u32(le + 0x44);
    uint32_t fixpage = u32(le + 0x68), fixrec = u32(le + 0x6C), datapages = u32(le + 0x80);
    struct Obj { uint32_t vsize, base, page_index, page_count; };
    std::vector<Obj> objs;
    uint32_t top = 0;
    for (uint32_t i = 0; i < nobj; ++i) {
        size_t o = le + objtab + i * 24;
        Obj ob{u32(o), u32(o + 4), u32(o + 12), u32(o + 16)};
        objs.push_back(ob);
        std::memset(&m.ram[ob.base], 0, ob.vsize);
        for (uint32_t p = 0; p < ob.page_count; ++p) {
            uint32_t pn = ob.page_index + p;
            uint32_t size = (pn == npages) ? last_page : page_size;
            size_t src = datapages + static_cast<size_t>(pn - 1) * page_size;
            uint32_t dst = p * page_size;
            if (dst >= ob.vsize) break;
            size = std::min<uint32_t>(size, ob.vsize - dst);
            if (src + size > d.size()) size = static_cast<uint32_t>(d.size() > src ? d.size() - src : 0);
            std::memcpy(&m.ram[ob.base + dst], &d[src], size);
        }
        top = std::max(top, ob.base + ob.vsize);
    }
    auto put32 = [&](uint32_t at, uint32_t v) { std::memcpy(&m.ram[at], &v, 4); };
    auto put16 = [&](uint32_t at, uint16_t v) { std::memcpy(&m.ram[at], &v, 2); };
    for (const Obj& ob : objs) {
        for (uint32_t p = 0; p < ob.page_count; ++p) {
            uint32_t g = ob.page_index + p - 1;
            size_t pos = le + fixrec + u32(le + fixpage + g * 4);
            size_t end = le + fixrec + u32(le + fixpage + g * 4 + 4);
            while (pos < end) {
                uint8_t src = d[pos], flags = d[pos + 1];
                pos += 2;
                uint8_t stype = src & 0x0F;
                std::vector<int16_t> offs;
                uint32_t cnt = 1;
                bool list = (src & 0x20) != 0;
                if (list) {
                    cnt = d[pos++];
                } else {
                    offs.push_back(static_cast<int16_t>(u16(pos)));
                    pos += 2;
                }
                uint32_t tobj;
                if (flags & 0x40) { tobj = u16(pos); pos += 2; } else { tobj = d[pos++]; }
                uint32_t toff = 0;
                if (stype != 0x02) {
                    if (flags & 0x10) { toff = u32(pos); pos += 4; } else { toff = u16(pos); pos += 2; }
                }
                if (list) {
                    for (uint32_t k = 0; k < cnt; ++k) offs.push_back(static_cast<int16_t>(u16(pos + 2 * k)));
                    pos += 2 * cnt;
                }
                if (tobj == 0 || tobj > objs.size()) continue;
                uint32_t target = objs[tobj - 1].base + toff;
                uint16_t sel = (tobj == 1) ? kCodeSel : kDataSel;
                for (int16_t so : offs) {
                    uint32_t at = ob.base + p * page_size + static_cast<uint32_t>(static_cast<int32_t>(so));
                    switch (stype) {
                    case 0x07: put32(at, target); break;
                    case 0x08: put32(at, target - (at + 4)); break;
                    case 0x02: put16(at, sel); break;
                    case 0x06: put32(at, target); put16(at + 4, sel); break;
                    case 0x05: put16(at, static_cast<uint16_t>(target)); break;
                    default: break;
                    }
                }
            }
        }
    }
    return top;
}

uint16_t Machine::alloc_selector(uint32_t base, uint32_t limit) {
    for (int i = 0x40; i < 8192; ++i) {
        if (!sel_used[i]) {
            sel_used[i] = true;
            sel_base[i] = base;
            sel_limit[i] = limit;
            return static_cast<uint16_t>((i << 3) | 7);  // LDT, RPL 3
        }
    }
    return 0;
}

void Machine::call32(uint32_t addr) {
    auto it = pm_table.find(addr);
    if (it != pm_table.end()) {
        it->second(*this);
        return;
    }
    std::fprintf(stderr, "[FATAL] call to unlifted 32-bit address %08X (esp=%08X)\n", addr, cpu.sp.e);
    running = false;
    throw ProcessTerminated{255};
}

void Machine::invalid_code32(uint32_t addr) {
    std::fprintf(stderr, "[FATAL] Execution reached undecodable code at %08X\n", addr);
    running = false;
    throw ProcessTerminated{255};
}

bool Machine::call_handler32(uint16_t sel, uint32_t off, bool iret_frame,
                             const std::function<void(Cpu&)>& setup) {
    uint32_t addr = sel_base[sel >> 3] + off;
    auto it = pm_table.find(addr);
    if (it == pm_table.end()) {
        static int warned = 0;
        if (warned++ < 10) std::fprintf(stderr, "[WARN] PM handler %04X:%08X is not lifted\n", sel, off);
        return false;
    }
    Cpu saved = cpu;
    if (iret_frame) push32(*this, cpu.flags16());
    push32(*this, cpu.cs);
    push32(*this, 0);  // return EIP (unused: handlers return natively)
    cpu.cs = sel;
    if (iret_frame) cpu.ifl = 0;
    if (setup) setup(cpu);
    ++isr_depth_;
    try {
        it->second(*this);
    } catch (...) {
        --isr_depth_;
        throw;
    }
    --isr_depth_;
    cpu = saved;
    return true;
}

int Machine::run_le(const LiftedModule32* mod, const std::string& cmd_line) {
    std::memset(ram.get() + 0x500, 0, 0xA0000 - 0x500);  // keep the IVT and BIOS data area
    std::memset(sel_used, 0, sizeof(sel_used));
    std::memset(sel_base, 0, sizeof(sel_base));
    pm_table.clear();
    for (int n = 0; n < 256; ++n) {
        pm_vectors[n] = {kCodeSel, kDefaultVec + static_cast<uint32_t>(n)};
        pm_table[kDefaultVec + n] = (n == 0x08) ? default_int08 : default_iret;
    }
    for (size_t i = 0; i < mod->num_functions; ++i) pm_table[mod->functions[i].addr] = mod->functions[i].fn;

    uint32_t top = load_le(*this, dos->resolve_path(mod->filename));
    if (!top) {
        std::fprintf(stderr, "[ERR] Could not load %s from the game folder\n", mod->filename);
        return 255;
    }

    // Selectors: flat code/data, PSP, environment.
    auto set_sel = [&](uint16_t sel, uint32_t base, uint32_t limit) {
        sel_used[sel >> 3] = true;
        sel_base[sel >> 3] = base;
        sel_limit[sel >> 3] = limit;
    };
    set_sel(kCodeSel, 0, 0xFFFFFFFF);
    set_sel(kDataSel, 0, 0xFFFFFFFF);
    uint16_t psp_sel = alloc_selector(kPspSeg << 4, 0xFF);
    uint16_t env_sel = alloc_selector(kEnvSeg << 4, 0xFFF);

    // DOS memory: PSP, then the area occupied by the LE objects is reserved;
    // DPMI 0100h allocates from what's left below 640K.
    dos->reset_memory(kPspSeg);
    uint16_t obj_end_seg = static_cast<uint16_t>((top + 0x10 + 15) >> 4);
    dos->set_memory_blocks({{kPspSeg, 0x10}, {0x0FFF, static_cast<uint16_t>(obj_end_seg - 0x0FFF)}});

    // Environment: COMSPEC so system() works, then the program path.
    uint32_t env = kEnvSeg << 4;
    std::memset(&ram[env], 0, 0x1000);
    uint32_t p = env;
    auto put = [&](const std::string& s) {
        std::memcpy(&ram[p], s.c_str(), s.size() + 1);
        p += static_cast<uint32_t>(s.size() + 1);
    };
    put("COMSPEC=C:\\COMMAND.COM");
    put("PATH=C:\\");
    put("DOS4G=QUIET");
    ram[p++] = 0;
    ram[p++] = 1; ram[p++] = 0;
    std::string exe = std::string("C:\\") + mod->filename;
    put(exe);

    uint32_t psp = kPspSeg << 4;
    std::memset(&ram[psp], 0, 0x100);
    ram[psp] = 0xCD; ram[psp + 1] = 0x20;
    *reinterpret_cast<uint16_t*>(&ram[psp + 2]) = 0x9FFF;
    *reinterpret_cast<uint16_t*>(&ram[psp + 0x2C]) = env_sel;
    ram[psp + 0x80] = static_cast<uint8_t>(cmd_line.size());
    std::memcpy(&ram[psp + 0x81], cmd_line.data(), std::min<size_t>(cmd_line.size(), 126));
    ram[psp + 0x81 + std::min<size_t>(cmd_line.size(), 126)] = '\r';

    pm = true;
    pm_heap_next = 0x00400000;
    cpu = Cpu{};
    cpu.cs = kCodeSel;
    cpu.ds = cpu.ss = kDataSel;
    cpu.es = psp_sel;
    cpu.fs = cpu.gs = 0;
    cpu.sp.e = mod->initial_esp;
    cpu.ifl = 1;
    running = true;
    exit_code = 0;
    pit_divisor_ = 65536;

    try {
        call32(mod->entry);
    } catch (const ProcessTerminated& pt) {
        exit_code = pt.exit_code;
    }
    pm = false;
    input->reset_mouse_handler();
    return exit_code;
}

// --------------------------------------------------------------------------
// INT 21h extensions and protected-mode vectors
// --------------------------------------------------------------------------
bool Machine::pm_interrupt(uint8_t int_no) {
    if (int_no == 0x31) {
        handle_int31();
        return true;
    }
    if (int_no == 0x21) {
        uint8_t ah = cpu.ax.b.h;
        if (ah == 0xFF && cpu.dx.w == 0x0078) {  // DOS/4G presence check
            cpu.ax.e = 0x4734FFFF;  // al != 0
            return true;
        }
        if (ah == 0x25) {
            pm_vectors[cpu.ax.b.l] = {cpu.ds, cpu.dx.e};
            if (trace_dpmi())
                std::fprintf(stderr, "[DPMI] INT 21h set vector %02X -> %04X:%08X\n", cpu.ax.b.l, cpu.ds, cpu.dx.e);
            return true;
        }
        if (ah == 0x35) {
            cpu.es = pm_vectors[cpu.ax.b.l].sel;
            cpu.bx.e = pm_vectors[cpu.ax.b.l].off;
            return true;
        }
        if (ah == 0x48) {  // DOS/4GW: allocate linear memory, BX = paragraphs -> AX = selector
            uint32_t size = static_cast<uint32_t>(cpu.bx.w) << 4;
            uint32_t base = pm_heap_next;
            pm_heap_next = (pm_heap_next + size + 0xFFF) & ~0xFFFu;
            cpu.ax.w = alloc_selector(base, size - 1);
            cpu.cf = 0;
            return true;
        }
        return false;
    }
    return false;
}

static void rm_regs_load(Machine& m, uint32_t s) {
    // DPMI real-mode call structure.
    auto& c = m.cpu;
    std::memcpy(&c.di.e, &m.ram[s + 0x00], 4);
    std::memcpy(&c.si.e, &m.ram[s + 0x04], 4);
    std::memcpy(&c.bp.e, &m.ram[s + 0x08], 4);
    std::memcpy(&c.bx.e, &m.ram[s + 0x10], 4);
    std::memcpy(&c.dx.e, &m.ram[s + 0x14], 4);
    std::memcpy(&c.cx.e, &m.ram[s + 0x18], 4);
    std::memcpy(&c.ax.e, &m.ram[s + 0x1C], 4);
    uint16_t f;
    std::memcpy(&f, &m.ram[s + 0x20], 2);
    c.set_flags16(f);
    std::memcpy(&c.es, &m.ram[s + 0x22], 2);
    std::memcpy(&c.ds, &m.ram[s + 0x24], 2);
    std::memcpy(&c.fs, &m.ram[s + 0x26], 2);
    std::memcpy(&c.gs, &m.ram[s + 0x28], 2);
}

static void rm_regs_store(Machine& m, uint32_t s) {
    auto& c = m.cpu;
    std::memcpy(&m.ram[s + 0x00], &c.di.e, 4);
    std::memcpy(&m.ram[s + 0x04], &c.si.e, 4);
    std::memcpy(&m.ram[s + 0x08], &c.bp.e, 4);
    std::memcpy(&m.ram[s + 0x10], &c.bx.e, 4);
    std::memcpy(&m.ram[s + 0x14], &c.dx.e, 4);
    std::memcpy(&m.ram[s + 0x18], &c.cx.e, 4);
    std::memcpy(&m.ram[s + 0x1C], &c.ax.e, 4);
    uint16_t f = c.flags16();
    std::memcpy(&m.ram[s + 0x20], &f, 2);
    std::memcpy(&m.ram[s + 0x22], &c.es, 2);
    std::memcpy(&m.ram[s + 0x24], &c.ds, 2);
    std::memcpy(&m.ram[s + 0x26], &c.fs, 2);
    std::memcpy(&m.ram[s + 0x28], &c.gs, 2);
}

void Machine::handle_int31() {
    auto& c = cpu;
    uint16_t ax = c.ax.w;
    if (trace_dpmi())
        std::fprintf(stderr, "[DPMI] AX=%04X BX=%04X CX=%04X DX=%04X\n", ax, c.bx.w, c.cx.w, c.dx.w);
    c.cf = 0;
    switch (ax) {
    case 0x0000: {  // allocate LDT descriptors
        uint16_t first = 0;
        for (int i = 0; i < std::max<int>(1, c.cx.w); ++i) {
            uint16_t s = alloc_selector(0, 0);
            if (i == 0) first = s;
        }
        c.ax.w = first;
        break;
    }
    case 0x0001:  // free descriptor
        sel_used[c.bx.w >> 3] = false;
        break;
    case 0x0002: {  // segment to descriptor
        uint32_t base = static_cast<uint32_t>(c.bx.w) << 4;
        for (int i = 0x40; i < 8192; ++i)
            if (sel_used[i] && sel_base[i] == base && sel_limit[i] == 0xFFFF) {
                c.ax.w = static_cast<uint16_t>((i << 3) | 7);
                return;
            }
        c.ax.w = alloc_selector(base, 0xFFFF);
        break;
    }
    case 0x0003:
        c.ax.w = 8;
        break;
    case 0x0006: {  // get segment base
        uint32_t b = sel_base[c.bx.w >> 3];
        c.cx.w = static_cast<uint16_t>(b >> 16);
        c.dx.w = static_cast<uint16_t>(b);
        break;
    }
    case 0x0007:
        sel_base[c.bx.w >> 3] = (static_cast<uint32_t>(c.cx.w) << 16) | c.dx.w;
        break;
    case 0x0008:
        sel_limit[c.bx.w >> 3] = (static_cast<uint32_t>(c.cx.w) << 16) | c.dx.w;
        break;
    case 0x0009:
        break;
    case 0x000A: {  // create alias
        uint16_t s = c.bx.w;
        c.ax.w = alloc_selector(sel_base[s >> 3], sel_limit[s >> 3]);
        break;
    }
    case 0x000B: {  // get descriptor (8 bytes at ES:EDI)
        uint8_t* d = ptr(c.es, c.di.e);
        uint32_t b = sel_base[c.bx.w >> 3], l = sel_limit[c.bx.w >> 3];
        bool g = l > 0xFFFFF;
        if (g) l >>= 12;
        d[0] = l & 0xFF; d[1] = (l >> 8) & 0xFF;
        d[2] = b & 0xFF; d[3] = (b >> 8) & 0xFF; d[4] = (b >> 16) & 0xFF;
        d[5] = 0xF2;
        d[6] = static_cast<uint8_t>(((l >> 16) & 0x0F) | 0x40 | (g ? 0x80 : 0));
        d[7] = static_cast<uint8_t>(b >> 24);
        break;
    }
    case 0x000C: {  // set descriptor
        const uint8_t* d = ptr(c.es, c.di.e);
        uint32_t b = d[2] | (d[3] << 8) | (d[4] << 16) | (static_cast<uint32_t>(d[7]) << 24);
        uint32_t l = d[0] | (d[1] << 8) | ((d[6] & 0x0F) << 16);
        if (d[6] & 0x80) l = (l << 12) | 0xFFF;
        sel_base[c.bx.w >> 3] = b;
        sel_limit[c.bx.w >> 3] = l;
        break;
    }
    case 0x0100: {  // allocate DOS memory: BX paras -> AX segment, DX selector
        uint16_t largest = 0;
        uint16_t seg = dos->dos_alloc_paras(c.bx.w, &largest);
        if (!seg) {
            c.cf = 1;
            c.ax.w = 8;
            c.bx.w = largest;
            break;
        }
        c.ax.w = seg;
        c.dx.w = alloc_selector(static_cast<uint32_t>(seg) << 4, (static_cast<uint32_t>(c.bx.w) << 4) - 1);
        break;
    }
    case 0x0101: {  // free DOS memory (DX selector)
        uint32_t b = sel_base[c.dx.w >> 3];
        dos->dos_free_paras(static_cast<uint16_t>(b >> 4));
        sel_used[c.dx.w >> 3] = false;
        break;
    }
    case 0x0102: {
        uint32_t b = sel_base[c.dx.w >> 3];
        uint16_t maxp = 0;
        if (!dos->dos_realloc_paras(static_cast<uint16_t>(b >> 4), c.bx.w, &maxp)) {
            c.cf = 1;
            c.ax.w = 8;
            c.bx.w = maxp;
        } else {
            sel_limit[c.dx.w >> 3] = (static_cast<uint32_t>(c.bx.w) << 4) - 1;
        }
        break;
    }
    case 0x0200: {  // get real-mode vector
        uint16_t o, s;
        std::memcpy(&o, &ram[c.bx.b.l * 4], 2);
        std::memcpy(&s, &ram[c.bx.b.l * 4 + 2], 2);
        c.cx.w = s;
        c.dx.w = o;
        break;
    }
    case 0x0201:
        std::memcpy(&ram[c.bx.b.l * 4], &c.dx.w, 2);
        std::memcpy(&ram[c.bx.b.l * 4 + 2], &c.cx.w, 2);
        break;
    case 0x0204:
        c.cx.w = pm_vectors[c.bx.b.l].sel;
        c.dx.e = pm_vectors[c.bx.b.l].off;
        break;
    case 0x0205:
        pm_vectors[c.bx.b.l] = {c.cx.w, c.dx.e};
        if (trace_dpmi())
            std::fprintf(stderr, "[DPMI] set PM vector %02X -> %04X:%08X\n", c.bx.b.l, c.cx.w, c.dx.e);
        break;
    case 0x0300: {  // simulate real-mode interrupt
        uint8_t n = c.bx.b.l;
        uint32_t s = phys_addr(c.es, c.di.e);
        Cpu saved = cpu;
        pm = false;
        rm_regs_load(*this, s);
        cpu.ss = 0x9000;  // scratch real-mode stack
        cpu.sp.e = 0xFFF0;
        try {
            interrupt(n, 0, 0);
        } catch (...) {
            pm = true;
            throw;
        }
        rm_regs_store(*this, s);
        cpu = saved;
        pm = true;
        c.cf = 0;
        break;
    }
    case 0x0400:
        c.ax.w = 0x005A;  // DPMI 0.90
        c.bx.w = 0x0005;
        c.cx.b.l = 4;     // 486
        c.dx.w = 0x0870;
        break;
    case 0x0500: {  // free memory information
        uint8_t* d = ptr(c.es, c.di.e);
        std::memset(d, 0xFF, 0x30);
        uint32_t free_bytes = static_cast<uint32_t>(kRamSize) - pm_heap_next;
        std::memcpy(d + 0x00, &free_bytes, 4);
        uint32_t pages = free_bytes >> 12;
        std::memcpy(d + 0x04, &pages, 4);
        std::memcpy(d + 0x08, &pages, 4);
        std::memcpy(d + 0x14, &pages, 4);
        break;
    }
    case 0x0501: {  // allocate linear memory BX:CX bytes -> BX:CX address, SI:DI handle
        uint32_t size = (static_cast<uint32_t>(c.bx.w) << 16) | c.cx.w;
        uint32_t base = pm_heap_next;
        if (base + size > kRamSize) {
            c.cf = 1;
            c.ax.w = 0x8013;
            break;
        }
        pm_heap_next = (pm_heap_next + size + 0xFFF) & ~0xFFFu;
        c.bx.w = static_cast<uint16_t>(base >> 16);
        c.cx.w = static_cast<uint16_t>(base);
        c.si.w = c.bx.w;
        c.di.w = c.cx.w;
        if (trace_dpmi()) std::fprintf(stderr, "[DPMI] alloc %u bytes at %08X\n", size, base);
        break;
    }
    case 0x0502:  // free (bump allocator: ignore)
        break;
    case 0x0503: {  // resize: allocate anew and copy
        uint32_t size = (static_cast<uint32_t>(c.bx.w) << 16) | c.cx.w;
        uint32_t old = (static_cast<uint32_t>(c.si.w) << 16) | c.di.w;
        uint32_t base = pm_heap_next;
        if (base + size > kRamSize) {
            c.cf = 1;
            c.ax.w = 0x8013;
            break;
        }
        pm_heap_next = (pm_heap_next + size + 0xFFF) & ~0xFFFu;
        std::memmove(&ram[base], &ram[old], std::min<uint32_t>(size, base - old));
        c.bx.w = static_cast<uint16_t>(base >> 16);
        c.cx.w = static_cast<uint16_t>(base);
        c.si.w = c.bx.w;
        c.di.w = c.cx.w;
        break;
    }
    case 0x0600: case 0x0601: case 0x0702: case 0x0703:
        break;
    case 0x0800: {  // physical address mapping: identity
        break;
    }
    case 0x0900: c.ax.b.l = c.ifl; c.ifl = 0; break;
    case 0x0901: c.ax.b.l = c.ifl; c.ifl = 1; break;
    case 0x0902: c.ax.b.l = c.ifl; break;
    default:
        std::fprintf(stderr, "[DPMI] unhandled INT 31h AX=%04X\n", ax);
        c.cf = 1;
        c.ax.w = 0x8001;
        break;
    }
}

}  // namespace gen
