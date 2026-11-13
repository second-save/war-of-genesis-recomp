// Audio subsystem: AdLib OPL via Nuked OPL3, Sound Blaster DSP and 8237 DMA.
#include "audio.h"
#include "opl/opl3.h"
#include "runtime/machine.h"

#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gen {

namespace {
bool trace_sb() {
    static const bool t = std::getenv("GENESIS_TRACE_SB") != nullptr;
    return t;
}
void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    static_cast<AudioSubsystem*>(userdata)->generate_samples(reinterpret_cast<int16_t*>(stream),
                                                              len / static_cast<int>(2 * sizeof(int16_t)));
}
constexpr uint64_t kPitHz = 1193182;
}  // namespace

AudioSubsystem::AudioSubsystem(Machine& machine) : machine_(machine) {
    opl_chip_ = std::make_unique<opl3_chip>();
    OPL3_Reset(opl_chip_.get(), kRate);
    dsp_reset();
}

AudioSubsystem::~AudioSubsystem() { shutdown(); }

bool AudioSubsystem::init() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "[SDL] Could not initialize audio: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec wanted{}, obtained{};
    wanted.freq = kRate;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 512;
    wanted.callback = sdl_audio_callback;
    wanted.userdata = this;
    if (SDL_OpenAudio(&wanted, &obtained) < 0) {
        std::fprintf(stderr, "[SDL] Could not open audio device: %s\n", SDL_GetError());
        return false;
    }
    audio_opened_ = true;
    SDL_PauseAudio(0);
    return true;
}

void AudioSubsystem::shutdown() {
    if (audio_opened_) {
        SDL_CloseAudio();
        audio_opened_ = false;
    }
}

bool AudioSubsystem::handles_port(uint16_t port) const {
    if (port >= 0x388 && port <= 0x38B) return true;
    if (port >= 0x220 && port <= 0x22F) return true;
    if (port <= 0x0F) return true;  // DMA controller 1
    if (port == 0x81 || port == 0x82 || port == 0x83 || port == 0x87) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------
uint8_t AudioSubsystem::read_port(uint16_t port) {
    switch (port) {
    case 0x388: case 0x228: case 0x220: case 0x222: {
        uint8_t st = 0;
        if (opl_t1_ && !(opl_timer_ctrl_ & 0x40)) st |= 0x40;
        if (opl_t2_ && !(opl_timer_ctrl_ & 0x20)) st |= 0x20;
        if (st) st |= 0x80;
        return st;
    }
    case 0x22A: {  // DSP read data
        if (out_.empty()) return 0xFF;
        uint8_t v = out_.front();
        out_.pop_front();
        return v;
    }
    case 0x22C:  // DSP write status: bit 7 clear = ready
        return 0x7F;
    case 0x22E:  // DSP read status; acknowledges the 8-bit IRQ
        irq8_pending_ = false;
        return out_.empty() ? 0x7F : 0xFF;
    case 0x22F:
        irq16_pending_ = false;
        return 0xFF;
    case 0x225:
        if (mixer_index_ == 0x82) return (irq8_pending_ ? 1 : 0) | (irq16_pending_ ? 2 : 0);
        return mixer_[mixer_index_];
    case 0x226:
        return 0xFF;
    case 0x08:
        { uint8_t s = dma_status_; dma_status_ &= 0xF0; return s; }
    default:
        break;
    }
    if (port <= 0x07) {  // DMA current address / count
        int ch = port >> 1;
        sync(UINT64_MAX);  // bring the transfer position up to date
        uint16_t v = (port & 1) ? dma_[ch].cur_count : dma_[ch].cur_addr;
        uint8_t b = flipflop_ ? static_cast<uint8_t>(v >> 8) : static_cast<uint8_t>(v);
        flipflop_ = !flipflop_;
        return b;
    }
    return 0xFF;
}

void AudioSubsystem::write_port(uint16_t port, uint8_t val) {
    switch (port) {
    case 0x388: case 0x228: case 0x220: case 0x222:
        opl_index_ = val;
        return;
    case 0x38A:
        opl_index_ = 0x100 | val;
        return;
    case 0x389: case 0x229: case 0x221: case 0x223: case 0x38B: {
        sync(UINT64_MAX);
        if ((opl_index_ & 0xFF) == 0x04 && !(opl_index_ & 0x100)) {
            if (val & 0x80) {
                opl_t1_ = opl_t2_ = false;
            } else {
                opl_timer_ctrl_ = val;
                if (val & 0x01) opl_t1_ = true;
                if (val & 0x02) opl_t2_ = true;
            }
        }
        OPL3_WriteReg(opl_chip_.get(), opl_index_, val);
        if (std::getenv("GENESIS_TRACE_OPL")) {
            static uint32_t count = 0, last = 0;
            ++count;
            uint32_t now = SDL_GetTicks();
            if (now - last >= 1000) {
                std::fprintf(stderr, "[OPL] %u writes (last %03X=%02X)\n", count, opl_index_, val);
                count = 0;
                last = now;
            }
        }
        return;
    }
    case 0x224:
        mixer_index_ = val;
        return;
    case 0x225:
        mixer_[mixer_index_] = val;
        if (mixer_index_ == 0x0E) stereo_ = (val & 0x02) != 0;
        return;
    case 0x226:
        if (val & 1) {
            reset_latch_ = 1;
        } else if (reset_latch_) {
            reset_latch_ = 0;
            dsp_reset();
            out_.push_back(0xAA);
        }
        return;
    case 0x22C:
        dsp_write(val);
        return;
    default:
        break;
    }
    // 8237 DMA controller
    if (port <= 0x07) {
        int ch = port >> 1;
        auto& d = dma_[ch];
        if (port & 1) {
            d.base_count = flipflop_ ? static_cast<uint16_t>((d.base_count & 0x00FF) | (val << 8))
                                     : static_cast<uint16_t>((d.base_count & 0xFF00) | val);
            d.cur_count = d.base_count;
        } else {
            d.base_addr = flipflop_ ? static_cast<uint16_t>((d.base_addr & 0x00FF) | (val << 8))
                                    : static_cast<uint16_t>((d.base_addr & 0xFF00) | val);
            d.cur_addr = d.base_addr;
        }
        flipflop_ = !flipflop_;
        d.terminal = false;
        return;
    }
    switch (port) {
    case 0x0A: dma_[val & 3].masked = (val & 4) != 0; return;
    case 0x0B: dma_[val & 3].mode = val; return;
    case 0x0C: flipflop_ = false; return;
    case 0x0D: for (auto& d : dma_) d.masked = true; flipflop_ = false; return;
    case 0x0E: for (auto& d : dma_) d.masked = false; return;
    case 0x0F: for (int i = 0; i < 4; ++i) dma_[i].masked = (val >> i) & 1; return;
    case 0x87: dma_[0].page = val; return;
    case 0x83: dma_[1].page = val; return;
    case 0x81: dma_[2].page = val; return;
    case 0x82: dma_[3].page = val; return;
    default: return;
    }
}

// ---------------------------------------------------------------------------
// DSP
// ---------------------------------------------------------------------------
void AudioSubsystem::dsp_reset() {
    out_.clear();
    cmd_ = 0;
    args_.clear();
    args_needed_ = 0;
    dma_active_ = false;
    dma_paused_ = false;
    speaker_ = false;
    irq8_pending_ = irq16_pending_ = false;
    stereo_ = false;
}

static int dsp_arg_count(uint8_t c) {
    if (c >= 0xB0 && c <= 0xCF) return 3;
    switch (c) {
    case 0x10: case 0x38: case 0x40: case 0xE0: case 0xE2: case 0xE4: return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x41: case 0x42: case 0x48:
    case 0x74: case 0x75: case 0x76: case 0x77: case 0x80: return 2;
    default: return 0;
    }
}

void AudioSubsystem::dsp_write(uint8_t v) {
    if (args_needed_ == 0) {
        cmd_ = v;
        args_.clear();
        args_needed_ = dsp_arg_count(v);
        if (args_needed_ == 0) dsp_command_complete();
        return;
    }
    args_.push_back(v);
    if (--args_needed_ == 0) dsp_command_complete();
}

void AudioSubsystem::start_dma(uint32_t length, bool auto_init, bool is16) {
    sync(UINT64_MAX);
    dsp_block_ = length;
    dsp_left_ = length;
    dma_auto_ = auto_init;
    dma_16_ = is16;
    dma_active_ = true;
    dma_paused_ = false;
    silence_ = false;
    if (trace_sb())
        std::fprintf(stderr, "[SB] DMA %s %u bytes at %u Hz%s (ch%d addr %X count %u)\n",
                     auto_init ? "auto-init" : "single", length, sample_rate_, stereo_ ? " stereo" : "",
                     sb_dma_, (dma_[sb_dma_].page << 16) | dma_[sb_dma_].cur_addr, dma_[sb_dma_].cur_count + 1u);
}

void AudioSubsystem::dsp_command_complete() {
    auto arg16 = [&](int i) { return static_cast<uint32_t>(args_[i] | (args_[i + 1] << 8)); };
    if (trace_sb() && cmd_ != 0x10)
        std::fprintf(stderr, "[SB] DSP cmd %02X%s\n", cmd_, args_.empty() ? "" : " +args");
    switch (cmd_) {
    case 0x10:  // direct 8-bit output: ignored
        break;
    case 0x14: case 0x16: case 0x17: case 0x74: case 0x75: case 0x76: case 0x77:
        start_dma(arg16(0) + 1, false, false);
        break;
    case 0x1C: case 0x1F: case 0x7D: case 0x7F: case 0x90:
        start_dma(block_size_, true, false);
        break;
    case 0x91:
        start_dma(block_size_, false, false);
        break;
    case 0x40:
        sample_rate_ = 1000000u / (256u - args_[0]);
        if (stereo_) sample_rate_ /= 2;
        break;
    case 0x41: case 0x42:
        sample_rate_ = (static_cast<uint32_t>(args_[0]) << 8) | args_[1];
        break;
    case 0x48:
        block_size_ = arg16(0) + 1;
        break;
    case 0x80:  // silence block: plays nothing, but interrupts after N samples
        start_dma(arg16(0) + 1, false, false);
        silence_ = true;
        break;
    case 0xD0: case 0xD5: dma_paused_ = true; break;
    case 0xD4: case 0xD6: dma_paused_ = false; break;
    case 0xD1: speaker_ = true; break;
    case 0xD3: speaker_ = false; break;
    case 0xD8: out_.push_back(speaker_ ? 0xFF : 0x00); break;
    case 0xD9: case 0xDA: dma_auto_ = false; break;
    case 0xE0: out_.push_back(static_cast<uint8_t>(~args_[0])); break;
    case 0xE1: out_.push_back(0x02); out_.push_back(0x01); break;  // DSP 2.01
    case 0xE4: test_reg_ = args_[0]; break;
    case 0xE8: out_.push_back(test_reg_); break;
    case 0xF2: case 0xF3: irq8_pending_ = true; break;
    default:
        if (cmd_ >= 0xB0 && cmd_ <= 0xCF) {  // SB16 generic DMA
            uint32_t len = arg16(1) + 1;
            bool is16 = cmd_ < 0xC0;
            stereo_ = (args_[0] & 0x20) != 0;
            start_dma(len * (is16 ? 2 : 1), (cmd_ & 0x04) != 0, is16);
        }
        break;
    }
}

int AudioSubsystem::pending_irq() {
    if (irq8_pending_ || irq16_pending_) return sb_irq_;
    return -1;
}

bool AudioSubsystem::dma_read_byte(uint8_t& out) {
    auto& d = dma_[sb_dma_];
    if (d.masked || d.terminal) return false;
    uint32_t lin = (static_cast<uint32_t>(d.page) << 16) | d.cur_addr;
    out = machine_.ram[lin & (Machine::kRamSize - 1)];
    d.cur_addr = static_cast<uint16_t>(d.cur_addr + ((d.mode & 0x20) ? -1 : 1));
    if (d.cur_count-- == 0) {
        dma_status_ |= static_cast<uint8_t>(1 << sb_dma_);
        if (d.mode & 0x10) {  // auto-initialise
            d.cur_addr = d.base_addr;
            d.cur_count = d.base_count;
        } else {
            d.terminal = true;
            d.cur_count = 0xFFFF;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void AudioSubsystem::sync(uint64_t pit_clock) {
    if (pit_clock == UINT64_MAX) pit_clock = machine_.pit_clock_now();
    if (!have_base_) {
        have_base_ = true;
        base_clock_ = pit_clock;
        rendered_ = 0;
    }
    if (pit_clock < base_clock_) return;
    uint64_t target = (pit_clock - base_clock_) * kRate / kPitHz;
    if (target <= rendered_) return;
    uint64_t n = target - rendered_;
    if (n > static_cast<uint64_t>(kRate) / 4) {  // a long stall: don't produce a burst of stale audio
        rendered_ = target - kRate / 4;
        n = kRate / 4;
    }
    render(static_cast<int>(n));
    rendered_ += n;
}

void AudioSubsystem::render(int frames) {
    std::vector<int16_t> buf(static_cast<size_t>(frames) * 2);
    uint32_t step = static_cast<uint32_t>((static_cast<uint64_t>(sample_rate_) << 16) / kRate);
    for (int i = 0; i < frames; ++i) {
        int16_t fm[2];
        OPL3_GenerateResampled(opl_chip_.get(), fm);
        int l = fm[0] * music_volume / 256, r = fm[1] * music_volume / 256;

        if (dma_active_ && !dma_paused_) {
            phase_ += step;
            while (phase_ >= 0x10000 && dma_active_) {
                phase_ -= 0x10000;
                int bytes_per = (dma_16_ ? 2 : 1) * (stereo_ ? 2 : 1);
                int16_t s[2] = {0, 0};
                bool ok = true;
                if (silence_) {
                    // silence
                } else {
                    for (int chn = 0; chn < (stereo_ ? 2 : 1) && ok; ++chn) {
                        if (dma_16_) {
                            uint8_t lo = 0, hi = 0;
                            ok = dma_read_byte(lo) && dma_read_byte(hi);
                            s[chn] = static_cast<int16_t>(lo | (hi << 8));
                        } else {
                            uint8_t b;
                            ok = dma_read_byte(b);
                            s[chn] = static_cast<int16_t>((static_cast<int>(b) - 128) << 8);
                        }
                    }
                }
                if (!stereo_) s[1] = s[0];
                last_l_ = s[0];
                last_r_ = s[1];
                if (dsp_left_ <= static_cast<uint32_t>(bytes_per) || !ok) {
                    irq8_pending_ = !dma_16_ || irq8_pending_;
                    if (dma_16_) irq16_pending_ = true;
                    if (dma_auto_ && ok) {
                        dsp_left_ = dsp_block_;
                    } else {
                        dma_active_ = false;
                        last_l_ = last_r_ = 0;
                    }
                } else {
                    dsp_left_ -= static_cast<uint32_t>(bytes_per);
                }
            }
            l += last_l_ * sfx_volume / 256;
            r += last_r_ * sfx_volume / 256;
        }
        // DC blocker (a real sound card's output is AC-coupled): removes the
        // constant offset the OPL core leaves during silence.
        float fl = static_cast<float>(l), fr = static_cast<float>(r);
        dc_yl_ = fl - dc_xl_ + 0.9995f * dc_yl_;
        dc_yr_ = fr - dc_xr_ + 0.9995f * dc_yr_;
        dc_xl_ = fl;
        dc_xr_ = fr;
        l = static_cast<int>(dc_yl_);
        r = static_cast<int>(dc_yr_);
        buf[static_cast<size_t>(i) * 2] = static_cast<int16_t>(std::clamp(l, -32768, 32767));
        buf[static_cast<size_t>(i) * 2 + 1] = static_cast<int16_t>(std::clamp(r, -32768, 32767));
    }
    // Debug: GENESIS_WAV=<file> appends everything rendered (s16le stereo 44.1 kHz, raw).
    static FILE* dump = [] {
        const char* p = std::getenv("GENESIS_WAV");
        return p ? std::fopen(p, "wb") : nullptr;
    }();
    if (dump) {
        std::fwrite(buf.data(), sizeof(int16_t), buf.size(), dump);
        std::fflush(dump);
    }
    std::lock_guard<std::mutex> lock(ring_mutex_);
    ring_.insert(ring_.end(), buf.begin(), buf.end());
    // Keep latency bounded if the emulation runs ahead of the sound card.
    size_t max_samples = static_cast<size_t>(kRate) / 5 * 2;
    if (ring_.size() > max_samples) ring_.erase(ring_.begin(), ring_.begin() + static_cast<long>(ring_.size() - max_samples));
}

void AudioSubsystem::generate_samples(int16_t* stream, int frames) {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    size_t need = static_cast<size_t>(frames) * 2;
    // Start once ~40 ms are buffered, so small timing jitter doesn't cause gaps.
    if (!primed_) {
        if (ring_.size() < static_cast<size_t>(kRate) / 25 * 2) {
            std::memset(stream, 0, need * sizeof(int16_t));
            return;
        }
        primed_ = true;
    }
    size_t n = std::min(need, ring_.size());
    std::copy(ring_.begin(), ring_.begin() + static_cast<long>(n), stream);
    ring_.erase(ring_.begin(), ring_.begin() + static_cast<long>(n));
    if (n < need) {
        std::memset(stream + n, 0, (need - n) * sizeof(int16_t));
        primed_ = false;
    }
}

}  // namespace gen
