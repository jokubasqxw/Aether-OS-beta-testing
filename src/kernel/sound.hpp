#ifndef KERNEL_SOUND_HPP
#define KERNEL_SOUND_HPP

#include "types.hpp"

namespace sound {

enum class DriverType {
    None,
    PCSpeaker,
    SB16,
    AC97
};

enum class Effect {
    Beep,
    Chime,
    Laser,
    Coin,
    Fanfare
};

enum class Waveform {
    Sine,
    Square,
    Triangle,
    Sawtooth
};

struct Note {
    uint32_t freq_hz;
    uint32_t duration_ms;
};

// Subsystem Lifecycle & Hardware Detection
void init();
DriverType get_driver_type();
const char* get_driver_name();
bool is_hardware_available();

// Master Volume and Mute Control
void set_volume(uint8_t volume_percent); // 0-100%
uint8_t get_volume();
void set_mute(bool mute);
bool is_muted();

// Playback Status & Control
bool is_playing();
void stop();
void wait_idle();

// Synthesizer Engine
void play_tone(uint32_t freq_hz, uint32_t duration_ms, Waveform wave = Waveform::Sine, bool blocking = true);
void play_chord(const uint32_t* freqs, size_t count, uint32_t duration_ms, bool blocking = true);
void play_melody(const Note* notes, size_t count, bool blocking = true);
void play_effect(Effect effect, bool blocking = true);

// WAV Audio File Parser & Player
bool play_wav(const void* data, size_t size, bool blocking = true);
bool play_wav_file(const char* filename, bool blocking = true);

// GUI Desktop Subtle Audio Cues
void play_gui_cue_minimize();
void play_gui_cue_maximize();
void play_gui_cue_command();
void set_gui_cues_enabled(bool enabled);
bool are_gui_cues_enabled();

} // namespace sound

#endif // KERNEL_SOUND_HPP
