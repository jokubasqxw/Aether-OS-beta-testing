#include "idt.hpp"
#include "vga.hpp"
#include "serial.hpp"
#include "printk.hpp"
#include "kstring.hpp"
#include "pic.hpp"
#include "../arch/x86_64/io.hpp"

// External ISR entry points from isr.asm
extern "C" {
    void isr0(); void isr1(); void isr2(); void isr3();
    void isr4(); void isr5(); void isr6(); void isr7();
    void isr8(); void isr9(); void isr10(); void isr11();
    void isr12(); void isr13(); void isr14(); void isr15();
    void isr16(); void isr17(); void isr18(); void isr19();
    void isr20(); void isr21(); void isr22(); void isr23();
    void isr24(); void isr25(); void isr26(); void isr27();
    void isr28(); void isr29(); void isr30(); void isr31();
    void isr32(); void isr33(); void isr34(); void isr35();
    void isr36(); void isr37(); void isr38(); void isr39();
    void isr40(); void isr41(); void isr42(); void isr43();
    void isr44(); void isr45(); void isr46(); void isr47();
}

namespace idt {

struct IDTEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct IDTDescriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static IDTEntry idt_entries[256];
static IRQHandler irq_handlers[256];

static const char* const EXCEPTION_MESSAGES[] = {
    "Divide-by-zero Error (#DE)",
    "Debug (#DB)",
    "Non-maskable Interrupt (#NMI)",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved",
    "x87 Floating-Point Exception (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point Exception (#XM)",
    "Virtualization Exception (#VE)",
    "Control Protection Exception (#CP)",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Security Exception (#SX)",
    "Reserved"
};

static void set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].offset_low  = static_cast<uint16_t>(base & 0xFFFF);
    idt_entries[num].selector    = sel;
    idt_entries[num].ist         = 0;
    idt_entries[num].type_attr   = flags;
    idt_entries[num].offset_mid  = static_cast<uint16_t>((base >> 16) & 0xFFFF);
    idt_entries[num].offset_high = static_cast<uint32_t>((base >> 32) & 0xFFFFFFFF);
    idt_entries[num].reserved    = 0;
}

void register_handler(uint8_t vector, IRQHandler handler) {
    irq_handlers[vector] = handler;
}

void unregister_handler(uint8_t vector) {
    irq_handlers[vector] = nullptr;
}

void init() {
    memset(idt_entries, 0, sizeof(idt_entries));
    memset(irq_handlers, 0, sizeof(irq_handlers));

    uint64_t isr_ptrs[48] = {
        reinterpret_cast<uint64_t>(isr0),  reinterpret_cast<uint64_t>(isr1),
        reinterpret_cast<uint64_t>(isr2),  reinterpret_cast<uint64_t>(isr3),
        reinterpret_cast<uint64_t>(isr4),  reinterpret_cast<uint64_t>(isr5),
        reinterpret_cast<uint64_t>(isr6),  reinterpret_cast<uint64_t>(isr7),
        reinterpret_cast<uint64_t>(isr8),  reinterpret_cast<uint64_t>(isr9),
        reinterpret_cast<uint64_t>(isr10), reinterpret_cast<uint64_t>(isr11),
        reinterpret_cast<uint64_t>(isr12), reinterpret_cast<uint64_t>(isr13),
        reinterpret_cast<uint64_t>(isr14), reinterpret_cast<uint64_t>(isr15),
        reinterpret_cast<uint64_t>(isr16), reinterpret_cast<uint64_t>(isr17),
        reinterpret_cast<uint64_t>(isr18), reinterpret_cast<uint64_t>(isr19),
        reinterpret_cast<uint64_t>(isr20), reinterpret_cast<uint64_t>(isr21),
        reinterpret_cast<uint64_t>(isr22), reinterpret_cast<uint64_t>(isr23),
        reinterpret_cast<uint64_t>(isr24), reinterpret_cast<uint64_t>(isr25),
        reinterpret_cast<uint64_t>(isr26), reinterpret_cast<uint64_t>(isr27),
        reinterpret_cast<uint64_t>(isr28), reinterpret_cast<uint64_t>(isr29),
        reinterpret_cast<uint64_t>(isr30), reinterpret_cast<uint64_t>(isr31),
        reinterpret_cast<uint64_t>(isr32), reinterpret_cast<uint64_t>(isr33),
        reinterpret_cast<uint64_t>(isr34), reinterpret_cast<uint64_t>(isr35),
        reinterpret_cast<uint64_t>(isr36), reinterpret_cast<uint64_t>(isr37),
        reinterpret_cast<uint64_t>(isr38), reinterpret_cast<uint64_t>(isr39),
        reinterpret_cast<uint64_t>(isr40), reinterpret_cast<uint64_t>(isr41),
        reinterpret_cast<uint64_t>(isr42), reinterpret_cast<uint64_t>(isr43),
        reinterpret_cast<uint64_t>(isr44), reinterpret_cast<uint64_t>(isr45),
        reinterpret_cast<uint64_t>(isr46), reinterpret_cast<uint64_t>(isr47)
    };

    for (int i = 0; i < 48; ++i) {
        set_gate(static_cast<uint8_t>(i), isr_ptrs[i], 0x08, 0x8E);
    }

    IDTDescriptor idtr;
    idtr.limit = sizeof(idt_entries) - 1;
    idtr.base = reinterpret_cast<uint64_t>(&idt_entries);

    asm volatile("lidt %0" : : "m"(idtr));
}

static void dump_registers_and_panic(Registers* regs) {
    arch::cli();

    vga::set_color(vga::Color::White, vga::Color::Red);
    vga::clear();

    const char* ex_name = "Unknown Exception";
    if (regs->int_no < 32) {
        ex_name = EXCEPTION_MESSAGES[regs->int_no];
    }

    uint64_t cr2 = 0;
    if (regs->int_no == 14) {
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
    }

    printk("================================================================================\n");
    printk("                         *** KERNEL PANIC: CPU EXCEPTION ***                   \n");
    printk("================================================================================\n\n");
    printk(" Vector: %u (%s)\n", regs->int_no, ex_name);
    printk(" Error Code: 0x%x\n", regs->err_code);
    printk(" RIP: %p   CS: 0x%x   RFLAGS: 0x%x\n", regs->rip, regs->cs, regs->rflags);
    if (regs->int_no == 14) {
        printk(" Faulting Address (CR2): %p\n", cr2);
    }
    printk("\n General Purpose Registers:\n");
    printk("   RAX: %p   RBX: %p   RCX: %p\n", regs->rax, regs->rbx, regs->rcx);
    printk("   RDX: %p   RSI: %p   RDI: %p\n", regs->rdx, regs->rsi, regs->rdi);
    printk("   RBP: %p   RSP: %p\n", regs->rbp, regs->rsp);
    printk("   R8 : %p   R9 : %p   R10: %p\n", regs->r8, regs->r9, regs->r10);
    printk("   R11: %p   R12: %p   R13: %p\n", regs->r11, regs->r12, regs->r13);
    printk("   R14: %p   R15: %p\n\n", regs->r14, regs->r15);
    printk(" System halted to prevent damage. Please reset machine.\n");
    printk("================================================================================\n");

    while (true) {
        arch::hlt();
    }
}

} // namespace idt

extern "C" void isr_dispatch(idt::Registers* regs) {
    if (regs->int_no < 32) {
        idt::dump_registers_and_panic(regs);
    } else {
        uint8_t irq = static_cast<uint8_t>(regs->int_no - 32);
        if (idt::irq_handlers[regs->int_no]) {
            idt::irq_handlers[regs->int_no](regs);
        }
        pic::send_eoi(irq);
    }
}
