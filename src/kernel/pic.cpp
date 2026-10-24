#include "pic.hpp"
#include "../arch/x86_64/io.hpp"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

#define PIC_EOI      0x20

namespace pic {

void init() {
    // Start initialization sequence in cascade mode
    arch::outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    arch::io_wait();
    arch::outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    arch::io_wait();

    // Master PIC vector offset: 32 (0x20)
    arch::outb(PIC1_DATA, 0x20);
    arch::io_wait();
    // Slave PIC vector offset: 40 (0x28)
    arch::outb(PIC2_DATA, 0x28);
    arch::io_wait();

    // Tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
    arch::outb(PIC1_DATA, 4);
    arch::io_wait();
    // Tell Slave PIC its cascade identity (2)
    arch::outb(PIC2_DATA, 2);
    arch::io_wait();

    // Set 8086 mode
    arch::outb(PIC1_DATA, ICW4_8086);
    arch::io_wait();
    arch::outb(PIC2_DATA, ICW4_8086);
    arch::io_wait();

    // Mask everything by default except IRQ2 (cascade)
    arch::outb(PIC1_DATA, 0xFB); // 1111 1011 (IRQ2 unmasked)
    arch::outb(PIC2_DATA, 0xFF);
}

void send_eoi(uint8_t irq) {
    if (irq >= 8) {
        arch::outb(PIC2_COMMAND, PIC_EOI);
    }
    arch::outb(PIC1_COMMAND, PIC_EOI);
}

void mask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t value = arch::inb(port) | (1 << irq);
    arch::outb(port, value);
}

void unmask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t value = arch::inb(port) & ~(1 << irq);
    arch::outb(port, value);
}

void disable() {
    arch::outb(PIC1_DATA, 0xFF);
    arch::outb(PIC2_DATA, 0xFF);
}

} // namespace pic
