#ifndef KERNEL_IDT_HPP
#define KERNEL_IDT_HPP

#include "types.hpp"

namespace idt {

struct Registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*IRQHandler)(Registers* regs);

void init();
void register_handler(uint8_t vector, IRQHandler handler);
void unregister_handler(uint8_t vector);

} // namespace idt

extern "C" void isr_dispatch(idt::Registers* regs);

#endif // KERNEL_IDT_HPP
