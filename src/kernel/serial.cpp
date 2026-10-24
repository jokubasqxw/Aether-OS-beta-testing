#include "serial.hpp"
#include "../arch/x86_64/io.hpp"
#include "kstring.hpp"

#define COM1_PORT 0x3F8

namespace serial {

void init() {
    arch::outb(COM1_PORT + 1, 0x00); // Disable all interrupts
    arch::outb(COM1_PORT + 3, 0x80); // Enable DLAB (set baud rate divisor)
    arch::outb(COM1_PORT + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    arch::outb(COM1_PORT + 1, 0x00); //                  (hi byte)
    arch::outb(COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    arch::outb(COM1_PORT + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    arch::outb(COM1_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

static inline bool is_transmit_empty() {
    return (arch::inb(COM1_PORT + 5) & 0x20) != 0;
}

void write_char(char c) {
    while (!is_transmit_empty()) {}
    arch::outb(COM1_PORT, c);
}

void write(const char* str) {
    while (*str) {
        if (*str == '\n') {
            write_char('\r');
        }
        write_char(*str++);
    }
}

void write_hex(uint64_t val) {
    char buf[32];
    write("0x");
    kstd::utoa(val, buf, 16);
    write(buf);
}

void write_dec(uint64_t val) {
    char buf[32];
    kstd::utoa(val, buf, 10);
    write(buf);
}

bool has_char() {
    return (arch::inb(COM1_PORT + 5) & 1) != 0;
}

char read_char() {
    while (!has_char()) {}
    return arch::inb(COM1_PORT);
}

} // namespace serial
