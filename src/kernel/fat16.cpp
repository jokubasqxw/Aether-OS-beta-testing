#include "fat16.hpp"
#include "ata.hpp"
#include "part.hpp"
#include "kstring.hpp"
#include "printk.hpp"

namespace fat16 {

static constexpr uint16_t SECTOR = 512;
static constexpr uint16_t FAT_FREE = 0x0000;
static constexpr uint16_t FAT_EOF  = 0xFFFF;
static constexpr uint16_t FAT_BAD  = 0xFFF7;

static int g_drive = -1;
static uint32_t g_part_lba = 0;

static uint16_t g_bytes_per_sector = 0;
static uint8_t  g_sectors_per_cluster = 0;
static uint16_t g_reserved = 0;
static uint8_t  g_num_fats = 0;
static uint16_t g_root_entries = 0;
static uint16_t g_fatsz = 0;
static uint32_t g_root_dir_sector = 0;   // relative to partition
static uint32_t g_first_data_sector = 0; // relative to partition
static uint32_t g_total_sectors = 0;
static uint16_t g_cluster_count = 0;
static bool g_mounted = false;

static uint8_t g_sec[SECTOR];

static bool read_at(uint32_t rel_sector, void* buf, uint8_t count) {
    return ata::read_sectors(g_drive, static_cast<uint64_t>(g_part_lba) + rel_sector, count, buf);
}

static bool write_at(uint32_t rel_sector, const void* buf, uint8_t count) {
    return ata::write_sectors(g_drive, static_cast<uint64_t>(g_part_lba) + rel_sector, count, buf);
}

static uint32_t cluster_to_sector(uint16_t cluster) {
    return g_first_data_sector + static_cast<uint32_t>(cluster - 2) * g_sectors_per_cluster;
}

static uint16_t fat_get(uint16_t cluster) {
    uint32_t fat_sector = g_reserved + (static_cast<uint32_t>(cluster) * 2) / SECTOR;
    uint32_t offset = (static_cast<uint32_t>(cluster) * 2) % SECTOR;
    if (!read_at(fat_sector, g_sec, 1)) return FAT_EOF;
    uint16_t v;
    kstd::memcpy(&v, g_sec + offset, 2);
    return v;
}

static bool fat_set(uint16_t cluster, uint16_t value) {
    uint32_t fat_sector = g_reserved + (static_cast<uint32_t>(cluster) * 2) / SECTOR;
    uint32_t offset = (static_cast<uint32_t>(cluster) * 2) % SECTOR;
    for (uint8_t f = 0; f < g_num_fats; ++f) {
        uint32_t s = fat_sector + static_cast<uint32_t>(f) * g_fatsz;
        if (!read_at(s, g_sec, 1)) return false;
        kstd::memcpy(g_sec + offset, &value, 2);
        if (!write_at(s, g_sec, 1)) return false;
    }
    return true;
}

static uint16_t alloc_cluster() {
    for (uint32_t c = 2; c < static_cast<uint32_t>(g_cluster_count) + 2; ++c) {
        if (fat_get(static_cast<uint16_t>(c)) == FAT_FREE) return static_cast<uint16_t>(c);
    }
    return 0;
}

// Find a root-directory entry by 11-char 8.3 name. Returns the byte offset of
// the entry within the root directory region, or -1.
static long find_root_entry(const char* name83) {
    uint32_t root_sectors = (static_cast<uint32_t>(g_root_entries) * 32 + SECTOR - 1) / SECTOR;
    for (uint32_t s = 0; s < root_sectors; ++s) {
        if (!read_at(g_root_dir_sector + s, g_sec, 1)) return -1;
        for (int e = 0; e < SECTOR / 32; ++e) {
            uint8_t* de = g_sec + e * 32;
            if (de[0] == 0x00) return -1;       // end of directory
            if (de[0] == 0xE5) continue;         // deleted
            if (de[11] == 0x0F) continue;        // long-file-name entry
            bool same = true;
            for (int k = 0; k < 11; ++k) {
                if (de[k] != static_cast<uint8_t>(name83[k])) { same = false; break; }
            }
            if (same) {
                return static_cast<long>(s) * SECTOR + e * 32;
            }
        }
    }
    return -1;
}

static void read_entry(const char* name83, uint16_t* first_cluster_out, uint32_t* size_out, long* off_out) {
    *first_cluster_out = 0;
    *size_out = 0;
    *off_out = find_root_entry(name83);
    if (*off_out < 0) return;
    uint32_t sec = g_root_dir_sector + static_cast<uint32_t>(*off_out) / SECTOR;
    uint32_t eoff = static_cast<uint32_t>(*off_out) % SECTOR;
    if (!read_at(sec, g_sec, 1)) return;
    kstd::memcpy(first_cluster_out, g_sec + eoff + 26, 2);
    kstd::memcpy(size_out, g_sec + eoff + 28, 4);
}

bool init() {
    if (part::count() == 0) part::scan();
    g_drive = part::drive();
    if (g_drive < 0) return false;

    int pc = part::count();
    for (int i = 0; i < pc; ++i) {
        const part::Info& pi = part::get(i);
        g_part_lba = pi.lba;
        if (!read_at(0, g_sec, 1)) continue;
        // Validate a FAT16 BPB.
        uint16_t bps; kstd::memcpy(&bps, g_sec + 11, 2);
        if (bps != SECTOR) continue;
        uint16_t fatsz; kstd::memcpy(&fatsz, g_sec + 22, 2);
        if (fatsz == 0) continue;

        kstd::memcpy(&g_reserved, g_sec + 14, 2);
        g_num_fats = g_sec[16];
        kstd::memcpy(&g_root_entries, g_sec + 17, 2);
        kstd::memcpy(&g_total_sectors, g_sec + 19, 2);
        if (g_total_sectors == 0) kstd::memcpy(&g_total_sectors, g_sec + 32, 4);
        g_sectors_per_cluster = g_sec[13];
        g_bytes_per_sector = bps;
        g_fatsz = fatsz;

        uint32_t root_dir_sectors = (static_cast<uint32_t>(g_root_entries) * 32 + SECTOR - 1) / SECTOR;
        g_root_dir_sector = g_reserved + static_cast<uint32_t>(g_num_fats) * g_fatsz;
        g_first_data_sector = g_root_dir_sector + root_dir_sectors;
        uint32_t data_sectors = g_total_sectors - g_first_data_sector;
        g_cluster_count = static_cast<uint16_t>(data_sectors / g_sectors_per_cluster);
        g_mounted = true;
        printk("[OK] FAT16 boot partition mounted (hd%up%u, %u clusters).\n",
               static_cast<unsigned>(g_drive), static_cast<unsigned>(i + 1),
               static_cast<unsigned>(g_cluster_count));
        return true;
    }
    return false;
}

bool mounted() { return g_mounted; }

int file_size(const char* name83) {
    if (!g_mounted) return -1;
    uint16_t fc; uint32_t size; long off;
    read_entry(name83, &fc, &size, &off);
    if (off < 0) return -1;
    return static_cast<int>(size);
}

int read_file(const char* name83, void* buf, size_t max) {
    if (!g_mounted) return -1;
    uint16_t cluster; uint32_t size; long off;
    read_entry(name83, &cluster, &size, &off);
    if (off < 0) return -1;

    uint8_t* dst = static_cast<uint8_t*>(buf);
    uint32_t copied = 0;
    while (cluster >= 2 && cluster < FAT_BAD && copied < size && copied < max) {
        uint32_t base = cluster_to_sector(cluster);
        for (uint8_t s = 0; s < g_sectors_per_cluster && copied < size && copied < max; ++s) {
            if (!read_at(base + s, g_sec, 1)) return static_cast<int>(copied);
            uint32_t chunk = size - copied;
            if (chunk > SECTOR) chunk = SECTOR;
            if (copied + chunk > max) chunk = static_cast<uint32_t>(max) - copied;
            kstd::memcpy(dst + copied, g_sec, chunk);
            copied += chunk;
        }
        cluster = fat_get(cluster);
    }
    return static_cast<int>(copied);
}

bool write_file(const char* name83, const void* data, size_t size) {
    if (!g_mounted) return false;

    uint16_t first; uint32_t old_size; long off;
    read_entry(name83, &first, &old_size, &off);
    bool create = (off < 0);
    if (create) {
        // Find a free root directory entry.
        uint32_t root_sectors = (static_cast<uint32_t>(g_root_entries) * 32 + SECTOR - 1) / SECTOR;
        for (uint32_t s = 0; s < root_sectors && off < 0; ++s) {
            if (!read_at(g_root_dir_sector + s, g_sec, 1)) return false;
            for (int e = 0; e < SECTOR / 32; ++e) {
                uint8_t* de = g_sec + e * 32;
                if (de[0] == 0x00 || de[0] == 0xE5) {
                    off = static_cast<long>(s) * SECTOR + e * 32;
                    break;
                }
            }
        }
        if (off < 0) return false;
        first = alloc_cluster();
        if (first == 0) return false;
        fat_set(first, FAT_EOF);
        // Write an empty directory entry.
        uint32_t sec = g_root_dir_sector + static_cast<uint32_t>(off) / SECTOR;
        uint32_t eoff = static_cast<uint32_t>(off) % SECTOR;
        if (!read_at(sec, g_sec, 1)) return false;
        uint8_t* de = g_sec + eoff;
        kstd::memset(de, 0, 32);
        for (int k = 0; k < 11; ++k) de[k] = static_cast<uint8_t>(name83[k]);
        de[11] = 0x20; // archive
        uint16_t cl = first;
        kstd::memcpy(de + 26, &cl, 2);
        uint32_t zero = 0;
        kstd::memcpy(de + 28, &zero, 4);
        if (!write_at(sec, g_sec, 1)) return false;
    }

    if (first < 2) return false;

    const uint8_t* src = static_cast<const uint8_t*>(data);
    uint32_t written = 0;
    uint16_t cluster = first;
    while (written < size) {
        uint32_t base = cluster_to_sector(cluster);
        for (uint8_t s = 0; s < g_sectors_per_cluster && written < size; ++s) {
            uint32_t chunk = size - written;
            if (chunk > SECTOR) chunk = SECTOR;
            kstd::memset(g_sec, 0, SECTOR);
            kstd::memcpy(g_sec, src + written, chunk);
            if (!write_at(base + s, g_sec, 1)) return false;
            written += chunk;
        }
        if (written < size) {
            uint16_t next = fat_get(cluster);
            if (next >= 2 && next < FAT_BAD) {
                cluster = next;
            } else {
                uint16_t nc = alloc_cluster();
                if (nc == 0) return false;
                fat_set(cluster, nc);
                fat_set(nc, FAT_EOF);
                cluster = nc;
            }
        }
    }

    // Update the directory entry's size.
    uint32_t sec = g_root_dir_sector + static_cast<uint32_t>(off) / SECTOR;
    uint32_t eoff = static_cast<uint32_t>(off) % SECTOR;
    if (!read_at(sec, g_sec, 1)) return false;
    uint32_t sz = static_cast<uint32_t>(size);
    kstd::memcpy(g_sec + eoff + 28, &sz, 4);
    uint16_t fc = first;
    kstd::memcpy(g_sec + eoff + 26, &fc, 2);
    return write_at(sec, g_sec, 1);
}

} // namespace fat16
