#ifndef KERNEL_AETHERFS_HPP
#define KERNEL_AETHERFS_HPP

#include "types.hpp"

namespace aetherfs {

constexpr size_t BLOCK_SIZE = 4096;
constexpr size_t MAX_NAME = 32;
constexpr size_t MAX_FILES = 64;

struct Entry {
    char name[MAX_NAME];
    uint32_t size;        // used bytes
    uint32_t alloc;       // allocated bytes (rounded to blocks)
    uint32_t start_block; // first data block
    uint32_t flags;       // bit0 = used, bit1 = locked
};

enum Flags : uint32_t {
    FLAG_USED   = 1u << 0,
    FLAG_LOCKED = 1u << 1,
};

// Locate (and mount, or first-time format) the AetherFS partition on the
// primary ATA drive. Returns true if a filesystem is mounted afterwards.
bool init();

bool mounted();
const char* device_name();

// File operations (flat namespace).
bool write_file(const char* name, const void* data, size_t size);
int  read_file(const char* name, void* buf, size_t max); // returns bytes, -1 if missing
bool remove_file(const char* name);
bool exists(const char* name);
int  file_size(const char* name);
const Entry* entries();          // array of MAX_FILES
int  file_count();

// Snapshot / rollback (used by the updater for crash-safe updates).
bool snapshot();
bool snapshot_valid();
bool restore();

// Update lock: a special file "update.lock" holding "true"/"false".
bool set_update_lock(bool locked);
bool update_lock_engaged(); // true if an update was in progress and was interrupted

// Statistics.
uint32_t used_blocks();
uint32_t total_blocks();

} // namespace aetherfs

#endif // KERNEL_AETHERFS_HPP
