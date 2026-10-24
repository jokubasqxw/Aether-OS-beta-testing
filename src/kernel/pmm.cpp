#include "pmm.hpp"
#include "kstring.hpp"
#include "printk.hpp"

extern "C" uint8_t __kernel_start[];
extern "C" uint8_t __kernel_end[];

namespace pmm {

// Support up to 4GB of physical RAM
constexpr size_t MAX_FRAMES = 1024 * 1024; // 1M frames * 4KB = 4GB
static uint64_t frame_bitmap[MAX_FRAMES / 64];

static size_t total_memory_bytes = 0;
static size_t used_frames_count = 0;
static size_t total_frames_count = 0;

// End (exclusive) of the contiguous available RAM region that follows the
// kernel image and is addressable through the kernel's identity mapping.
// Machines with lots of RAM (e.g. QEMU's 4 GB layout) split usable memory
// around the PCI hole below 4 GB, so the heap must be sized from this region
// rather than from the total RAM figure.
static uintptr_t heap_region_end = 0;

// Identity mapping covers the first 4 GB of physical address space only.
constexpr uint64_t IDENTITY_LIMIT = 0x100000000ULL;

static inline void set_frame(size_t frame_idx) {
    if (frame_idx < MAX_FRAMES) {
        if (!(frame_bitmap[frame_idx / 64] & (1ULL << (frame_idx % 64)))) {
            frame_bitmap[frame_idx / 64] |= (1ULL << (frame_idx % 64));
            used_frames_count++;
        }
    }
}

static inline void clear_frame(size_t frame_idx) {
    if (frame_idx < MAX_FRAMES) {
        if (frame_bitmap[frame_idx / 64] & (1ULL << (frame_idx % 64))) {
            frame_bitmap[frame_idx / 64] &= ~(1ULL << (frame_idx % 64));
            used_frames_count--;
        }
    }
}

static inline bool test_frame(size_t frame_idx) {
    if (frame_idx >= MAX_FRAMES) return true;
    return (frame_bitmap[frame_idx / 64] & (1ULL << (frame_idx % 64))) != 0;
}

void init(multiboot_info* mbi) {
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    total_frames_count = 0;
    total_memory_bytes = 0;
    heap_region_end = 0;

    uintptr_t kernel_end_addr = reinterpret_cast<uintptr_t>(__kernel_end);

    if (!mbi || !(mbi->flags & (1 << 6))) {
        printk("[PMM] Multiboot memory map not provided, assuming standard 128MB.\n");
        total_memory_bytes = 128 * 1024 * 1024;
        heap_region_end = total_memory_bytes;
    } else {
        uint32_t mmap_addr = mbi->mmap_addr;
        uint32_t mmap_length = mbi->mmap_length;

        // Sum every available region for an accurate installed-RAM total, and
        // remember the end of the region that contains the kernel image so the
        // heap can be grown into contiguous, identity-mapped RAM.
        uint32_t offset = 0;
        while (offset < mmap_length) {
            multiboot_mmap_entry* entry = reinterpret_cast<multiboot_mmap_entry*>(static_cast<uintptr_t>(mmap_addr + offset));
            if (entry->type == 1) { // Available RAM
                total_memory_bytes += static_cast<size_t>(entry->len);

                uint64_t start_addr = entry->addr;
                uint64_t end_addr = entry->addr + entry->len;
                if (start_addr <= kernel_end_addr && kernel_end_addr < end_addr) {
                    uint64_t limit = end_addr < IDENTITY_LIMIT ? end_addr : IDENTITY_LIMIT;
                    heap_region_end = static_cast<uintptr_t>(limit);
                }
            }
            offset += entry->size + sizeof(entry->size);
        }
        if (heap_region_end == 0) {
            // No region contained the kernel (unusual): fall back to the
            // identity-mapped portion of the installed RAM.
            uint64_t limit = total_memory_bytes < IDENTITY_LIMIT ? total_memory_bytes : IDENTITY_LIMIT;
            heap_region_end = static_cast<uintptr_t>(limit);
        }
    }

    total_frames_count = total_memory_bytes / PAGE_SIZE;
    if (total_frames_count > MAX_FRAMES) total_frames_count = MAX_FRAMES;
    used_frames_count = total_frames_count;

    if (!mbi || !(mbi->flags & (1 << 6))) {
        for (size_t i = 0; i < total_frames_count; ++i) {
            clear_frame(i);
        }
    } else {
        uint32_t mmap_addr = mbi->mmap_addr;
        uint32_t mmap_length = mbi->mmap_length;
        uint32_t offset = 0;
        while (offset < mmap_length) {
            multiboot_mmap_entry* entry = reinterpret_cast<multiboot_mmap_entry*>(static_cast<uintptr_t>(mmap_addr + offset));
            if (entry->type == 1) {
                size_t start_frame = entry->addr / PAGE_SIZE;
                size_t end_frame = (entry->addr + entry->len) / PAGE_SIZE;
                if (end_frame > total_frames_count) end_frame = total_frames_count;
                for (size_t f = start_frame; f < end_frame; ++f) {
                    clear_frame(f);
                }
            }
            offset += entry->size + sizeof(entry->size);
        }
    }

    // Reserve first 1MB (BIOS, real-mode IVT, VGA buffer)
    size_t first_mb_frames = (1024 * 1024) / PAGE_SIZE;
    for (size_t f = 0; f < first_mb_frames; ++f) {
        set_frame(f);
    }

    // Reserve kernel image (from __kernel_start to __kernel_end + some headroom)
    uintptr_t kstart = reinterpret_cast<uintptr_t>(__kernel_start);
    uintptr_t kend = reinterpret_cast<uintptr_t>(__kernel_end) + 0x400000; // + 4MB safety margin
    size_t kstart_frame = kstart / PAGE_SIZE;
    size_t kend_frame = (kend + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t f = kstart_frame; f <= kend_frame; ++f) {
        set_frame(f);
    }
}

void* alloc_frame() {
    for (size_t i = 0; i < total_frames_count / 64; ++i) {
        if (frame_bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
            for (size_t b = 0; b < 64; ++b) {
                if (!(frame_bitmap[i] & (1ULL << b))) {
                    size_t frame_idx = i * 64 + b;
                    set_frame(frame_idx);
                    return reinterpret_cast<void*>(frame_idx * PAGE_SIZE);
                }
            }
        }
    }
    return nullptr; // Out of physical memory
}

void free_frame(void* frame) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(frame);
    size_t frame_idx = addr / PAGE_SIZE;
    clear_frame(frame_idx);
}

void reserve_region(uintptr_t addr, size_t size) {
    if (size == 0) return;
    size_t start_frame = addr / PAGE_SIZE;
    size_t end_frame = (addr + size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t f = start_frame; f < end_frame; ++f) {
        set_frame(f);
    }
}

size_t get_total_memory() {
    return total_memory_bytes;
}

uintptr_t get_heap_region_end() {
    return heap_region_end;
}

size_t get_used_memory() {
    return used_frames_count * PAGE_SIZE;
}

size_t get_free_memory() {
    if (used_frames_count >= total_frames_count) return 0;
    return (total_frames_count - used_frames_count) * PAGE_SIZE;
}

size_t get_total_frames() {
    return total_frames_count;
}

size_t get_used_frames() {
    return used_frames_count;
}

} // namespace pmm
