# The War of Genesis (창세기전, 1995) — Static Recompilation Architecture & Developer Handover

This document outlines the architecture, static binary lifting pipeline, DOS emulation environment, key findings, and bug fixes implemented to bring *The War of Genesis* (Softmax, 1995) to modern platforms natively (macOS Apple Silicon/Intel, Linux, and Windows) using C++20 and SDL2.

---

## 1. Project Overview & Repository Structure

- **Target Game:** *The War of Genesis* (창세기전 / 創世紀戰, Softmax, 1995).
- **Original Platform:** 16-bit MS-DOS Real Mode (Borland C++ / Turbo C++ runtime, VESA 101h 640x480x256, OPL3 FM-Synthesis & Sound Blaster VOC audio, Johab Korean text encoding).
- **Port Approach:** Native static recompilation (ahead-of-time instruction translation from 8086/80286/80386 16-bit real-mode machine code to idiomatic C++20), backed by an authentic high-performance DOS & hardware abstraction runtime.
- **Directory Layout:**
  - `The-War-of-Genesis_DOS_KO/`: Original DOS distribution directory (game data, `.DEF`, `.PAK`, `.VOC`, `.EXE` binaries, fonts `SAN16.FNT`, `MYUNG16.FNT`, `ENG8.FNT`, etc.).
  - `tools/lifter/`: Custom Python-based static binary lifter (`lift16.py`, `mz.py`, `lift_all.py`, `hints/*.json`).
  - `src/runtime/`: Virtual machine environment, register state (`cpu.h`), memory access (`memory.h`), ALU flag emulation (`alu.h`), module execution loader (`module.h`, `modules_registry.h`), and startup orchestration (`boot_sequence.cpp`).
  - `src/subsystems/`: Hardware abstraction layer:
    - `video.cpp` / `.h`: Software framebuffer, VESA 640x480x256 bank switching (`0xA000:0000`), DAC port emulation (`0x3C8`/`0x3C9`), double buffering, SDL2 texture & Metal/Direct3D/OpenGL presentation.
    - `audio.cpp` / `.h`: Sound Blaster digitized audio (VOC PCM streaming) + integrated Nuked OPL3 synthesizer for MIDI/FM music.
    - `input.cpp` / `.h`: Keyboard matrix scan-codes, INT 16h / INT 33h mouse pointer handling with coordinate scaling.
    - `dos.cpp` / `.h`: MS-DOS INT 21h emulation (file I/O, memory allocation MCBs, EXEC `AH=4Bh`, find-first/next `DTA`, system clock).
    - `font_vwf.cpp` / `.h`: Johab 16x16 Korean font renderer with proportional variable-width font (VWF) engine support.
  - `src/modules/generated/`: Lifted C++ code divided into separate static libraries for each original executable:
    - `main`: `MAIN.EXE` (top-level launcher, authorization check, save loading).
    - `chp`: `CHP.EXE` (campaign gameplay, battle engine, tactical grid, cutscenes).
    - `chrsys`: `CHRSYS.EXE` (character status, equipment, skills, party management).
    - `pack`: `PACK.EXE` (resource archiver and extractor).
  - `build/`: CMake build output directory (`ninja -C build`).

---

## 2. Binary Lifter & Static Recompilation Engine

### 2.1 MZ Binary Parsing & Segment Relocation
Original MS-DOS executables are MZ format 16-bit real-mode binaries with multiple code and data segments.
- `tools/lifter/mz.py`: Parses the MZ header, reads the relocation table, and groups segments by relocation target addresses.
- `tools/lifter/lift16.py`: Uses Capstone disassembly to decode 16-bit x86 instructions. It reconstructs control flow graphs (CFGs), resolves far calls, near calls, conditional jumps, indirect calls, and converts segments into C++ functions.
- Every function is named `F_<seg>_<offset>(Machine& m)`.
- Segment bases in DOS memory are dynamically bound at runtime. A relocation offset `kLoadSeg` is added to segment references:
  $$\text{effective\_segment} = \text{static\_segment} + kLoadSeg$$

### 2.2 Recompiled Calling Convention & Runtime State
- The CPU state is encapsulated in `gen::Cpu` within `gen::Machine` ([`src/runtime/cpu.h`](file:///Users/antoineclerc-renaud/develop/genesis/src/runtime/cpu.h)):
  - 16-bit registers: `ax`, `bx`, `cx`, `dx`, `si`, `di`, `bp`, `sp`.
  - Segment registers: `cs`, `ds`, `es`, `ss`.
  - Status flags: `cf`, `zf`, `sf`, `of`, `df`, `pf`, `af`, `if_flag`.
- Memory operations use free helper functions:
  - Read: `rd8(m, seg, off)`, `rd16(m, seg, off)`
  - Write: `wr8(m, seg, off, val)`, `wr16(m, seg, off, val)`
  - Stack: `push16(m, val)`, `pop16(m)`
- The function dispatch table ([`chp_table.cpp`](file:///Users/antoineclerc-renaud/develop/genesis/src/modules/generated/chp/chp_table.cpp), etc.) maps `(cs, ip)` pairs to compiled C++ function pointers for both far and near indirect calls.

---

## 3. DOS & Hardware Subsystems Architecture

### 3.1 Memory Architecture
- Virtual 1 MB (or 16 MB extended) physical address space:
  - `0x00000..0x003FF`: Interrupt Vector Table (IVT, 256 entries $\times$ 4 bytes).
  - `0x00400..0x004FF`: BIOS Data Area (BDA).
  - `0x00500..0x005FF`: DOS Data Area / Communication buffers.
  - `0x01000..0x9FFFF`: Conventional memory (allocated to program segments via DOS MCB arena).
  - `0xA0000..0xAFFFF`: VESA / VGA Video RAM (64 KB window at `0xA000:0000` with bank banking).
- Far addresses are calculated via standard x86 real-mode segment:offset arithmetic:
  $$\text{phys\_addr} = (\text{seg} \ll 4) + \text{offset}$$

### 3.2 Video & Graphics
- **Resolution**: 640 $\times$ 480 8-bit indexed color (VESA Mode 101h).
- **VRAM Bank Switching**: The original game selects 64 KB banks into `0xA000` by calling INT 10h `AX=4F05h` or by calling a saved 16-bit bank function pointer (`F_121A_01D4`). `VideoSubsystem::set_vesa_bank(bank)` handles this mapping.
- **Palette Management**: 256-color DAC programmed via I/O ports `0x3C8` (index) and `0x3C9` (data).
  - *Note*: Standard VGA DAC uses 6-bit color components ($0\dots63$). Some files (like `.PAL`) store raw 6-bit values, requiring `(val * 255) / 63` scaling when loading directly into 8-bit RGBA texture buffers.
- **Presentation**: Rendered into an internal 640x480 RGBA buffer and upscaled with aspect-ratio preservation to the SDL2 window with accelerated Metal/OpenGL rendering.

### 3.3 Audio Subsystem
- **Digitized Sound**: Softmax voice logos and sound effects are stored in Creative Voice (`.VOC`) format (e.g. `TTLDATA/LOGO.VOC`). Parsed and streamed as 8-bit unsigned PCM through SDL Audio callbacks.
- **Music**: Background music is synthesized via OPL3 FM registers (`0x388`, `0x389`).

### 3.4 Multi-Module Execution (`INT 21h AH=4Bh`)
Under DOS, the game flow consists of chained executables:
1. `MAIN.EXE` runs first, verifies authorization (`cstldata/pass.`), determines whether to start a new game or resume a save, and launches `CHP.EXE` via `INT 21h AH=4Bh`.
2. `CHP.EXE` executes the campaign chapter, scenarios, cutscenes, and battles.
3. During gameplay, `CHP.EXE` may spawn `CHRSYS.EXE` to manage character stats, skills, or items, then returns to `CHP.EXE`.
4. Our runtime emulates `AH=4Bh` by saving the caller CPU state, looking up the target executable in `ModulesRegistry`, initializing its code/data segments, and invoking its entry point before returning cleanly to the caller.

---

## 4. Critical Bugs Identified & Resolved

### 4.1 Missing Softmax Corporate Logo & Early Boot Skipping
- **Symptom**: On startup, only the G&M logo was displayed; the Softmax logo was black, skipped, or missing.
- **Root Causes**:
  1. *Palette Scaling*: `TTLDATA/LOGO.PAL` stores 768 bytes of 6-bit DAC values ($0\dots63$). When `is_6bit` was set to `false`, the palette was interpreted as 8-bit values ($0\dots255$), rendering colors nearly pitch black (max intensity 63/255 = ~24%).
  2. *Event Queue Contamination*: Launching from the terminal left keyboard events in the SDL event queue. The logo wait timer (`wait_for_key_or_timeout`) polled `SDL_PollEvent`, immediately saw the pending keystroke, and skipped the logo in 0 ms.
- **Fix**:
  - In [`src/runtime/boot_sequence.cpp`](file:///Users/antoineclerc-renaud/develop/genesis/src/runtime/boot_sequence.cpp):
    - Added `SDL_PumpEvents()` and `SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT)` inside `wait_for_key_or_timeout()` before entering the wait loop.
    - Passed `true` (`is_6bit`) to `machine.video->set_palette_rgb(pal.data(), true)`.
  - Result: Softmax corporate logo and accompanying "Softmax" voice audio display and play authentically for ~2.5 seconds before transitioning to the title logo.

### 4.2 The "Black Screen" Infinite Loop on Menu Selection (INT 1Ch Timer)
- **Symptom**: Selecting "처음부터 하기 (New Game)" or other menu options resulted in a permanent black screen with CPU execution frozen in a tight loop.
- **Root Cause Analysis**:
  1. Tracing execution into `CHP.EXE` revealed that the game enters function `F_12F8_037C`.
  2. This function performs a 64-step smooth palette fade-in from pure black ($0,0,0$) to the target scene colors.
  3. On each iteration of the fade loop, `F_12F8_037C` calls `F_02F7_00C7`, which polls memory address `[0x4524+kLoadSeg:0x3B2E]` waiting for it to become non-zero.
  4. Investigation in Ghidra and the disassembled source showed that `[0x3B2E]` is **only** set to `1` by an interrupt handler installed at `0x02F7:0x0003`.
  5. During startup, `CHP.EXE` calls `setvect(0x1C, 0x02F7:0x0003)` via DOS `INT 21h AH=25h` to hook the BIOS User Timer Tick interrupt (`INT 1Ch`).
  6. In our runtime, `Machine::frame_sync()` only invoked `INT 08h` (system timer) and did not dispatch `INT 1Ch`. Because `INT 1Ch` was never fired, `[0x3B2E]` remained `0` forever. `F_12F8_037C` was trapped at step 0 of the fade loop indefinitely, leaving the screen completely black.
- **Fix**:
  - Implemented the lifted interrupt handler `F_02F7_0003` in [`src/modules/generated/chp/chp_seg02F7.cpp`](file:///Users/antoineclerc-renaud/develop/genesis/src/modules/generated/chp/chp_seg02F7.cpp):
    ```cpp
    void F_02F7_0003(Machine& m) {
        Cpu& c = m.cpu;
        (void)c;
        // Sets the frame tick flag at [ds:0x3B2E]
        wr16(m, c.ds, 0x3B2E, 1);
        // iret
    }
    ```
  - Declared `F_02F7_0003` in [`chp_decl.h`](file:///Users/antoineclerc-renaud/develop/genesis/src/modules/generated/chp/chp_decl.h) and registered it in [`chp_table.cpp`](file:///Users/antoineclerc-renaud/develop/genesis/src/modules/generated/chp/chp_table.cpp) at entry `0x02F7:0x0003`.
  - Updated `Machine::frame_sync()` in [`src/runtime/machine.cpp`](file:///Users/antoineclerc-renaud/develop/genesis/src/runtime/machine.cpp) to dispatch `INT 1Ch` if an interrupt vector is present in the IVT (`0000:0070`).
  - Result: The fade loop ticks 64 times, the screen brightens smoothly, and the chapter initializes properly.

### 4.3 New Game / Save State State Machine Handshake
- **Symptom**: `MAIN.EXE` was previously crashing or jumping to `CHRSYS.EXE ' ? hacker'` instead of starting Chapter 0.
- **Analysis of `MAIN.EXE` `main()` (`F_0259_02DB`)**:
  - `MAIN.EXE` inspects the filesystem to determine program flow:
    1. Checks if `cstldata/pass.` exists. If absent, it exits immediately with a black screen (`mov ax, 3; int 10h; retf`).
    2. Checks if `cstldata/load.` exists:
       - If `cstldata/load.` exists: It unlinks `load.` and reads `continue.sve`. If `continue.sve` contains a chapter number $\ge 100$, it invokes `CHRSYS.EXE ' ? hacker'`. Otherwise, it launches `CHP.EXE ' p'`.
       - If `cstldata/load.` does NOT exist: It treats this as a New Game. It unlinks `continue.sve`, sets up `contibak.tmp` from `0.CHP`, and invokes `CHP.EXE ' p'`.
- **Fix**:
  - In `copy_default_save_files()` ([`src/runtime/boot_sequence.cpp`](file:///Users/antoineclerc-renaud/develop/genesis/src/runtime/boot_sequence.cpp)):
    - Ensure `continue.sve` and `cstldata/LOAD.` are removed for New Game.
    - Copy `CHAPDATA/0.CHP` to `CHAPDATA/contibak.tmp`.
    - Touch `cstldata/pass.` with `"OK"` before invoking `MAIN.EXE`.
  - In `load_save_slot()`:
    - Copy `contibak.sv<N>` to `contibak.tmp` and restore save slot `.SWP` files.
    - Touch `cstldata/LOAD.` and `cstldata/pass.`.

---

## 5. Building & Running the Project

### Prerequisites
- macOS (Apple Silicon or Intel), Linux, or Windows.
- C++20 compliant compiler (Clang 15+, GCC 12+, or MSVC 2022).
- CMake 3.22+ and Ninja build system.
- SDL2 development libraries (`brew install sdl2` on macOS).

### Build Instructions
```bash
cd /Users/antoineclerc-renaud/develop/genesis
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

### Running the Game
```bash
./build/genesis
```
- **Controls**:
  - Arrow keys / `W`, `S` / Numpad 8, 2: Navigate menu options.
  - `Enter` / `Space` / Left Mouse Click: Select option.
  - `Esc` / `Cmd+Q` (macOS) / `Alt+F4`: Exit game cleanly.
  - Mouse: Full point-and-click support.

---

## 6. Next Steps & Translation Roadmap

With the native executable running stably and transitioning smoothly into Chapter 0, subsequent development can proceed to the translation pipeline:

1. **Korean Text Encoding Architecture**:
   - The game uses 16-bit Johab encoding (combination of 5-bit initial consonant, 5-bit vowel, 5-bit final consonant).
   - Standard 16x16 Korean glyphs are loaded from `SAN16.FNT` and `MYUNG16.FNT`.
   - English 8x16 glyphs are loaded from `ENG8.FNT`.
2. **Variable-Width Font (VWF) Engine**:
   - In [`src/subsystems/font_vwf.cpp`](file:///Users/antoineclerc-renaud/develop/genesis/src/subsystems/font_vwf.cpp), the VWF engine already supports proportional spacing and character-by-character blitting.
   - Dialogue boxes in `CHP.EXE` can be hooked to intercept string rendering calls, redirecting English ASCII strings through the VWF blitter with proportional glyph metrics.
3. **Script Extraction & Insertion**:
   - Scenario and dialogue data reside in `CHAPDATA/*.CHP` and script files.
   - Text pointers inside `CHP.EXE` and scenario tables can be dumped using a custom script, translated into English, and re-injected with updated pointer offsets.
