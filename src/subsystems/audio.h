// Audio subsystem: AdLib OPL2 FM synthesis and Sound Blaster DSP emulation.
#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>

struct _opl3_chip;
typedef struct _opl3_chip opl3_chip;

namespace gen {

class Machine;

class AudioSubsystem {
public:
    AudioSubsystem(Machine& machine);
    ~AudioSubsystem();

    bool init();
    void shutdown();

    // Hardware Port I/O
    uint8_t read_port(uint16_t port);
    void write_port(uint16_t port, uint8_t val);

    // Audio mixing callback
    void generate_samples(int16_t* stream, int length);

    // PCM Playback for Sound Blaster / Creative VOC effects
    void play_pcm_u8(const uint8_t* data, size_t length, uint32_t sample_rate);

private:
    Machine& machine_;
    bool audio_opened_ = false;

    // Mutex for thread-safe audio mixing
    std::mutex audio_mutex_;

    // Nuked OPL3 chip instance
    std::unique_ptr<opl3_chip> opl_chip_;

    // AdLib OPL2 ports 0x388 / 0x389
    uint8_t opl_index_ = 0;
    uint8_t opl_status_ = 0;
    uint8_t opl_timer_ctrl_ = 0;
    bool opl_timer1_active_ = false;
    bool opl_timer2_active_ = false;
    uint8_t opl_regs_[256]{};

    // Sound Blaster DSP ports 0x220..0x22F
    uint8_t sb_reset_ = 0;
    std::vector<uint8_t> sb_read_queue_;
    uint8_t sb_dsp_state_ = 0;

    // Active PCM stream playback
    struct PcmVoice {
        std::vector<uint8_t> data;
        size_t position = 0;
        uint32_t sample_rate = 22050;
        uint32_t step_frac = 0;
        uint32_t step_accum = 0;
        bool active = false;
    } pcm_voice_;
};

}  // namespace gen
