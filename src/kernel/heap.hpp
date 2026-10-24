#ifndef KERNEL_HEAP_HPP
#define KERNEL_HEAP_HPP

#include "types.hpp"

namespace heap {

void init(uintptr_t start_addr, size_t size);
void* malloc(size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t new_size);
size_t get_used_bytes();
size_t get_free_bytes();
size_t get_total_bytes();

} // namespace heap

// Standard C / C++ functions
void* kmalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t new_size);

// C++ operator new/delete declarations
void* operator new(size_t size);
void* operator new[](size_t size);
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, size_t) noexcept;
void operator delete[](void* ptr, size_t) noexcept;

#endif // KERNEL_HEAP_HPP
