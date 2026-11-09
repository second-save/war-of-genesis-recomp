// Audio subsystem implementation for SDL2 audio output, OPL2, and Sound Blaster.
#include "audio.h"
#include "opl/opl3.h"
#include "runtime/machine.h"

#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace gen {

static void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    auto* audio = static_cast<AudioSubsystem*>(userdata);
    audio->generate_samples(reinterpret_cast<int16_t*>(stream), len / sizeof(int16_t));
}

AudioSubsystem::AudioSubsystem(Machine& machine) : machine_(machine) {
    opl_chip_ = std::make_unique<opl3_chip>();
    OPL3_Reset(opl_chip_.get(), 44100);
}

AudioSubsystem::~AudioSubsystem() {
    shutdown();
}

bool AudioSubsystem::init() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "[SDL] Could not initialize audio: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec wanted{}, obtained{};
    wanted.freq = 44100;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    wanted.callback = sdl_audio_callback;
    wanted.userdata = this;

    if (SDL_OpenAudio(&wanted, &obtained) < 0) {
        std::fprintf(stderr, "[SDL] Could not open audio device: %s\n", SDL_GetError());
        return false;
    }

    audio_opened_ = true;
    SDL_PauseAudio(0); // Start playback
    return true;
}

void AudioSubsystem::shutdown() {
    if (audio_opened_) {
        SDL_CloseAudio();
        audio_opened_ = false;
    }
}

uint8_t AudioSubsystem::read_port(uint16_t port) {
    if (port == 0x388) { // OPL2 Status Register
        uint8_t status = 0;
        if (opl_timer1_active_ && (opl_timer_ctrl_ & 0x01)) status |= 0xC0; // Timer 1 expired + IRQ
        if (opl_timer2_active_ && (opl_timer_ctrl_ & 0x02)) status |= 0xA0; // Timer 2 expired + IRQ
        return status;
    }
    if (port == 0x22A) { // Sound Blaster DSP Read Data
        if (!sb_read_queue_.empty()) {
            uint8_t val = sb_read_queue_.back();
            sb_read_queue_.pop_back();
            return val;
        }
        return 0xAA; // Ready / handshake
    }
    if (port == 0x22E) { // Sound Blaster DSP Read Buffer Status
        return (!sb_read_queue_.empty()) ? 0x80 : 0x00;
    }
    return 0xFF;
}

void AudioSubsystem::write_port(uint16_t port, uint8_t val) {
    if (port == 0x388) { // OPL2 Address Index
        opl_index_ = val;
    } else if (port == 0x389) { // OPL2 Data
        opl_regs_[opl_index_] = val;
        if (opl_index_ == 0x04) { // OPL Timer Control Register
            opl_timer_ctrl_ = val;
            if (val & 0x80) { // Reset timer flags
                opl_timer1_active_ = false;
                opl_timer2_active_ = false;
            } else {
                if (val & 0x01) opl_timer1_active_ = true;
                if (val & 0x02) opl_timer2_active_ = true;
            }
        }
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (opl_chip_) {
            OPL3_WriteReg(opl_chip_.get(), opl_index_, val);
        }
    } else if (port == 0x226) { // Sound Blaster DSP Reset
        if (val == 1) {
            sb_reset_ = 1;
        } else if (val == 0 && sb_reset_ == 1) {
            sb_reset_ = 0;
            sb_read_queue_.clear();
            sb_read_queue_.push_back(0xAA); // Reset ready code
        }
    } else if (port == 0x22C) { // Sound Blaster DSP Write Command / Data
        if (val == 0xE1) { // Get DSP version
            sb_read_queue_.push_back(0x02); // Minor
            sb_read_queue_.push_back(0x02); // Major (DSP 2.01)
        }
    }
}

void AudioSubsystem::play_pcm_u8(const uint8_t* data, size_t length, uint32_t sample_rate) {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    pcm_voice_.data.assign(data, data + length);
    pcm_voice_.position = 0;
    pcm_voice_.sample_rate = sample_rate ? sample_rate : 22050;
    pcm_voice_.step_frac = static_cast<uint32_t>((static_cast<uint64_t>(pcm_voice_.sample_rate) << 16) / 44100);
    pcm_voice_.step_accum = 0;
    pcm_voice_.active = true;
}

void AudioSubsystem::generate_samples(int16_t* stream, int length) {
    // length is total number of int16_t elements; stereo has frames = length / 2
    int num_frames = length / 2;
    std::memset(stream, 0, length * sizeof(int16_t));

    std::lock_guard<std::mutex> lock(audio_mutex_);

    // 1. Synthesize OPL3 audio
    if (opl_chip_) {
        // OPL3_GenerateResampled generates 2 channels (stereo, 4 frames at 44.1k)
        for (int i = 0; i < num_frames; ++i) {
            int16_t frame[2];
            OPL3_GenerateResampled(opl_chip_.get(), frame);
            stream[i * 2 + 0] = frame[0];
            stream[i * 2 + 1] = frame[1];
        }
    }

    // 2. Mix PCM voice if active (Creative VOC / Sound Blaster sample)
    if (pcm_voice_.active && !pcm_voice_.data.empty()) {
        for (int i = 0; i < num_frames; ++i) {
            if (pcm_voice_.position >= pcm_voice_.data.size()) {
                pcm_voice_.active = false;
                break;
            }
            uint8_t u_val = pcm_voice_.data[pcm_voice_.position];
            // Convert unsigned 8-bit [0..255] (128 = silent) to signed 16-bit
            int16_t pcm_sample = static_cast<int16_t>((static_cast<int>(u_val) - 128) << 8);

            // Mix with clipping protection
            int mixed_l = stream[i * 2 + 0] + pcm_sample;
            int mixed_r = stream[i * 2 + 1] + pcm_sample;
            stream[i * 2 + 0] = static_cast<int16_t>(std::clamp(mixed_l, -32768, 32767));
            stream[i * 2 + 1] = static_cast<int16_t>(std::clamp(mixed_r, -32768, 32767));

            pcm_voice_.step_accum += pcm_voice_.step_frac;
            pcm_voice_.position += (pcm_voice_.step_accum >> 16);
            pcm_voice_.step_accum &= 0xFFFF;
        }
    }
}

}  // namespace gen
