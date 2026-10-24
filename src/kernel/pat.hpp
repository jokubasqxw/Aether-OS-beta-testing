#ifndef KERNEL_PAT_HPP
#define KERNEL_PAT_HPP

#include "types.hpp"

namespace pat {

// MSR Addresses
constexpr uint32_t IA32_PAT_MSR = 0x277;

// PAT Memory Type Encodings (Intel SDM Table 11-12 / 11-7)
constexpr uint8_t MEM_UC  = 0x00; // Uncacheable
constexpr uint8_t MEM_WC  = 0x01; // Write-Combining
constexpr uint8_t MEM_WT  = 0x04; // Write-Through
constexpr uint8_t MEM_WP  = 0x05; // Write-Protected
constexpr uint8_t MEM_WB  = 0x06; // Write-Back
constexpr uint8_t MEM_UCM = 0x07; // UC- (Weakly Uncacheable, can be overridden by WC MTRR)

// Intel Standard Reset Value:
// PA0=WB(06), PA1=WT(04), PA2=UC-(07), PA3=UC(00), PA4=WB(06), PA5=WT(04), PA6=UC-(07), PA7=UC(00)
constexpr uint64_t PAT_DEFAULT_MSR = 0x0007040600070406ULL;

// AetherOS High-Performance Configuration:
// PA0=WB(06), PA1=WC(01), PA2=UC-(07), PA3=UC(00), PA4=WC(01), PA5=WT(04), PA6=UC-(07), PA7=UC(00)
// This layout provides dual WC selection:
//   - PAT Index 1 (PWT=1, PCD=0, PAT=0) -> WC
//   - PAT Index 4 (PAT=1, PCD=0, PWT=0) -> WC
constexpr uint64_t PAT_AETHEROS_MSR = 0x0007040100070106ULL;

// Paging Flags for 2MB Huge Pages (Page Directory Entries)
constexpr uint64_t PDE_FLAG_PRESENT   = (1ULL << 0);
constexpr uint64_t PDE_FLAG_WRITABLE  = (1ULL << 1);
constexpr uint64_t PDE_FLAG_USER      = (1ULL << 2);
constexpr uint64_t PDE_FLAG_PWT       = (1ULL << 3);  // Page-level Write-Through
constexpr uint64_t PDE_FLAG_PCD       = (1ULL << 4);  // Page-level Cache Disable
constexpr uint64_t PDE_FLAG_ACCESSED  = (1ULL << 5);
constexpr uint64_t PDE_FLAG_DIRTY     = (1ULL << 6);
constexpr uint64_t PDE_FLAG_HUGE      = (1ULL << 7);  // 2MB Page Size (PS=1)
constexpr uint64_t PDE_FLAG_GLOBAL    = (1ULL << 8);
constexpr uint64_t PDE_FLAG_PAT       = (1ULL << 12); // PAT bit for 2MB huge page

// Caching bitmask in 2MB PDE (PWT, PCD, PAT)
constexpr uint64_t PDE_CACHE_MASK     = PDE_FLAG_PWT | PDE_FLAG_PCD | PDE_FLAG_PAT;

// Initialize IA32_PAT MSR with AetherOS configuration
bool init();

// Check if CPU supports PAT (CPUID.01H:EDX[16])
bool is_supported();

// Get raw IA32_PAT MSR value
uint64_t get_pat_msr();

// Get memory type encoding for given PAT index (0..7)
uint8_t get_pat_entry(uint8_t index);

// Get human-readable description of memory type
const char* get_mem_type_name(uint8_t type);
const char* get_mem_type_short_name(uint8_t type);

// Modify memory caching type for a 2MB-aligned physical address range
bool set_range_caching(uintptr_t phys_addr, size_t size, uint8_t pat_index);
bool set_range_wc(uintptr_t phys_addr, size_t size);
bool set_range_wb(uintptr_t phys_addr, size_t size);
bool set_range_uc(uintptr_t phys_addr, size_t size);

// Flush TLB entries and processor caches for memory range
void flush_range(uintptr_t phys_addr, size_t size);

// Inspect PDE for a physical address
uint64_t get_pde(uintptr_t phys_addr);
uint8_t get_page_pat_index(uintptr_t phys_addr);
uint8_t get_page_mem_type(uintptr_t phys_addr);

// Status and diagnostics
void print_status();
void run_benchmark();

} // namespace pat

#endif // KERNEL_PAT_HPP
