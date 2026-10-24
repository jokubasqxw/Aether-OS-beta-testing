#ifndef KERNEL_AC97_HPP
#define KERNEL_AC97_HPP

#include "types.hpp"

namespace ac97 {

// Intel AC97 PCI Audio Controller Buffer Descriptor (BDL entry)
struct alignas(8) BdlEntry {
    uint32_t addr;    // Physical memory address of audio buffer (4-byte aligned)
    uint16_t samples; // Length in samples (number of 16-bit words)
    uint16_t flags;   // Control flags: bit 15 = IOC (0x8000), bit 14 = BUP (0x4000)
};

// Initialization and hardware discovery
bool init();
bool is_available();

// PCI and I/O information
uint8_t get_pci_bus();
uint8_t get_pci_slot();
uint8_t get_pci_func();
uint16_t get_nambar();
uint16_t get_nabmbar();
uint32_t get_sample_rate();

// Volume and Mute control
void set_master_volume(uint8_t volume_percent); // 0-100%
uint8_t get_master_volume();
void set_pcm_volume(uint8_t volume_percent);    // 0-100%
uint8_t get_pcm_volume();
void set_mute(bool mute);
bool is_muted();

// Audio playback (16-bit signed stereo PCM)
bool play_pcm(const int16_t* pcm_stereo, size_t num_frames, uint32_t sample_rate = 48000, bool blocking = true);
void stop();
bool is_playing();
void wait_completion(uint32_t max_wait_ms = 10000);

} // namespace ac97

#endif // KERNEL_AC97_HPP
