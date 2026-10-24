#include "ac97.hpp"
#include "pci.hpp"
#include "printk.hpp"
#include "pit.hpp"
#include "heap.hpp"
#include "kstring.hpp"
#include "../arch/x86_64/io.hpp"

// NAMBAR Mixer Register Offsets
#define AC97_MIX_RESET          0x00
#define AC97_MIX_MASTER_VOL     0x02
#define AC97_MIX_PC_BEEP_VOL    0x0A
#define AC97_MIX_PCM_VOL        0x18
#define AC97_MIX_POWERDOWN      0x26
#define AC97_MIX_EXT_AUDIO_ID   0x28
#define AC97_MIX_EXT_AUDIO_CTRL 0x2A
#define AC97_MIX_PCM_RATE       0x2C

// NABMBAR PCM Out Channel Register Offsets (Base + 0x10)
#define AC97_PO_BDBAR           0x10
#define AC97_PO_CIV             0x14
#define AC97_PO_LVI             0x15
#define AC97_PO_SR              0x16
#define AC97_PO_PICB            0x18
#define AC97_PO_PIV             0x1A
#define AC97_PO_CR              0x1B

// Buffer Descriptor List constants
#define AC97_MAX_BDL_ENTRIES    32
#define AC97_WORDS_PER_BDL      16384 // 16K samples = 32KB = 8192 stereo frames

namespace ac97 {

static bool s_available = false;
static uint8_t s_bus = 0;
static uint8_t s_slot = 0;
static uint8_t s_func = 0;
static uint16_t s_nambar = 0;
static uint16_t s_nabmbar = 0;
static uint32_t s_sample_rate = 48000;
static uint8_t s_master_volume = 80;
static uint8_t s_pcm_volume = 80;
static bool s_muted = false;

// Aligned BDL structure and playback buffer
alignas(8) static BdlEntry s_bdl[AC97_MAX_BDL_ENTRIES];
alignas(4096) static int16_t s_dma_buf[32768 * 2]; // 128KB static DMA buffer (32768 stereo frames)

static void update_hardware_volume() {
    if (!s_available) return;

    // In AC97, attenuation is 5 bits: 0 = 0dB (max), 31 (0x1F) = -46.5dB (min)
    uint8_t master_att = s_muted ? 0x1F : static_cast<uint8_t>((100 - s_master_volume) * 31 / 100);
    uint8_t pcm_att = s_muted ? 0x1F : static_cast<uint8_t>((100 - s_pcm_volume) * 31 / 100);

    uint16_t master_val = ((master_att & 0x1F) << 8) | (master_att & 0x1F);
    uint16_t pcm_val = ((pcm_att & 0x1F) << 8) | (pcm_att & 0x1F);

    if (s_muted) {
        master_val |= 0x8000;
        pcm_val |= 0x8000;
    }

    arch::outw(s_nambar + AC97_MIX_MASTER_VOL, master_val);
    arch::outw(s_nambar + AC97_MIX_PCM_VOL, pcm_val);
}

bool init() {
    pci::Device dev;
    bool found = false;

    // 1. Search for Intel 82801AA AC97 controller (0x8086:0x2415)
    if (pci::find_device(0x8086, 0x2415, &dev)) {
        found = true;
    } else if (pci::find_device_by_class(0x04, 0x01, &dev)) {
        // Fallback: search by PCI Class 0x04 (Multimedia), Subclass 0x01 (Audio Controller)
        found = true;
    }

    if (!found) {
        s_available = false;
        return false;
    }

    s_bus = dev.bus;
    s_slot = dev.slot;
    s_func = dev.func;

    // 2. Enable PCI Bus Master and I/O Space
    pci::enable_bus_mastering(dev);

    // 3. Read BAR0 (NAMBAR) and BAR1 (NABMBAR)
    uint32_t bar0 = pci::get_bar(dev, 0);
    uint32_t bar1 = pci::get_bar(dev, 1);

    s_nambar = static_cast<uint16_t>(bar0 & ~0x3);
    s_nabmbar = static_cast<uint16_t>(bar1 & ~0x3);

    if (s_nambar == 0 || s_nabmbar == 0) {
        s_available = false;
        return false;
    }

    // 4. Cold reset AC97 Codec (write any value to NAMBAR + 0x00)
    arch::outw(s_nambar + AC97_MIX_RESET, 0x0000);
    for (int i = 0; i < 100; ++i) arch::io_wait();

    // 5. Reset Bus Master PCM Out Channel
    arch::outb(s_nabmbar + AC97_PO_CR, 0x02); // Set RR (Reset Registers)
    for (int i = 0; i < 1000; ++i) {
        if ((arch::inb(s_nabmbar + AC97_PO_CR) & 0x02) == 0) break;
        arch::io_wait();
    }

    // Clear status register bits
    arch::outw(s_nabmbar + AC97_PO_SR, 0x001C);

    // 6. Check Variable Rate Audio (VRA) support
    uint16_t ext_id = arch::inw(s_nambar + AC97_MIX_EXT_AUDIO_ID);
    if (ext_id & 0x0001) {
        // Enable VRA (bit 0 in Extended Audio Status and Control)
        uint16_t ext_ctrl = arch::inw(s_nambar + AC97_MIX_EXT_AUDIO_CTRL);
        arch::outw(s_nambar + AC97_MIX_EXT_AUDIO_CTRL, ext_ctrl | 0x0001);
        arch::io_wait();

        // Configure default 48000 Hz Front DAC rate
        arch::outw(s_nambar + AC97_MIX_PCM_RATE, 48000);
        arch::io_wait();
        s_sample_rate = arch::inw(s_nambar + AC97_MIX_PCM_RATE);
    } else {
        s_sample_rate = 48000;
    }

    s_available = true;
    s_master_volume = 80;
    s_pcm_volume = 80;
    s_muted = false;
    update_hardware_volume();

    return true;
}

bool is_available() {
    return s_available;
}

uint8_t get_pci_bus() { return s_bus; }
uint8_t get_pci_slot() { return s_slot; }
uint8_t get_pci_func() { return s_func; }
uint16_t get_nambar() { return s_nambar; }
uint16_t get_nabmbar() { return s_nabmbar; }
uint32_t get_sample_rate() { return s_sample_rate; }

void set_master_volume(uint8_t volume_percent) {
    if (volume_percent > 100) volume_percent = 100;
    s_master_volume = volume_percent;
    update_hardware_volume();
}

uint8_t get_master_volume() {
    return s_master_volume;
}

void set_pcm_volume(uint8_t volume_percent) {
    if (volume_percent > 100) volume_percent = 100;
    s_pcm_volume = volume_percent;
    update_hardware_volume();
}

uint8_t get_pcm_volume() {
    return s_pcm_volume;
}

void set_mute(bool mute) {
    s_muted = mute;
    update_hardware_volume();
}

bool is_muted() {
    return s_muted;
}

bool is_playing() {
    if (!s_available) return false;
    uint8_t cr = arch::inb(s_nabmbar + AC97_PO_CR);
    if ((cr & 0x01) == 0) return false; // Not running

    uint16_t sr = arch::inw(s_nabmbar + AC97_PO_SR);
    // Bit 0: DCH (DMA Controller Halted). If 1, DMA has halted.
    if ((sr & 0x0001) != 0) {
        return false;
    }
    return true;
}

void stop() {
    if (!s_available) return;
    // Clear Run/Pause bit
    arch::outb(s_nabmbar + AC97_PO_CR, 0x00);
    // Clear status bits
    arch::outw(s_nabmbar + AC97_PO_SR, 0x001C);
}

void wait_completion(uint32_t max_wait_ms) {
    if (!s_available) return;
    uint32_t start = pit::uptime_ms();
    while (is_playing()) {
        pit::sleep(1);
        if (pit::uptime_ms() - start >= max_wait_ms) {
            stop();
            break;
        }
    }
    stop();
}

bool play_pcm(const int16_t* pcm_stereo, size_t num_frames, uint32_t sample_rate, bool blocking) {
    if (!s_available || !pcm_stereo || num_frames == 0) return false;

    // If already playing, stop current playback
    if (is_playing()) {
        stop();
    }

    // Adjust sample rate if VRA is active and rate differs
    if (sample_rate != s_sample_rate && (arch::inw(s_nambar + AC97_MIX_EXT_AUDIO_ID) & 1)) {
        arch::outw(s_nambar + AC97_MIX_PCM_RATE, static_cast<uint16_t>(sample_rate));
        s_sample_rate = arch::inw(s_nambar + AC97_MIX_PCM_RATE);
    }

    const int16_t* play_ptr = pcm_stereo;

    // If non-blocking or buffer is smaller than static DMA buffer, safely copy to aligned DMA buffer
    if (!blocking) {
        size_t frames_to_copy = num_frames;
        if (frames_to_copy > 32768) frames_to_copy = 32768;
        kstd::memcpy(s_dma_buf, pcm_stereo, frames_to_copy * 2 * sizeof(int16_t));
        play_ptr = s_dma_buf;
        num_frames = frames_to_copy;
    }

    size_t total_words = num_frames * 2; // 2 words (L+R) per stereo frame
    size_t num_bdl = (total_words + AC97_WORDS_PER_BDL - 1) / AC97_WORDS_PER_BDL;
    if (num_bdl > AC97_MAX_BDL_ENTRIES) {
        num_bdl = AC97_MAX_BDL_ENTRIES;
        total_words = num_bdl * AC97_WORDS_PER_BDL;
        num_frames = total_words / 2;
    }

    for (size_t i = 0; i < num_bdl; ++i) {
        size_t word_offset = i * AC97_WORDS_PER_BDL;
        size_t words = (total_words - word_offset > AC97_WORDS_PER_BDL) ? AC97_WORDS_PER_BDL : (total_words - word_offset);

        uintptr_t phys_addr = reinterpret_cast<uintptr_t>(play_ptr + word_offset);
        s_bdl[i].addr = static_cast<uint32_t>(phys_addr);
        s_bdl[i].samples = static_cast<uint16_t>(words);
        s_bdl[i].flags = (i == num_bdl - 1) ? 0x8000 : 0x0000; // Interrupt on last buffer
    }

    // Reset PCM Out channel registers
    arch::outb(s_nabmbar + AC97_PO_CR, 0x02);
    for (int i = 0; i < 500; ++i) {
        if ((arch::inb(s_nabmbar + AC97_PO_CR) & 0x02) == 0) break;
        arch::io_wait();
    }

    // Clear Status register bits
    arch::outw(s_nabmbar + AC97_PO_SR, 0x001C);

    // Set BDL Physical Base Address
    uint32_t bdl_phys = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_bdl[0]));
    arch::outl(s_nabmbar + AC97_PO_BDBAR, bdl_phys);

    // Set Last Valid Index (LVI)
    arch::outb(s_nabmbar + AC97_PO_LVI, static_cast<uint8_t>(num_bdl - 1));

    // Start DMA Bus Master playback (RP bit = 1)
    arch::outb(s_nabmbar + AC97_PO_CR, 0x01);

    if (blocking) {
        uint32_t duration_ms = static_cast<uint32_t>((num_frames * 1000ULL) / s_sample_rate) + 100;
        wait_completion(duration_ms);
    }

    return true;
}

} // namespace ac97
