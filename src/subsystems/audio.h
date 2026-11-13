// Audio subsystem: AdLib (OPL2/3 via Nuked OPL3), Sound Blaster DSP + 8237 DMA.
//
// Sound is rendered on the emulation thread against the PIT timeline (see
// Machine::service_interrupts): before every timer tick the mixer is advanced
// to that tick's time, so register writes made by the game's music driver land
// at the right sample. The SDL callback only drains a ring buffer.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <deque>

struct _opl3_chip;
typedef struct _opl3_chip opl3_chip;

namespace gen {

class Machine;

class AudioSubsystem {
public:
    static constexpr int kRate = 44100;

    explicit AudioSubsystem(Machine& machine);
    ~AudioSubsystem();

    bool init();
    void shutdown();

    uint8_t read_port(uint16_t port);
    void write_port(uint16_t port, uint8_t val);
    bool handles_port(uint16_t port) const;

    // Render audio up to the given PIT clock count (1193182 Hz timeline).
    void sync(uint64_t pit_clock);

    // Card configuration (must match the game's GENESIS.CFG).
    void configure(int irq, int dma) { sb_irq_ = irq; sb_dma_ = dma; }

    // Sound Blaster interrupt waiting to be delivered (IRQ line, or -1).
    int pending_irq();
    int sb_irq() const { return sb_irq_; }

    // SDL callback side.
    void generate_samples(int16_t* stream, int frames);

    // Master volume controls (0..256).
    int music_volume = 256;
    int sfx_volume = 256;

private:
    void render(int frames);
    void dsp_write(uint8_t v);
    void dsp_command_complete();
    void dsp_reset();
    void start_dma(uint32_t length, bool auto_init, bool is16);
    int dma_channel() const { return sb_dma_; }

    Machine& machine_;
    bool audio_opened_ = false;
    std::unique_ptr<opl3_chip> opl_chip_;
    uint16_t opl_index_ = 0;
    uint8_t opl_timer_ctrl_ = 0;
    bool opl_t1_ = false, opl_t2_ = false;

    // Rendering timeline
    bool have_base_ = false;
    uint64_t base_clock_ = 0;
    uint64_t rendered_ = 0;  // frames rendered since base

    // Ring buffer to the SDL thread (interleaved stereo).
    std::mutex ring_mutex_;
    std::deque<int16_t> ring_;
    bool primed_ = false;

    // --- Sound Blaster DSP ---
    int sb_irq_ = 7;
    int sb_dma_ = 1;
    uint8_t reset_latch_ = 0;
    std::deque<uint8_t> out_;  // bytes the program reads from 22Ah
    uint8_t cmd_ = 0;
    std::vector<uint8_t> args_;
    int args_needed_ = 0;
    uint32_t sample_rate_ = 11025;
    bool stereo_ = false;
    uint32_t block_size_ = 0x800;
    bool speaker_ = false;
    bool irq8_pending_ = false;
    bool irq16_pending_ = false;
    uint8_t mixer_index_ = 0;
    uint8_t mixer_[256]{};
    uint8_t test_reg_ = 0;

    // DSP DMA transfer in progress
    bool dma_active_ = false;
    bool dma_paused_ = false;
    bool dma_auto_ = false;
    bool dma_16_ = false;
    bool silence_ = false;
    uint32_t dsp_left_ = 0;   // bytes left in the current DSP block
    uint32_t dsp_block_ = 0;  // DSP block length (bytes)
    uint32_t phase_ = 0;      // 16.16 resampling phase
    int16_t last_l_ = 0, last_r_ = 0;
    float dc_xl_ = 0, dc_xr_ = 0, dc_yl_ = 0, dc_yr_ = 0;

    // --- 8237 DMA controller (channels 0-3) ---
    struct DmaChannel {
        uint16_t base_addr = 0, base_count = 0;
        uint16_t cur_addr = 0, cur_count = 0;
        uint8_t page = 0;
        uint8_t mode = 0;
        bool masked = true;
        bool terminal = false;
    } dma_[4];
    bool flipflop_ = false;
    uint8_t dma_status_ = 0;
    bool dma_read_byte(uint8_t& out);
};

}  // namespace gen
