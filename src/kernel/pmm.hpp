#ifndef KERNEL_PMM_HPP
#define KERNEL_PMM_HPP

#include "types.hpp"
#include "multiboot.hpp"

namespace pmm {

constexpr size_t PAGE_SIZE = 4096;

void init(multiboot_info* mbi);
void* alloc_frame();
void free_frame(void* frame);

// Mark a physical range as reserved so alloc_frame() never hands it out
// (used for the kernel heap, whose memory is managed independently).
void reserve_region(uintptr_t addr, size_t size);

size_t get_total_memory();
// End (exclusive) of the contiguous, identity-mapped RAM region available for
// the kernel heap (i.e. the region directly after the kernel image).
uintptr_t get_heap_region_end();
size_t get_used_memory();
size_t get_free_memory();
size_t get_total_frames();
size_t get_used_frames();

} // namespace pmm

#endif // KERNEL_PMM_HPP
