#ifndef KERNEL_RAMFS_HPP
#define KERNEL_RAMFS_HPP

#include "types.hpp"

namespace ramfs {

constexpr size_t MAX_FILES = 64;
constexpr size_t MAX_FILENAME_LEN = 32;

struct File {
    char name[MAX_FILENAME_LEN];
    char* content;
    size_t size;
    size_t capacity;
    bool used;
};

void init();
bool create(const char* name);
bool write(const char* name, const char* text, bool append = false);
bool write_bytes(const char* name, const void* data, size_t size, bool append = false);
const char* read(const char* name);
const void* read_bytes(const char* name, size_t* out_size = nullptr);
bool remove(const char* name);
void list();
bool exists(const char* name);
const File* get_files();

} // namespace ramfs

#endif // KERNEL_RAMFS_HPP
