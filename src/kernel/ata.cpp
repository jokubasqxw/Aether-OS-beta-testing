#include "ata.hpp"
#include "printk.hpp"
#include "../arch/x86_64/io.hpp"

namespace ata {

// Register offsets from the I/O base port.
enum : uint16_t {
    REG_DATA  = 0,
    REG_COUNT = 2,
    REG_LBA0  = 3,
    REG_LBA1  = 4,
    REG_LBA2  = 5,
    REG_DRIVE = 6,
    REG_CMD   = 7,
};

enum : uint8_t {
    STATUS_BSY = 0x80,
    STATUS_DRDY = 0x40,
    STATUS_DF  = 0x20,
    STATUS_DRQ = 0x08,
    STATUS_ERR = 0x01,
};

enum : uint8_t {
    CMD_READ  = 0x20,
    CMD_WRITE = 0x30,
    CMD_FLUSH = 0xE7,
    CMD_IDENTIFY = 0xEC,
};

static constexpr int MAX_DRIVES = 4;
static Drive drives[MAX_DRIVES];
static int drive_count = 0;

static void io_delay(uint16_t ctrl) {
    for (int i = 0; i < 4; ++i) arch::inb(ctrl);
}

static bool wait_not_busy(uint16_t base, uint16_t ctrl) {
    (void)ctrl;
    for (uint32_t i = 0; i < 1000000; ++i) {
        uint8_t s = arch::inb(base + REG_CMD);
        if (s == 0) return false; // floating bus: no device
        if (!(s & STATUS_BSY)) return true;
    }
    return false;
}

static bool wait_drq(uint16_t base, uint16_t ctrl) {
    (void)ctrl;
    for (uint32_t i = 0; i < 1000000; ++i) {
        uint8_t s = arch::inb(base + REG_CMD);
        if (s & (STATUS_ERR | STATUS_DF)) return false;
        if (!(s & STATUS_BSY) && (s & STATUS_DRQ)) return true;
    }
    return false;
}

static void select(uint16_t base, uint16_t ctrl, bool slave, uint64_t lba) {
    arch::outb(base + REG_DRIVE,
               static_cast<uint8_t>(0xE0 | (slave ? 0x10 : 0x00) |
                                    (static_cast<uint8_t>((lba >> 24) & 0x0F))));
    io_delay(ctrl);
}

static void probe(uint16_t base, uint16_t ctrl, bool slave) {
    if (drive_count >= MAX_DRIVES) return;

    arch::outb(base + REG_DRIVE, static_cast<uint8_t>(0xA0 | (slave ? 0x10 : 0x00)));
    io_delay(ctrl);

    // ATA signature check: LBA mid/low must be zero for plain ATA.
    arch::outb(base + REG_COUNT, 0);
    arch::outb(base + REG_LBA0, 0);
    arch::outb(base + REG_LBA1, 0);
    arch::outb(base + REG_LBA2, 0);
    arch::outb(base + REG_CMD, CMD_IDENTIFY);
    io_delay(ctrl);

    uint8_t status = arch::inb(base + REG_CMD);
    if (status == 0) return; // no device

    if (!wait_not_busy(base, ctrl)) return;
    if (arch::inb(base + REG_LBA1) != 0 || arch::inb(base + REG_LBA2) != 0) return; // ATAPI/SATA
    if (!wait_drq(base, ctrl)) return;

    uint16_t id[256];
    for (int i = 0; i < 256; ++i) id[i] = arch::inw(base + REG_DATA);

    Drive& d = drives[drive_count];
    d.present = true;
    d.slave = slave;
    d.base = base;
    d.ctrl = ctrl;
    d.lba48 = (id[83] & (1 << 10)) != 0;

    // Model string is 40 bytes, byte-swapped across words 27..46.
    for (int i = 0; i < 20; ++i) {
        d.model[i * 2] = static_cast<char>(id[27 + i] >> 8);
        d.model[i * 2 + 1] = static_cast<char>(id[27 + i] & 0xFF);
    }
    d.model[40] = '\0';
    for (int i = 39; i >= 0 && d.model[i] == ' '; --i) d.model[i] = '\0';

    uint32_t lba28_sectors = (static_cast<uint32_t>(id[61]) << 16) | id[60];
    uint64_t sectors = lba28_sectors;
    if (d.lba48) {
        sectors = (static_cast<uint64_t>(id[103]) << 48) |
                  (static_cast<uint64_t>(id[102]) << 32) |
                  (static_cast<uint64_t>(id[101]) << 16) |
                  static_cast<uint64_t>(id[100]);
    }
    d.sectors = sectors;

    drive_count++;
}

void init() {
    drive_count = 0;
    for (int i = 0; i < MAX_DRIVES; ++i) drives[i] = Drive{};

    probe(0x1F0, 0x3F6, false); // primary master
    probe(0x1F0, 0x3F6, true);  // primary slave
    probe(0x170, 0x376, false); // secondary master
    probe(0x170, 0x376, true);  // secondary slave
}

int count() { return drive_count; }

const Drive& drive(int index) {
    static const Drive none{};
    if (index < 0 || index >= drive_count) return none;
    return drives[index];
}

int first_present(int from) {
    for (int i = from; i < drive_count; ++i)
        if (drives[i].present) return i;
    return -1;
}

bool read_sectors(int index, uint64_t lba, uint8_t count, void* buf) {
    if (index < 0 || index >= drive_count) return false;
    const Drive& d = drives[index];
    if (!d.present) return false;
    uint16_t n = (count == 0) ? 256 : count;

    select(d.base, d.ctrl, d.slave, lba);
    if (!wait_not_busy(d.base, d.ctrl)) return false;
    arch::outb(d.base + REG_COUNT, static_cast<uint8_t>(n == 256 ? 0 : n));
    arch::outb(d.base + REG_LBA0, static_cast<uint8_t>(lba & 0xFF));
    arch::outb(d.base + REG_LBA1, static_cast<uint8_t>((lba >> 8) & 0xFF));
    arch::outb(d.base + REG_LBA2, static_cast<uint8_t>((lba >> 16) & 0xFF));
    arch::outb(d.base + REG_CMD, CMD_READ);

    uint16_t* out = static_cast<uint16_t*>(buf);
    for (uint16_t s = 0; s < n; ++s) {
        if (!wait_drq(d.base, d.ctrl)) return false;
        for (int i = 0; i < 256; ++i) *out++ = arch::inw(d.base + REG_DATA);
    }
    return true;
}

bool write_sectors(int index, uint64_t lba, uint8_t count, const void* buf) {
    if (index < 0 || index >= drive_count) return false;
    const Drive& d = drives[index];
    if (!d.present) return false;
    uint16_t n = (count == 0) ? 256 : count;

    select(d.base, d.ctrl, d.slave, lba);
    if (!wait_not_busy(d.base, d.ctrl)) return false;
    arch::outb(d.base + REG_COUNT, static_cast<uint8_t>(n == 256 ? 0 : n));
    arch::outb(d.base + REG_LBA0, static_cast<uint8_t>(lba & 0xFF));
    arch::outb(d.base + REG_LBA1, static_cast<uint8_t>((lba >> 8) & 0xFF));
    arch::outb(d.base + REG_LBA2, static_cast<uint8_t>((lba >> 16) & 0xFF));
    arch::outb(d.base + REG_CMD, CMD_WRITE);

    const uint16_t* in = static_cast<const uint16_t*>(buf);
    for (uint16_t s = 0; s < n; ++s) {
        if (!wait_drq(d.base, d.ctrl)) return false;
        for (int i = 0; i < 256; ++i) arch::outw(d.base + REG_DATA, *in++);
        arch::outb(d.base + REG_CMD, CMD_FLUSH);
        io_delay(d.ctrl);
    }
    return true;
}

} // namespace ata
