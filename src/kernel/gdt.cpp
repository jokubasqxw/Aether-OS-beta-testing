#include "gdt.hpp"
#include "kstring.hpp"

namespace gdt {

struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct TSSDescriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t flags1;
    uint8_t flags2;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct GDTDescriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct GDTTable {
    uint64_t null_desc;
    uint64_t kernel_code; // 0x08
    uint64_t kernel_data; // 0x10
    uint64_t user_data;   // 0x18
    uint64_t user_code;   // 0x20
    TSSDescriptor tss;    // 0x28 (16 bytes)
} __attribute__((packed));

static GDTTable gdt_table;
static TSS kernel_tss;

static void set_tss_desc(TSSDescriptor* desc, uint64_t base, uint32_t limit) {
    desc->limit_low = static_cast<uint16_t>(limit & 0xFFFF);
    desc->base_low = static_cast<uint16_t>(base & 0xFFFF);
    desc->base_mid = static_cast<uint8_t>((base >> 16) & 0xFF);
    desc->flags1 = 0x89; // Present, 64-bit TSS (Available)
    desc->flags2 = static_cast<uint8_t>((limit >> 16) & 0x0F);
    desc->base_high = static_cast<uint8_t>((base >> 24) & 0xFF);
    desc->base_upper = static_cast<uint32_t>((base >> 32) & 0xFFFFFFFF);
    desc->reserved = 0;
}

void init() {
    memset(&kernel_tss, 0, sizeof(TSS));
    kernel_tss.iomap_base = sizeof(TSS);

    // 0x00: Null
    gdt_table.null_desc = 0;
    // 0x08: Kernel Code (Present, Ring 0, Code, Executable, Readable, Long Mode)
    gdt_table.kernel_code = 0x00209A0000000000ULL;
    // 0x10: Kernel Data (Present, Ring 0, Data, Writable)
    gdt_table.kernel_data = 0x0000920000000000ULL;
    // 0x18: User Data (Present, Ring 3, Data, Writable)
    gdt_table.user_data   = 0x0000F20000000000ULL;
    // 0x20: User Code (Present, Ring 3, Code, Executable, Readable, Long Mode)
    gdt_table.user_code   = 0x0020FA0000000000ULL;

    set_tss_desc(&gdt_table.tss, reinterpret_cast<uint64_t>(&kernel_tss), sizeof(TSS) - 1);

    GDTDescriptor gdtr;
    gdtr.limit = sizeof(GDTTable) - 1;
    gdtr.base = reinterpret_cast<uint64_t>(&gdt_table);

    asm volatile(
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "mov $0x28, %%ax\n\t"
        "ltr %%ax\n\t"
        : : "m"(gdtr) : "rax", "memory"
    );
}

void set_kernel_stack(uint64_t stack_top) {
    kernel_tss.rsp0 = stack_top;
}

} // namespace gdt
