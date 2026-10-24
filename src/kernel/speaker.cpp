#include "speaker.hpp"
#include "pit.hpp"
#include "../arch/x86_64/io.hpp"

#define PIT_BASE_FREQ 1193180

namespace speaker {

void play(uint32_t freq_hz) {
    if (freq_hz == 0) return;
    uint32_t div = PIT_BASE_FREQ / freq_hz;
    if (div > 65535) div = 65535;

    // Set PIT channel 2 to Mode 3 (Square Wave Generator)
    arch::outb(0x43, 0xB6);
    arch::outb(0x42, static_cast<uint8_t>(div & 0xFF));
    arch::outb(0x42, static_cast<uint8_t>((div >> 8) & 0xFF));

    // Enable speaker gates (bits 0 and 1 of port 0x61)
    uint8_t tmp = arch::inb(0x61);
    if ((tmp & 3) != 3) {
        arch::outb(0x61, tmp | 3);
    }
}

void stop() {
    uint8_t tmp = arch::inb(0x61) & 0xFC;
    arch::outb(0x61, tmp);
}

void beep(uint32_t freq_hz, uint32_t duration_ms) {
    play(freq_hz);
    pit::sleep(duration_ms);
    stop();
}

void play_tune() {
    // A classic 8-bit victory melody (frequencies in Hz)
    uint32_t notes[] = { 523, 659, 784, 1046, 784, 1046 };
    uint32_t durations[] = { 100, 100, 100, 250, 100, 400 };

    for (int i = 0; i < 6; ++i) {
        beep(notes[i], durations[i]);
        pit::sleep(30);
    }
}

} // namespace speaker
