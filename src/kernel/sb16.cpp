#include "sb16.hpp"
#include "../arch/x86_64/io.hpp"

#define SB16_DEFAULT_BASE    0x220
#define SB16_MIXER_ADDR      0x224
#define SB16_MIXER_DATA      0x225
#define SB16_DSP_RESET       0x226
#define SB16_DSP_READ        0x22A
#define SB16_DSP_WRITE       0x22C
#define SB16_DSP_STATUS      0x22E

namespace sb16 {

static bool s_available = false;
static uint16_t s_base = SB16_DEFAULT_BASE;
static uint8_t s_ver_major = 0;
static uint8_t s_ver_minor = 0;
static uint8_t s_master_volume = 80;
static bool s_muted = false;

static bool dsp_write(uint8_t val) {
    for (int i = 0; i < 10000; ++i) {
        if ((arch::inb(s_base + 0x0C) & 0x80) == 0) { // Bit 7 clear = ready
            arch::outb(s_base + 0x0C, val);
            return true;
        }
        arch::io_wait();
    }
    return false;
}

static uint8_t dsp_read() {
    for (int i = 0; i < 10000; ++i) {
        if ((arch::inb(s_base + 0x0E) & 0x80) != 0) { // Bit 7 set = data ready
            return arch::inb(s_base + 0x0A);
        }
        arch::io_wait();
    }
    return 0;
}

static void mixer_write(uint8_t reg, uint8_t val) {
    arch::outb(s_base + 0x04, reg);
    arch::outb(s_base + 0x05, val);
}

[[maybe_unused]] static uint8_t mixer_read(uint8_t reg) {
    arch::outb(s_base + 0x04, reg);
    return arch::inb(s_base + 0x05);
}

static void update_mixer_volume() {
    if (!s_available) return;

    if (s_muted) {
        mixer_write(0x22, 0x00);
        mixer_write(0x30, 0x00);
        mixer_write(0x31, 0x00);
    } else {
        // SB Pro 4-bit volume (0-15)
        uint8_t v4 = static_cast<uint8_t>((s_master_volume * 15) / 100);
        mixer_write(0x22, (v4 << 4) | (v4 & 0x0F));

        // SB16 5-bit volume (0-31, shifted to bits 7..3)
        uint8_t v5 = static_cast<uint8_t>((s_master_volume * 31) / 100);
        mixer_write(0x30, v5 << 3);
        mixer_write(0x31, v5 << 3);
    }
}

bool init() {
    // 1. Reset DSP
    arch::outb(s_base + 0x06, 1);
    for (int i = 0; i < 50; ++i) arch::io_wait();
    arch::outb(s_base + 0x06, 0);

    // 2. Poll for ready byte (0xAA)
    bool ready = false;
    for (int i = 0; i < 10000; ++i) {
        if ((arch::inb(s_base + 0x0E) & 0x80) != 0) {
            if (arch::inb(s_base + 0x0A) == 0xAA) {
                ready = true;
                break;
            }
        }
        arch::io_wait();
    }

    if (!ready) {
        s_available = false;
        return false;
    }

    // 3. Get DSP Version
    if (!dsp_write(0xE1)) {
        s_available = false;
        return false;
    }

    s_ver_major = dsp_read();
    s_ver_minor = dsp_read();

    s_available = (s_ver_major >= 1);
    s_master_volume = 80;
    s_muted = false;

    if (s_available) {
        update_mixer_volume();
    }

    return s_available;
}

bool is_available() {
    return s_available;
}

uint16_t get_base_port() {
    return s_base;
}

uint8_t get_version_major() {
    return s_ver_major;
}

uint8_t get_version_minor() {
    return s_ver_minor;
}

void set_master_volume(uint8_t volume_percent) {
    if (volume_percent > 100) volume_percent = 100;
    s_master_volume = volume_percent;
    update_mixer_volume();
}

uint8_t get_master_volume() {
    return s_master_volume;
}

void set_mute(bool mute) {
    s_muted = mute;
    update_mixer_volume();
}

bool is_muted() {
    return s_muted;
}

} // namespace sb16
