#ifndef ARCH_X86_64_IO_HPP
#define ARCH_X86_64_IO_HPP

#include "../../kernel/types.hpp"

namespace arch {

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait() {
    outb(0x80, 0);
}

static inline void cli() {
    asm volatile("cli");
}

static inline void sti() {
    asm volatile("sti");
}

static inline void hlt() {
    asm volatile("hlt");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = static_cast<uint32_t>(val);
    uint32_t high = static_cast<uint32_t>(val >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline void invlpg(uintptr_t addr) {
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline void wbinvd() {
    asm volatile("wbinvd" : : : "memory");
}

static inline uint64_t read_cr3() {
    uint64_t val;
    asm volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val) {
    asm volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}

} // namespace arch

#endif // ARCH_X86_64_IO_HPP
