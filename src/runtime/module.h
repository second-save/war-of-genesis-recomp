// Definitions for lifted binary modules and function tables.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gen {

class Machine;
using FunctionPointer = void (*)(Machine&);

struct LiftedFunction {
    uint16_t seg;
    uint16_t off;
    FunctionPointer fn;
};

struct LiftedModule {
    const char* name;
    const char* filename;
    uint16_t load_seg;
    const LiftedFunction* functions;
    size_t num_functions;
    uint16_t init_cs;
    uint16_t init_ip;
};

// 32-bit protected-mode (LE) modules: functions keyed by linear address.
struct LiftedFunction32 {
    uint32_t addr;
    FunctionPointer fn;
};

struct LiftedModule32 {
    const char* name;
    const char* filename;
    const LiftedFunction32* functions;
    size_t num_functions;
    uint32_t entry;
    uint32_t initial_esp;
};

struct ProcessTerminated {
    int exit_code;
};

}  // namespace gen
