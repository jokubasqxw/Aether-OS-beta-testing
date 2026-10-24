#ifndef KERNEL_ATA_HPP
#define KERNEL_ATA_HPP

#include "types.hpp"

namespace ata {

constexpr size_t SECTOR_SIZE = 512;

struct Drive {
    bool present;
    bool lba48;
    bool slave;
    uint16_t base;      // I/O base port (0x1F0 primary, 0x170 secondary)
    uint16_t ctrl;      // control/alt-status port
    uint64_t sectors;   // total addressable 512-byte sectors
    char model[41];
};

void init();

// Number of detected ATA drives (index into drive()).
int count();
const Drive& drive(int index);

// Find the first present drive at or after `from`; returns -1 if none.
int first_present(int from = 0);

// Raw 512-byte sector I/O. `count` sectors starting at `lba`.
bool read_sectors(int index, uint64_t lba, uint8_t count, void* buf);
bool write_sectors(int index, uint64_t lba, uint8_t count, const void* buf);

} // namespace ata

#endif // KERNEL_ATA_HPP
