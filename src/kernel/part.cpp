#include "part.hpp"
#include "ata.hpp"
#include "kstring.hpp"

namespace part {

static constexpr int MAX_PARTS = 4;
static Info parts[MAX_PARTS];
static int part_count = 0;
static int part_drive = -1;

int scan() {
    part_count = 0;
    part_drive = ata::first_present();
    if (part_drive < 0) return 0;

    uint8_t mbr[512];
    if (!ata::read_sectors(part_drive, 0, 1, mbr)) return 0;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;

    const int off = 446;
    for (int i = 0; i < 4 && part_count < MAX_PARTS; ++i) {
        const uint8_t* e = mbr + off + i * 16;
        uint32_t lba, sectors;
        kstd::memcpy(&lba, e + 8, 4);
        kstd::memcpy(&sectors, e + 12, 4);
        if (sectors == 0 || lba == 0) continue;
        parts[part_count].lba = lba;
        parts[part_count].sectors = sectors;
        parts[part_count].type = e[4];
        part_count++;
    }
    return part_count;
}

int count() { return part_count; }

const Info& get(int index) {
    static const Info none{};
    if (index < 0 || index >= part_count) return none;
    return parts[index];
}

int drive() { return part_drive; }

} // namespace part
