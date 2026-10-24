#include "sound.hpp"
#include "ac97.hpp"
#include "sb16.hpp"
#include "speaker.hpp"
#include "ramfs.hpp"
#include "pit.hpp"
#include "heap.hpp"
#include "kstring.hpp"
#include "printk.hpp"

namespace sound {

static DriverType s_driver = DriverType::None;
static uint8_t s_volume = 80;
static bool s_muted = false;
static bool s_gui_cues_enabled = true;

// 1 second of 48kHz 16-bit stereo PCM (48000 frames * 4 bytes = 192KB)
alignas(4096) static int16_t s_synth_buf[48000 * 2];

// 256-entry sine lookup table (-32767 to +32767)
static const int16_t SINE_LUT[256] = {
        0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
     6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
    12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
    18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
    23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
    27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
    30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
    32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
    32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
    32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
    30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
    27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
    23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
    18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
    12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
     6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
        0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
    -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
   -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
   -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
   -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
   -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
   -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
   -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
   -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
   -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
   -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
   -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
   -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
   -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
   -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
    -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804
};

static inline int16_t get_waveform_sample(Waveform wave, uint32_t phase) {
    switch (wave) {
        case Waveform::Sine:
            return SINE_LUT[(phase >> 8) & 0xFF];
        case Waveform::Square:
            return ((phase >> 15) & 1) ? 22000 : -22000;
        case Waveform::Triangle: {
            uint16_t p = static_cast<uint16_t>(phase & 0xFFFF);
            int32_t v = (p < 32768) ? ((static_cast<int32_t>(p) - 16384) * 2)
                                    : ((49152 - static_cast<int32_t>(p)) * 2);
            return static_cast<int16_t>(v * 28000 / 32768);
        }
        case Waveform::Sawtooth: {
            uint16_t p = static_cast<uint16_t>(phase & 0xFFFF);
            return static_cast<int16_t>(((static_cast<int32_t>(p) - 32768) * 24000) / 32768);
        }
    }
    return 0;
}

void init() {
    // 1. Try Intel AC97 PCI Audio Controller
    if (ac97::init()) {
        s_driver = DriverType::AC97;
        printk("[OK] Intel AC97 Audio Controller active (NAMBAR: 0x%x, NABMBAR: 0x%x, Rate: %u Hz).\n",
               ac97::get_nambar(), ac97::get_nabmbar(), ac97::get_sample_rate());
    }
    // 2. Try Sound Blaster 16 DSP
    else if (sb16::init()) {
        s_driver = DriverType::SB16;
        printk("[OK] Sound Blaster 16 DSP active (Base: 0x%x, DSP v%u.%u).\n",
               sb16::get_base_port(), sb16::get_version_major(), sb16::get_version_minor());
    }
    // 3. Fallback to PC Speaker
    else {
        s_driver = DriverType::PCSpeaker;
        printk("[OK] Audio Subsystem initialized with PC Speaker fallback (PIT Port 0x61).\n");
    }

    s_volume = 80;
    s_muted = false;
    s_gui_cues_enabled = true;

    set_volume(s_volume);
}

DriverType get_driver_type() {
    return s_driver;
}

const char* get_driver_name() {
    switch (s_driver) {
        case DriverType::AC97:
            return "Intel AC97 PCI Audio Controller (82801AA)";
        case DriverType::SB16:
            return "Creative Sound Blaster 16 (DSP)";
        case DriverType::PCSpeaker:
            return "PC Speaker (PIT Timer 2 Square Wave)";
        default:
            return "No Audio Device";
    }
}

bool is_hardware_available() {
    return (s_driver != DriverType::None);
}

void set_volume(uint8_t volume_percent) {
    if (volume_percent > 100) volume_percent = 100;
    s_volume = volume_percent;

    if (s_driver == DriverType::AC97) {
        ac97::set_master_volume(volume_percent);
        ac97::set_pcm_volume(volume_percent);
    } else if (s_driver == DriverType::SB16) {
        sb16::set_master_volume(volume_percent);
    }
}

uint8_t get_volume() {
    return s_volume;
}

void set_mute(bool mute) {
    s_muted = mute;

    if (s_driver == DriverType::AC97) {
        ac97::set_mute(mute);
    } else if (s_driver == DriverType::SB16) {
        sb16::set_mute(mute);
    }
}

bool is_muted() {
    return s_muted;
}

bool is_playing() {
    if (s_driver == DriverType::AC97) {
        return ac97::is_playing();
    }
    return false;
}

void stop() {
    if (s_driver == DriverType::AC97) {
        ac97::stop();
    }
    speaker::stop();
}

void wait_idle() {
    if (s_driver == DriverType::AC97) {
        ac97::wait_completion(10000);
    }
}

void play_tone(uint32_t freq_hz, uint32_t duration_ms, Waveform wave, bool blocking) {
    if (s_muted || freq_hz == 0 || duration_ms == 0) {
        if (duration_ms > 0 && blocking) pit::sleep(duration_ms);
        return;
    }

    if (s_driver == DriverType::AC97) {
        size_t num_frames = (duration_ms * 48000ULL) / 1000;
        if (num_frames > 48000) num_frames = 48000;

        uint32_t phase = 0;
        uint32_t phase_step = (static_cast<uint64_t>(freq_hz) * 65536) / 48000;

        size_t attack = 240; // 5ms attack
        size_t decay = 480;  // 10ms decay
        if (num_frames < attack + decay) {
            attack = num_frames / 4;
            decay = num_frames / 4;
        }

        for (size_t i = 0; i < num_frames; ++i) {
            int32_t env = 32767;
            if (i < attack && attack > 0) {
                env = (i * 32767) / attack;
            } else if (i > num_frames - decay && decay > 0) {
                env = ((num_frames - i) * 32767) / decay;
            }

            int16_t sample = static_cast<int16_t>((get_waveform_sample(wave, phase) * env) / 32768);
            s_synth_buf[i * 2 + 0] = sample;
            s_synth_buf[i * 2 + 1] = sample;

            phase += phase_step;
        }

        ac97::play_pcm(s_synth_buf, num_frames, 48000, blocking);
    } else {
        speaker::beep(freq_hz, duration_ms);
    }
}

void play_chord(const uint32_t* freqs, size_t count, uint32_t duration_ms, bool blocking) {
    if (s_muted || !freqs || count == 0 || duration_ms == 0) {
        if (duration_ms > 0 && blocking) pit::sleep(duration_ms);
        return;
    }

    if (s_driver == DriverType::AC97) {
        size_t num_frames = (duration_ms * 48000ULL) / 1000;
        if (num_frames > 48000) num_frames = 48000;

        uint32_t phases[8];
        uint32_t steps[8];
        size_t active_count = count > 8 ? 8 : count;

        for (size_t c = 0; c < active_count; ++c) {
            phases[c] = 0;
            steps[c] = (static_cast<uint64_t>(freqs[c]) * 65536) / 48000;
        }

        size_t attack = 240;
        size_t decay = 480;

        for (size_t i = 0; i < num_frames; ++i) {
            int32_t sum = 0;
            for (size_t c = 0; c < active_count; ++c) {
                sum += SINE_LUT[(phases[c] >> 8) & 0xFF];
                phases[c] += steps[c];
            }
            sum /= static_cast<int32_t>(active_count);

            int32_t env = 32767;
            if (i < attack) env = (i * 32767) / attack;
            else if (i > num_frames - decay) env = ((num_frames - i) * 32767) / decay;

            int16_t sample = static_cast<int16_t>((sum * env) / 32768);
            s_synth_buf[i * 2 + 0] = sample;
            s_synth_buf[i * 2 + 1] = sample;
        }

        ac97::play_pcm(s_synth_buf, num_frames, 48000, blocking);
    } else {
        // Fast retro arpeggio on PC speaker
        uint32_t per_note = duration_ms / count;
        if (per_note < 20) per_note = 20;
        for (size_t i = 0; i < count; ++i) {
            speaker::beep(freqs[i], per_note);
        }
    }
}

void play_melody(const Note* notes, size_t count, bool blocking) {
    if (!notes || count == 0) return;

    for (size_t i = 0; i < count; ++i) {
        if (notes[i].freq_hz == 0) {
            pit::sleep(notes[i].duration_ms);
        } else {
            play_tone(notes[i].freq_hz, notes[i].duration_ms, Waveform::Sine, true);
            pit::sleep(15); // Small articulation gap
        }
    }

    if (!blocking) {
        // Already played sequentially
    }
}

void play_effect(Effect effect, bool blocking) {
    if (s_muted) return;

    switch (effect) {
        case Effect::Beep: {
            play_tone(880, 100, Waveform::Sine, blocking);
            break;
        }
        case Effect::Chime: {
            if (s_driver == DriverType::AC97) {
                // Multi-frequency bell chord with exponential decay
                constexpr size_t num_frames = (48000 * 420) / 1000;
                uint32_t p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0;
                uint32_t s1 = (523ULL * 65536) / 48000;  // C5
                uint32_t s2 = (659ULL * 65536) / 48000;  // E5
                uint32_t s3 = (784ULL * 65536) / 48000;  // G5
                uint32_t s4 = (1046ULL * 65536) / 48000; // C6
                uint32_t s5 = (1568ULL * 65536) / 48000; // G6

                for (size_t i = 0; i < num_frames; ++i) {
                    int32_t env1 = (num_frames - i) * 32767 / num_frames;
                    int32_t env2 = (i < num_frames * 4 / 5) ? (num_frames * 4 / 5 - i) * 32767 / (num_frames * 4 / 5) : 0;
                    int32_t env3 = (i < num_frames * 3 / 5) ? (num_frames * 3 / 5 - i) * 32767 / (num_frames * 3 / 5) : 0;
                    int32_t env4 = (i < num_frames * 2 / 5) ? (num_frames * 2 / 5 - i) * 32767 / (num_frames * 2 / 5) : 0;
                    int32_t env5 = (i < num_frames / 4) ? (num_frames / 4 - i) * 32767 / (num_frames / 4) : 0;

                    int32_t v1 = (SINE_LUT[(p1 >> 8) & 0xFF] * env1) / 32768;
                    int32_t v2 = (SINE_LUT[(p2 >> 8) & 0xFF] * env2) / 32768;
                    int32_t v3 = (SINE_LUT[(p3 >> 8) & 0xFF] * env3) / 32768;
                    int32_t v4 = (SINE_LUT[(p4 >> 8) & 0xFF] * env4) / 32768;
                    int32_t v5 = (SINE_LUT[(p5 >> 8) & 0xFF] * env5) / 32768;

                    // Spatial stereo spread
                    int32_t left = (v1 * 8 + v2 * 5 + v3 * 7 + v4 * 4 + v5 * 6) / 30;
                    int32_t right = (v1 * 5 + v2 * 8 + v3 * 4 + v4 * 7 + v5 * 6) / 30;

                    s_synth_buf[i * 2 + 0] = static_cast<int16_t>(left);
                    s_synth_buf[i * 2 + 1] = static_cast<int16_t>(right);

                    p1 += s1; p2 += s2; p3 += s3; p4 += s4; p5 += s5;
                }

                ac97::play_pcm(s_synth_buf, num_frames, 48000, blocking);
            } else {
                // Speaker ascending chime
                speaker::beep(523, 70);
                speaker::beep(659, 70);
                speaker::beep(784, 70);
                speaker::beep(1046, 160);
            }
            break;
        }
        case Effect::Laser: {
            if (s_driver == DriverType::AC97) {
                // Pitch-swept synth effect from 2400 Hz down to 250 Hz
                constexpr size_t num_frames = (48000 * 160) / 1000;
                uint32_t phase = 0;

                for (size_t i = 0; i < num_frames; ++i) {
                    uint32_t freq = 2400 - static_cast<uint32_t>((2150ULL * i) / num_frames);
                    uint32_t step = (static_cast<uint64_t>(freq) * 65536) / 48000;

                    int32_t env = (num_frames - i) * 32767 / num_frames;
                    int16_t raw = get_waveform_sample(Waveform::Sawtooth, phase);
                    int16_t sample = static_cast<int16_t>((raw * env) / 32768);

                    s_synth_buf[i * 2 + 0] = sample;
                    s_synth_buf[i * 2 + 1] = sample;

                    phase += step;
                }

                ac97::play_pcm(s_synth_buf, num_frames, 48000, blocking);
            } else {
                for (uint32_t f = 2400; f >= 400; f -= 350) {
                    speaker::beep(f, 20);
                }
            }
            break;
        }
        case Effect::Coin: {
            if (s_driver == DriverType::AC97) {
                // Retro 8-bit coin pickup arpeggio (B5 -> E6)
                constexpr size_t frames1 = (48000 * 60) / 1000;
                constexpr size_t frames2 = (48000 * 220) / 1000;
                constexpr size_t total = frames1 + frames2;

                uint32_t phase = 0;
                uint32_t step1 = (987ULL * 65536) / 48000;  // B5
                uint32_t step2 = (1318ULL * 65536) / 48000; // E6

                for (size_t i = 0; i < frames1; ++i) {
                    int16_t raw = get_waveform_sample(Waveform::Square, phase);
                    s_synth_buf[i * 2 + 0] = raw;
                    s_synth_buf[i * 2 + 1] = raw;
                    phase += step1;
                }

                for (size_t i = 0; i < frames2; ++i) {
                    int32_t env = (frames2 - i) * 32767 / frames2;
                    int16_t raw = get_waveform_sample(Waveform::Square, phase);
                    int16_t sample = static_cast<int16_t>((raw * env) / 32768);
                    s_synth_buf[(frames1 + i) * 2 + 0] = sample;
                    s_synth_buf[(frames1 + i) * 2 + 1] = sample;
                    phase += step2;
                }

                ac97::play_pcm(s_synth_buf, total, 48000, blocking);
            } else {
                speaker::beep(987, 60);
                speaker::beep(1318, 220);
            }
            break;
        }
        case Effect::Fanfare: {
            if (s_driver == DriverType::AC97) {
                // Victory jingle
                Note fanfare_notes[] = {
                    { 392, 90 },  // G4
                    { 523, 90 },  // C5
                    { 659, 90 },  // E5
                    { 784, 320 }  // G5
                };
                play_melody(fanfare_notes, 4, blocking);
            } else {
                speaker::play_tune();
            }
            break;
        }
    }
}

// WAV File Structures
struct WavHeader {
    char riff[4];
    uint32_t size;
    char wave[4];
} __attribute__((packed));

struct ChunkHeader {
    char id[4];
    uint32_t size;
} __attribute__((packed));

struct FmtChunk {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} __attribute__((packed));

bool play_wav(const void* data, size_t size, bool blocking) {
    if (s_muted || !data || size < 44) return false;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    const WavHeader* hdr = reinterpret_cast<const WavHeader*>(bytes);

    if (kstd::strncmp(hdr->riff, "RIFF", 4) != 0 || kstd::strncmp(hdr->wave, "WAVE", 4) != 0) {
        return false;
    }

    size_t offset = 12;
    const FmtChunk* fmt = nullptr;
    const uint8_t* pcm_data = nullptr;
    size_t pcm_size = 0;

    while (offset + sizeof(ChunkHeader) <= size) {
        const ChunkHeader* chunk = reinterpret_cast<const ChunkHeader*>(bytes + offset);
        offset += sizeof(ChunkHeader);

        if (kstd::strncmp(chunk->id, "fmt ", 4) == 0) {
            if (chunk->size >= 16 && offset + 16 <= size) {
                fmt = reinterpret_cast<const FmtChunk*>(bytes + offset);
            }
        } else if (kstd::strncmp(chunk->id, "data", 4) == 0) {
            pcm_data = bytes + offset;
            pcm_size = chunk->size;
            if (offset + pcm_size > size) {
                pcm_size = size - offset;
            }
            break;
        }

        offset += chunk->size;
        if (chunk->size & 1) offset++; // Word-align chunks
    }

    if (!fmt || !pcm_data || pcm_size == 0) return false;
    if (fmt->format != 1) return false; // Only PCM supported
    if (fmt->channels != 1 && fmt->channels != 2) return false;
    if (fmt->bits_per_sample != 8 && fmt->bits_per_sample != 16) return false;

    if (s_driver == DriverType::AC97) {
        size_t bytes_per_in_frame = (fmt->bits_per_sample / 8) * fmt->channels;
        size_t total_in_frames = pcm_size / bytes_per_in_frame;
        if (total_in_frames == 0) return false;

        // Resample to 48000 Hz 16-bit stereo
        size_t total_out_frames = (static_cast<uint64_t>(total_in_frames) * 48000) / fmt->sample_rate;
        if (total_out_frames > 48000 * 8) { // Cap at 8 seconds to prevent large allocations
            total_out_frames = 48000 * 8;
        }

        size_t out_bytes = total_out_frames * 2 * sizeof(int16_t);
        int16_t* out_buf = static_cast<int16_t*>(kmalloc(out_bytes));
        if (!out_buf) return false;

        const int16_t* in16 = reinterpret_cast<const int16_t*>(pcm_data);

        for (size_t j = 0; j < total_out_frames; ++j) {
            size_t in_idx = (static_cast<uint64_t>(j) * fmt->sample_rate) / 48000;
            if (in_idx >= total_in_frames) in_idx = total_in_frames - 1;

            int16_t left = 0, right = 0;

            if (fmt->bits_per_sample == 8) {
                if (fmt->channels == 1) {
                    left = right = (static_cast<int16_t>(pcm_data[in_idx]) - 128) << 8;
                } else {
                    left = (static_cast<int16_t>(pcm_data[in_idx * 2 + 0]) - 128) << 8;
                    right = (static_cast<int16_t>(pcm_data[in_idx * 2 + 1]) - 128) << 8;
                }
            } else { // 16-bit
                if (fmt->channels == 1) {
                    left = right = in16[in_idx];
                } else {
                    left = in16[in_idx * 2 + 0];
                    right = in16[in_idx * 2 + 1];
                }
            }

            out_buf[j * 2 + 0] = left;
            out_buf[j * 2 + 1] = right;
        }

        bool ok = ac97::play_pcm(out_buf, total_out_frames, 48000, blocking);
        kfree(out_buf);
        return ok;
    } else {
        // Speaker notification for non-AC97
        speaker::beep(1000, 150);
        return true;
    }
}

bool play_wav_file(const char* filename, bool blocking) {
    if (!filename || *filename == '\0') return false;

    size_t file_size = 0;
    const void* data = ramfs::read_bytes(filename, &file_size);
    if (!data || file_size == 0) {
        return false;
    }

    return play_wav(data, file_size, blocking);
}

void play_gui_cue_minimize() {
    if (!s_gui_cues_enabled || s_muted) return;

    if (s_driver == DriverType::AC97) {
        // Subtle descending whoosh (880Hz for 25ms, then 587Hz for 35ms)
        constexpr size_t f1 = (48000 * 25) / 1000;
        constexpr size_t f2 = (48000 * 35) / 1000;
        constexpr size_t total = f1 + f2;

        uint32_t p = 0;
        uint32_t s1 = (880ULL * 65536) / 48000;
        uint32_t s2 = (587ULL * 65536) / 48000;

        for (size_t i = 0; i < f1; ++i) {
            int32_t env = (f1 - i) * 18000 / f1;
            int16_t sample = static_cast<int16_t>((SINE_LUT[(p >> 8) & 0xFF] * env) / 32768);
            s_synth_buf[i * 2 + 0] = sample;
            s_synth_buf[i * 2 + 1] = sample;
            p += s1;
        }

        for (size_t i = 0; i < f2; ++i) {
            int32_t env = (f2 - i) * 16000 / f2;
            int16_t sample = static_cast<int16_t>((SINE_LUT[(p >> 8) & 0xFF] * env) / 32768);
            s_synth_buf[(f1 + i) * 2 + 0] = sample;
            s_synth_buf[(f1 + i) * 2 + 1] = sample;
            p += s2;
        }

        ac97::play_pcm(s_synth_buf, total, 48000, false);
    }
}

void play_gui_cue_maximize() {
    if (!s_gui_cues_enabled || s_muted) return;

    if (s_driver == DriverType::AC97) {
        // Subtle ascending whoosh (587Hz for 25ms, then 880Hz for 35ms)
        constexpr size_t f1 = (48000 * 25) / 1000;
        constexpr size_t f2 = (48000 * 35) / 1000;
        constexpr size_t total = f1 + f2;

        uint32_t p = 0;
        uint32_t s1 = (587ULL * 65536) / 48000;
        uint32_t s2 = (880ULL * 65536) / 48000;

        for (size_t i = 0; i < f1; ++i) {
            int32_t env = (f1 - i) * 18000 / f1;
            int16_t sample = static_cast<int16_t>((SINE_LUT[(p >> 8) & 0xFF] * env) / 32768);
            s_synth_buf[i * 2 + 0] = sample;
            s_synth_buf[i * 2 + 1] = sample;
            p += s1;
        }

        for (size_t i = 0; i < f2; ++i) {
            int32_t env = (f2 - i) * 16000 / f2;
            int16_t sample = static_cast<int16_t>((SINE_LUT[(p >> 8) & 0xFF] * env) / 32768);
            s_synth_buf[(f1 + i) * 2 + 0] = sample;
            s_synth_buf[(f1 + i) * 2 + 1] = sample;
            p += s2;
        }

        ac97::play_pcm(s_synth_buf, total, 48000, false);
    }
}

void play_gui_cue_command() {
    if (!s_gui_cues_enabled || s_muted) return;

    if (s_driver == DriverType::AC97) {
        // Crisp terminal execution click (1100Hz, 16ms)
        constexpr size_t total = (48000 * 16) / 1000;
        uint32_t p = 0;
        uint32_t s = (1100ULL * 65536) / 48000;

        for (size_t i = 0; i < total; ++i) {
            int32_t env = (total - i) * 14000 / total;
            int16_t sample = static_cast<int16_t>((SINE_LUT[(p >> 8) & 0xFF] * env) / 32768);
            s_synth_buf[i * 2 + 0] = sample;
            s_synth_buf[i * 2 + 1] = sample;
            p += s;
        }

        ac97::play_pcm(s_synth_buf, total, 48000, false);
    }
}

void set_gui_cues_enabled(bool enabled) {
    s_gui_cues_enabled = enabled;
}

bool are_gui_cues_enabled() {
    return s_gui_cues_enabled;
}

} // namespace sound
