#ifndef KERNEL_FAT16_HPP
#define KERNEL_FAT16_HPP

#include "types.hpp"

namespace fat16 {

// Locate and parse the FAT (boot) partition on the primary ATA drive.
bool init();
bool mounted();

// Read a file from the root directory (8.3 short name, e.g. "AETHEROSBIN").
// Returns bytes read, or -1 if not found.
int read_file(const char* name83, void* buf, size_t max);

// Overwrite (or create) a root-directory file. Extends the cluster chain as
// needed. Returns true on success.
bool write_file(const char* name83, const void* data, size_t size);

int file_size(const char* name83);

} // namespace fat16

#endif // KERNEL_FAT16_HPP
