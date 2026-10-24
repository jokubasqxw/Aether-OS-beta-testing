#include "pit.hpp"
#include "idt.hpp"
#include "pic.hpp"
#include "../arch/x86_64/io.hpp"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND_REG   0x43
#define PIT_BASE_FREQ     1193182

namespace pit {

static volatile uint64_t timer_ticks = 0;
static uint32_t current_freq = 100;

static void pit_irq_handler(idt::Registers*) {
    timer_ticks = timer_ticks + 1;
}

void init(uint32_t frequency_hz) {
    current_freq = frequency_hz;
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;

    // Channel 0, lobyte/hibyte, Mode 3 (Square Wave Generator), Binary 16-bit
    arch::outb(PIT_COMMAND_REG, 0x36);
    arch::outb(PIT_CHANNEL0_DATA, static_cast<uint8_t>(divisor & 0xFF));
    arch::outb(PIT_CHANNEL0_DATA, static_cast<uint8_t>((divisor >> 8) & 0xFF));

    // Register IRQ0 (vector 32)
    idt::register_handler(32, pit_irq_handler);

    // Unmask IRQ0 on PIC
    pic::unmask(0);
}

uint64_t get_ticks() {
    return timer_ticks;
}

uint64_t uptime_ms() {
    return (timer_ticks * 1000) / current_freq;
}

uint64_t uptime_seconds() {
    return timer_ticks / current_freq;
}

void sleep(uint32_t ms) {
    uint64_t start_ticks = timer_ticks;
    uint64_t wait_ticks = (static_cast<uint64_t>(ms) * current_freq) / 1000;
    if (wait_ticks == 0 && ms > 0) wait_ticks = 1;

    while (timer_ticks - start_ticks < wait_ticks) {
        arch::hlt();
    }
}

} // namespace pit
