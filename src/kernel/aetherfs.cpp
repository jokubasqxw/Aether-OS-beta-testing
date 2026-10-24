#include "aetherfs.hpp"
#include "ata.hpp"
#include "part.hpp"
#include "kstring.hpp"
#include "printk.hpp"

namespace aetherfs {

static const char MAGIC[8] = {'A', 'E', 'T', 'H', 'F', 'S', '0', '1'};

#pragma pack(push, 1)
struct Superblock {
    char magic[8];
    uint32_t version;
    uint32_t block_size;
    uint32_t table_blocks;
    uint32_t data_start;
    uint32_t snap_start;
    uint32_t snap_blocks;
    uint32_t snap_valid;
    uint32_t snap_end;
    uint32_t free_block;
    uint32_t blocks_total;
};
#pragma pack(pop)

static constexpr uint32_t VERSION = 1;
static constexpr uint32_t TABLE_BLOCKS = 1;

static int g_drive = -1;
static uint32_t g_part_lba = 0;
static uint32_t g_blocks = 0;
static Superblock g_sb;
static Entry g_table[MAX_FILES];
static bool g_mounted = false;
static char g_devname[24];

static uint8_t g_scratch[BLOCK_SIZE];

static bool read_block(uint32_t rel, void* buf) {
    if (g_drive < 0) return false;
    uint64_t lba = static_cast<uint64_t>(g_part_lba) + static_cast<uint64_t>(rel) * (BLOCK_SIZE / ata::SECTOR_SIZE);
    return ata::read_sectors(g_drive, lba, BLOCK_SIZE / ata::SECTOR_SIZE, buf);
}

static bool write_block(uint32_t rel, const void* buf) {
    uint64_t lba = static_cast<uint64_t>(g_part_lba) + static_cast<uint64_t>(rel) * (BLOCK_SIZE / ata::SECTOR_SIZE);
    return ata::write_sectors(g_drive, lba, BLOCK_SIZE / ata::SECTOR_SIZE, buf);
}

static int find_entry(const char* name) {
    for (size_t i = 0; i < MAX_FILES; ++i) {
        if (!(g_table[i].flags & FLAG_USED)) continue;
        if (g_table[i].name[0] == '\0') continue;
        bool same = true;
        for (size_t k = 0; k < MAX_NAME; ++k) {
            char a = g_table[i].name[k];
            char b = name[k];
            if (a != b) { same = false; break; }
            if (a == '\0') break;
        }
        if (same) return static_cast<int>(i);
    }
    return -1;
}

static bool store_metadata() {
    uint8_t* buf = g_scratch;
    kstd::memset(buf, 0, BLOCK_SIZE);
    kstd::memcpy(buf, &g_sb, sizeof(Superblock));
    if (!write_block(0, buf)) return false;

    kstd::memset(buf, 0, BLOCK_SIZE);
    kstd::memcpy(buf, g_table, sizeof(g_table) < BLOCK_SIZE ? sizeof(g_table) : BLOCK_SIZE);
    return write_block(1, buf);
}

static bool load_metadata() {
    if (!read_block(0, g_scratch)) return false;
    kstd::memcpy(&g_sb, g_scratch, sizeof(Superblock));

    if (kstd::memcmp(g_sb.magic, MAGIC, 8) != 0) return false;
    if (g_sb.block_size != BLOCK_SIZE) return false;

    if (!read_block(1, g_scratch)) return false;
    kstd::memcpy(g_table, g_scratch, sizeof(g_table) < BLOCK_SIZE ? sizeof(g_table) : BLOCK_SIZE);
    return true;
}

static bool format_partition(uint32_t lba, uint32_t sectors) {
    g_part_lba = lba;
    g_blocks = sectors / (BLOCK_SIZE / ata::SECTOR_SIZE);
    if (g_blocks < 64) return false;

    kstd::memset(&g_sb, 0, sizeof(g_sb));
    kstd::memcpy(g_sb.magic, MAGIC, 8);
    g_sb.version = VERSION;
    g_sb.block_size = BLOCK_SIZE;
    g_sb.table_blocks = TABLE_BLOCKS;
    g_sb.data_start = 2;
    g_sb.snap_blocks = g_blocks / 4;          // last 25% reserved for snapshots
    g_sb.snap_start = g_blocks - g_sb.snap_blocks;
    g_sb.snap_valid = 0;
    g_sb.free_block = g_sb.data_start;
    g_sb.blocks_total = g_blocks;

    kstd::memset(g_table, 0, sizeof(g_table));
    g_mounted = true;
    return store_metadata();
}

static bool try_mount(uint32_t lba, uint32_t sectors) {
    g_part_lba = lba;
    g_blocks = sectors / (BLOCK_SIZE / ata::SECTOR_SIZE);
    g_mounted = false;
    if (!read_block(0, g_scratch)) return false;
    Superblock tmp;
    kstd::memcpy(&tmp, g_scratch, sizeof(tmp));
    if (kstd::memcmp(tmp.magic, MAGIC, 8) != 0) return false;
    if (tmp.block_size != BLOCK_SIZE) return false;

    g_mounted = true;
    if (!load_metadata()) { g_mounted = false; return false; }
    return true;
}

bool init() {
    int pc = part::count() > 0 ? part::count() : part::scan();
    g_drive = part::drive();
    if (g_drive < 0 || pc == 0) return false;

    // Prefer an already-formatted AetherFS partition.
    for (int i = 0; i < pc; ++i) {
        const part::Info& pi = part::get(i);
        if (try_mount(pi.lba, pi.sectors)) {
            printk("[OK] AetherFS mounted partition %d (hd%up%u, %u blocks).\n",
                   i + 1, static_cast<unsigned>(g_drive), static_cast<unsigned>(i + 1),
                   static_cast<unsigned>(g_blocks));
            return true;
        }
    }

    // None found: format the second partition (index 1), else the first.
    int target = pc > 1 ? 1 : 0;
    const part::Info& pi = part::get(target);
    if (!format_partition(pi.lba, pi.sectors)) return false;
    printk("[OK] AetherFS formatted new partition %d (hd%up%u, %u blocks).\n",
           target + 1, static_cast<unsigned>(g_drive), static_cast<unsigned>(target + 1),
           static_cast<unsigned>(g_blocks));
    return true;
}

bool mounted() { return g_mounted; }
const char* device_name() { return g_devname; }

static bool alloc_blocks(uint32_t n, uint32_t* out_start) {
    if (g_sb.free_block + n > g_sb.snap_start) return false;
    *out_start = g_sb.free_block;
    g_sb.free_block += n;
    return true;
}

bool write_file(const char* name, const void* data, size_t size) {
    if (!g_mounted) return false;
    if (size > 0xFFFFFFFFULL) return false;

    uint32_t blocks_needed = static_cast<uint32_t>((size + BLOCK_SIZE - 1) / BLOCK_SIZE);
    if (blocks_needed == 0) blocks_needed = 1;

    int idx = find_entry(name);
    uint32_t start;
    if (idx >= 0 && g_table[idx].alloc >= blocks_needed * BLOCK_SIZE) {
        start = g_table[idx].start_block;
    } else {
        if (!alloc_blocks(blocks_needed, &start)) {
            printk("[AetherFS] out of space writing '%s' (%u blocks)\n", name, blocks_needed);
            return false;
        }
        if (idx < 0) {
            for (size_t i = 0; i < MAX_FILES; ++i) {
                if (!(g_table[i].flags & FLAG_USED)) { idx = static_cast<int>(i); break; }
            }
        }
        if (idx < 0) {
            printk("[AetherFS] file table full\n");
            return false;
        }
        // Mark the new location before writing so it is never orphaned on crash.
        kstd::memset(&g_table[idx], 0, sizeof(Entry));
        kstd::strncpy(g_table[idx].name, name, MAX_NAME - 1);
        g_table[idx].start_block = start;
        g_table[idx].flags = FLAG_USED;
    }

    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (uint32_t b = 0; b < blocks_needed; ++b) {
        size_t remain = size - static_cast<size_t>(b) * BLOCK_SIZE;
        size_t chunk = remain < BLOCK_SIZE ? remain : BLOCK_SIZE;
        kstd::memset(g_scratch, 0, BLOCK_SIZE);
        if (chunk) kstd::memcpy(g_scratch, src + static_cast<size_t>(b) * BLOCK_SIZE, chunk);
        if (!write_block(start + b, g_scratch)) return false;
    }

    g_table[idx].size = static_cast<uint32_t>(size);
    g_table[idx].alloc = blocks_needed * BLOCK_SIZE;
    return store_metadata();
}

int read_file(const char* name, void* buf, size_t max) {
    if (!g_mounted) return -1;
    int idx = find_entry(name);
    if (idx < 0) return -1;

    uint32_t size = g_table[idx].size;
    uint32_t start = g_table[idx].start_block;
    uint8_t* dst = static_cast<uint8_t*>(buf);
    uint32_t copied = 0;
    uint32_t blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (uint32_t b = 0; b < blocks && copied < max; ++b) {
        if (!read_block(start + b, g_scratch)) break;
        uint32_t remain = size - copied;
        uint32_t chunk = remain < BLOCK_SIZE ? remain : BLOCK_SIZE;
        if (copied + chunk > max) chunk = static_cast<uint32_t>(max) - copied;
        kstd::memcpy(dst + copied, g_scratch, chunk);
        copied += chunk;
    }
    return static_cast<int>(copied);
}

bool remove_file(const char* name) {
    if (!g_mounted) return false;
    int idx = find_entry(name);
    if (idx < 0) return false;
    g_table[idx].flags = 0;
    g_table[idx].name[0] = '\0';
    return store_metadata();
}

bool exists(const char* name) {
    return g_mounted && find_entry(name) >= 0;
}

int file_size(const char* name) {
    int idx = find_entry(name);
    return idx < 0 ? -1 : static_cast<int>(g_table[idx].size);
}

const Entry* entries() { return g_table; }

int file_count() {
    int c = 0;
    for (size_t i = 0; i < MAX_FILES; ++i)
        if (g_table[i].flags & FLAG_USED) c++;
    return c;
}

bool snapshot() {
    if (!g_mounted) return false;
    uint32_t used = g_sb.free_block; // blocks [0, used)
    if (used == 0) return false;
    if (used > g_sb.snap_blocks) {
        printk("[AetherFS] snapshot too large (%u > %u blocks)\n", used, g_sb.snap_blocks);
        return false;
    }
    // Capture blocks [1, used) into the reserved snapshot region.
    for (uint32_t i = 1; i < used; ++i) {
        if (!read_block(i, g_scratch)) return false;
        if (!write_block(g_sb.snap_start + (i - 1), g_scratch)) return false;
    }
    g_sb.snap_valid = 1;
    g_sb.snap_end = used;
    return store_metadata();
}

bool snapshot_valid() { return g_mounted && g_sb.snap_valid != 0; }

bool restore() {
    if (!g_mounted || !g_sb.snap_valid) return false;
    uint32_t end = g_sb.snap_end;
    uint32_t snap_start = g_sb.snap_start;
    for (uint32_t i = 1; i < end; ++i) {
        if (!read_block(snap_start + (i - 1), g_scratch)) return false;
        if (!write_block(i, g_scratch)) return false;
    }
    // Reload the restored table + superblock from disk before rewriting the
    // superblock, otherwise the stale in-RAM table would clobber the rollback.
    if (!load_metadata()) return false;
    g_sb.free_block = end;
    g_sb.snap_valid = 0;
    return store_metadata();
}

bool set_update_lock(bool locked) {
    const char* v = locked ? "true" : "false";
    return write_file("update.lock", v, kstd::strlen(v));
}

bool update_lock_engaged() {
    if (!g_mounted) return false;
    char buf[8];
    kstd::memset(buf, 0, sizeof(buf));
    int n = read_file("update.lock", buf, 4);
    if (n <= 0) return false;
    return buf[0] == 't' && buf[1] == 'r' && buf[2] == 'u' && buf[3] == 'e';
}

uint32_t used_blocks() { return g_sb.free_block; }
uint32_t total_blocks() { return g_mounted ? g_sb.blocks_total : 0; }

} // namespace aetherfs
