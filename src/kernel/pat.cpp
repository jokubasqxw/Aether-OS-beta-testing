#include "pat.hpp"
#include "printk.hpp"
#include "vga.hpp"
#include "pit.hpp"
#include "bga.hpp"
#include "../arch/x86_64/io.hpp"

// Global page directories table exported from boot.asm (maps 4GB identity with 2048 x 2MB huge pages)
extern "C" uint64_t page_directories[2048];

namespace pat {

static bool pat_initialized = false;

static void get_cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    asm volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf)
    );
}

bool is_supported() {
    uint32_t eax, ebx, ecx, edx;
    get_cpuid(1, &eax, &ebx, &ecx, &edx);
    // EDX bit 16 = Page Attribute Table (PAT)
    return (edx & (1 << 16)) != 0;
}

uint64_t get_pat_msr() {
    if (!is_supported()) return 0;
    return arch::rdmsr(IA32_PAT_MSR);
}

uint8_t get_pat_entry(uint8_t index) {
    if (index > 7) return 0xFF;
    uint64_t msr = get_pat_msr();
    return static_cast<uint8_t>((msr >> (index * 8)) & 0xFF);
}

const char* get_mem_type_name(uint8_t type) {
    switch (type) {
        case MEM_UC:  return "UC  (Uncacheable)";
        case MEM_WC:  return "WC  (Write-Combining)";
        case MEM_WT:  return "WT  (Write-Through)";
        case MEM_WP:  return "WP  (Write-Protected)";
        case MEM_WB:  return "WB  (Write-Back)";
        case MEM_UCM: return "UC- (Weakly Uncacheable)";
        default:      return "Unknown";
    }
}

const char* get_mem_type_short_name(uint8_t type) {
    switch (type) {
        case MEM_UC:  return "UC";
        case MEM_WC:  return "WC";
        case MEM_WT:  return "WT";
        case MEM_WP:  return "WP";
        case MEM_WB:  return "WB";
        case MEM_UCM: return "UC-";
        default:      return "UNK";
    }
}

bool init() {
    if (!is_supported()) {
        printk("[PAT] Error: IA32_PAT is not supported on this CPU.\n");
        return false;
    }

    uint64_t before = arch::rdmsr(IA32_PAT_MSR);

    // Write AetherOS optimized IA32_PAT configuration
    arch::wrmsr(IA32_PAT_MSR, PAT_AETHEROS_MSR);

    // Verify
    uint64_t after = arch::rdmsr(IA32_PAT_MSR);
    if (after != PAT_AETHEROS_MSR) {
        printk("[PAT] Error: Written MSR mismatch! (Got 0x%p, expected 0x%p)\n", after, PAT_AETHEROS_MSR);
        return false;
    }

    // Invalidate caches and reload CR3 to flush all non-global TLB entries
    arch::wbinvd();
    uint64_t cr3 = arch::read_cr3();
    arch::write_cr3(cr3);

    pat_initialized = true;
    (void)before;
    return true;
}

void flush_range(uintptr_t phys_addr, size_t size) {
    // 1. Write back and invalidate internal caches (L1/L2/L3)
    arch::wbinvd();

    // 2. Invalidate TLB entries for each 2MB page in range
    uintptr_t start = phys_addr & ~0x1FFFFFULL;
    uintptr_t end = phys_addr + size;
    for (uintptr_t addr = start; addr < end; addr += 2 * 1024 * 1024) {
        arch::invlpg(addr);
    }

    // 3. Reload CR3 to flush entire TLB
    uint64_t cr3 = arch::read_cr3();
    arch::write_cr3(cr3);
}

bool set_range_caching(uintptr_t phys_addr, size_t size, uint8_t pat_index) {
    if (pat_index > 7) return false;

    size_t start_idx = phys_addr / (2 * 1024 * 1024);
    size_t count = (size + (2 * 1024 * 1024 - 1)) / (2 * 1024 * 1024);

    if (start_idx >= 2048) return false;
    if (start_idx + count > 2048) {
        count = 2048 - start_idx;
    }

    // PAT index mapping for 2MB Huge Page:
    // Bit 0 of index -> Bit 3 (PWT)
    // Bit 1 of index -> Bit 4 (PCD)
    // Bit 2 of index -> Bit 12 (PAT)
    uint64_t cache_bits = 0;
    if (pat_index & 1) cache_bits |= PDE_FLAG_PWT;
    if (pat_index & 2) cache_bits |= PDE_FLAG_PCD;
    if (pat_index & 4) cache_bits |= PDE_FLAG_PAT;

    for (size_t i = 0; i < count; ++i) {
        size_t idx = start_idx + i;
        uint64_t entry = page_directories[idx];
        entry &= ~PDE_CACHE_MASK;
        entry |= cache_bits;
        page_directories[idx] = entry;
    }

    flush_range(phys_addr, count * 2 * 1024 * 1024);
    return true;
}

bool set_range_wc(uintptr_t phys_addr, size_t size) {
    // PAT Index 4: PAT=1, PCD=0, PWT=0 -> WC (0x01)
    // In AetherOS IA32_PAT, both Index 4 and Index 1 are configured as WC!
    return set_range_caching(phys_addr, size, 4);
}

bool set_range_wb(uintptr_t phys_addr, size_t size) {
    // PAT Index 0: PAT=0, PCD=0, PWT=0 -> WB (0x06)
    return set_range_caching(phys_addr, size, 0);
}

bool set_range_uc(uintptr_t phys_addr, size_t size) {
    // PAT Index 3: PAT=0, PCD=1, PWT=1 -> UC (0x00)
    return set_range_caching(phys_addr, size, 3);
}

uint64_t get_pde(uintptr_t phys_addr) {
    size_t idx = phys_addr / (2 * 1024 * 1024);
    if (idx >= 2048) return 0;
    return page_directories[idx];
}

uint8_t get_page_pat_index(uintptr_t phys_addr) {
    uint64_t pde = get_pde(phys_addr);
    bool pat = (pde & PDE_FLAG_PAT) != 0;
    bool pcd = (pde & PDE_FLAG_PCD) != 0;
    bool pwt = (pde & PDE_FLAG_PWT) != 0;
    return (pat << 2) | (pcd << 1) | pwt;
}

uint8_t get_page_mem_type(uintptr_t phys_addr) {
    uint8_t idx = get_page_pat_index(phys_addr);
    return get_pat_entry(idx);
}

void print_status() {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("=== x86_64 Page Attribute Table (PAT) & Caching Status ===\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    bool supported = is_supported();
    printk(" CPUID PAT Support  : %s (EDX bit 16)\n", supported ? "[YES] Available" : "[NO] Missing");
    if (!supported) return;

    uint64_t msr = get_pat_msr();
    printk(" IA32_PAT MSR (0x277): %p\n", msr);
    printk(" Active Configuration: %s\n\n",
           (msr == PAT_AETHEROS_MSR) ? "AetherOS Optimized (Dual-WC PAT1 & PAT4)" :
           (msr == PAT_DEFAULT_MSR)  ? "Intel Reset Default" : "Custom Configuration");

    printk(" PAT Table Layout:\n");
    for (uint8_t i = 0; i < 8; ++i) {
        uint8_t type = get_pat_entry(i);
        uint8_t pat_bit = (i >> 2) & 1;
        uint8_t pcd_bit = (i >> 1) & 1;
        uint8_t pwt_bit = i & 1;

        if (type == MEM_WC) {
            vga::set_color(vga::Color::LightGreen, vga::Color::Black);
        } else if (type == MEM_WB) {
            vga::set_color(vga::Color::Yellow, vga::Color::Black);
        } else {
            vga::set_color(vga::Color::White, vga::Color::Black);
        }

        printk("   PAT%u [PAT=%u, PCD=%u, PWT=%u] : %s (0x%x)%s\n",
               i, pat_bit, pcd_bit, pwt_bit, get_mem_type_name(type), type,
               (type == MEM_WC) ? "  <-- WRITE-COMBINING" : "");
    }

    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("\n Framebuffer Caching Mode:\n");

    uintptr_t fb = bga::get_framebuffer_addr();
    if (fb) {
        size_t pd_idx = fb / (2 * 1024 * 1024);
        uint64_t pde = get_pde(fb);
        uint8_t pat_idx = get_page_pat_index(fb);
        uint8_t type = get_page_mem_type(fb);

        printk("   Phys Base Address : %p (PCI BAR0 / BGA)\n", fb);
        printk("   VRAM Mapping Size : 32 MB (16 x 2MB Huge Pages)\n");
        printk("   PDE Range Index   : PDE[%u..%u]\n", pd_idx, pd_idx + 15);
        printk("   PDE Entry Value   : %p\n", pde);
        printk("   Decoded PDE Flags : ");
        if (pde & PDE_FLAG_PRESENT)  printk("P ");
        if (pde & PDE_FLAG_WRITABLE) printk("RW ");
        if (pde & PDE_FLAG_HUGE)     printk("2MB(PS) ");
        if (pde & PDE_FLAG_PAT)      printk("PAT ");
        if (pde & PDE_FLAG_PCD)      printk("PCD ");
        if (pde & PDE_FLAG_PWT)      printk("PWT ");
        printk("\n");

        printk("   Selected PAT Index: PAT%u -> ", pat_idx);
        if (type == MEM_WC) {
            vga::set_color(vga::Color::LightGreen, vga::Color::Black);
            printk("%s [OPTIMAL]\n", get_mem_type_name(type));
        } else {
            vga::set_color(vga::Color::Yellow, vga::Color::Black);
            printk("%s\n", get_mem_type_name(type));
        }
    } else {
        printk("   (Framebuffer not detected)\n");
    }
    vga::set_color(vga::Color::White, vga::Color::Black);
}

void run_benchmark() {
    uintptr_t fb = bga::get_framebuffer_addr();
    if (!fb) {
        printk("Error: Framebuffer not detected!\n");
        return;
    }

    volatile uint64_t* ptr = reinterpret_cast<volatile uint64_t*>(fb);
    constexpr size_t TEST_SIZE = 16 * 1024 * 1024; // 16 MB test block
    constexpr size_t ITERATIONS = 4;               // 4 passes = 64 MB transferred
    size_t qwords = TEST_SIZE / sizeof(uint64_t);

    uint8_t type = get_page_mem_type(fb);
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("=== Benchmarking Framebuffer Memory Bandwidth ===\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk(" Target Address : %p (%s)\n", fb, get_mem_type_name(type));
    printk(" Total Payload  : %u MB (%u passes of 16 MB 64-bit writes)...\n",
           (ITERATIONS * TEST_SIZE) / (1024 * 1024), ITERATIONS);

    uint64_t start_ms = pit::uptime_ms();

    for (size_t iter = 0; iter < ITERATIONS; ++iter) {
        uint64_t pattern = 0xAA55AA5500000000ULL | (iter << 8);
        for (size_t i = 0; i < qwords; ++i) {
            ptr[i] = pattern;
        }
    }

    uint64_t duration_ms = pit::uptime_ms() - start_ms;
    if (duration_ms == 0) duration_ms = 1;

    uint64_t total_mb = (ITERATIONS * TEST_SIZE) / (1024 * 1024);
    uint64_t mb_per_sec = (total_mb * 1000) / duration_ms;

    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    printk(" Finished in %u ms | Write Bandwidth: %u MB/s\n", duration_ms, mb_per_sec);
    vga::set_color(vga::Color::White, vga::Color::Black);
}

} // namespace pat
