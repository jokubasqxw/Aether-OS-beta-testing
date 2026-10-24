#include "heap.hpp"
#include "kstring.hpp"
#include "printk.hpp"

#define HEAP_MAGIC 0x48454150 // 'HEAP'

namespace heap {

struct BlockHeader {
    uint32_t magic;
    bool is_free;
    size_t size; // Size of usable payload in bytes
    BlockHeader* next;
};

static BlockHeader* head_block = nullptr;
static uintptr_t heap_start = 0;
static size_t heap_total_size = 0;
static size_t used_bytes = 0;

void init(uintptr_t start_addr, size_t size) {
    // 16-byte align the heap start
    if (start_addr % 16 != 0) {
        start_addr += (16 - (start_addr % 16));
    }
    heap_start = start_addr;
    heap_total_size = size;
    used_bytes = 0;

    head_block = reinterpret_cast<BlockHeader*>(heap_start);
    head_block->magic = HEAP_MAGIC;
    head_block->is_free = true;
    head_block->size = size - sizeof(BlockHeader);
    head_block->next = nullptr;
}

void* malloc(size_t size) {
    if (size == 0) return nullptr;

    // 16-byte align size
    if (size % 16 != 0) {
        size += (16 - (size % 16));
    }

    BlockHeader* curr = head_block;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            printk("[HEAP] Memory corruption detected at %p!\n", curr);
            return nullptr;
        }

        if (curr->is_free && curr->size >= size) {
            // Can we split the block?
            if (curr->size >= size + sizeof(BlockHeader) + 16) {
                BlockHeader* next_block = reinterpret_cast<BlockHeader*>(
                    reinterpret_cast<uint8_t*>(curr) + sizeof(BlockHeader) + size
                );
                next_block->magic = HEAP_MAGIC;
                next_block->is_free = true;
                next_block->size = curr->size - size - sizeof(BlockHeader);
                next_block->next = curr->next;

                curr->size = size;
                curr->next = next_block;
            }

            curr->is_free = false;
            used_bytes += curr->size;
            return reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(curr) + sizeof(BlockHeader));
        }

        curr = curr->next;
    }

    printk("[HEAP] Out of memory! Requested: %u bytes\n", size);
    return nullptr;
}

void free(void* ptr) {
    if (!ptr) return;

    BlockHeader* block = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(BlockHeader)
    );

    if (block->magic != HEAP_MAGIC) {
        printk("[HEAP] Invalid free of %p (corrupted magic)!\n", ptr);
        return;
    }

    if (block->is_free) {
        printk("[HEAP] Double free detected at %p!\n", ptr);
        return;
    }

    block->is_free = true;
    if (used_bytes >= block->size) {
        used_bytes -= block->size;
    }

    // Coalesce adjacent free blocks
    BlockHeader* curr = head_block;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(BlockHeader) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void* realloc(void* ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) {
        free(ptr);
        return nullptr;
    }

    BlockHeader* block = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(BlockHeader)
    );

    if (block->magic != HEAP_MAGIC) {
        return nullptr;
    }

    if (block->size >= new_size) {
        return ptr;
    }

    void* new_ptr = malloc(new_size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    return new_ptr;
}

size_t get_used_bytes() {
    return used_bytes;
}

size_t get_free_bytes() {
    if (used_bytes >= heap_total_size) return 0;
    return heap_total_size - used_bytes;
}

size_t get_total_bytes() {
    return heap_total_size;
}

} // namespace heap

void* kmalloc(size_t size) {
    return heap::malloc(size);
}

void kfree(void* ptr) {
    heap::free(ptr);
}

void* krealloc(void* ptr, size_t new_size) {
    return heap::realloc(ptr, new_size);
}

void* operator new(size_t size) {
    return kmalloc(size);
}

void* operator new[](size_t size) {
    return kmalloc(size);
}

void operator delete(void* ptr) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr) noexcept {
    kfree(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    kfree(ptr);
}
