#ifndef KERNEL_PART_HPP
#define KERNEL_PART_HPP

#include "types.hpp"

namespace part {

struct Info {
    uint32_t lba;     // first sector (absolute LBA on the drive)
    uint32_t sectors; // length in 512-byte sectors
    uint8_t type;     // MBR partition type
};

// Read the MBR of the first present ATA drive and enumerate primary partitions.
// Returns the number of partitions found (0 if no disk / no MBR).
int scan();

int count();
const Info& get(int index);
int drive();          // ATA drive index the partitions live on, or -1

} // namespace part

#endif // KERNEL_PART_HPP
