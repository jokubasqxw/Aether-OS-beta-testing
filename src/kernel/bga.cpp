#include "bga.hpp"
#include "pci.hpp"
#include "printk.hpp"
#include "pat.hpp"
#include "../arch/x86_64/io.hpp"

namespace bga {

static uint32_t current_width = 0;
static uint32_t current_height = 0;
static uint32_t current_bpp = 0;
static uintptr_t framebuffer_phys_addr = 0;
static size_t vram_size_bytes = 32 * 1024 * 1024; // Default 32MB VRAM

static bool hardware_double_buffering = false;
static int current_page = 0; // 0 or 1

// Optimized I/O Register Cache to avoid redundant port cycles
constexpr size_t REG_CACHE_SIZE = 16;
static uint16_t reg_cache[REG_CACHE_SIZE];
static bool reg_cached[REG_CACHE_SIZE] = { false };

void flush_register_cache() {
    for (size_t i = 0; i < REG_CACHE_SIZE; ++i) {
        reg_cached[i] = false;
    }
}

static inline void bga_write(uint16_t index, uint16_t value) {
    if (index < REG_CACHE_SIZE && reg_cached[index] && reg_cache[index] == value) {
        return; // Cache hit: avoid slow I/O port write
    }

    arch::outw(VBE_DISPI_IOPORT_INDEX, index);
    arch::outw(VBE_DISPI_IOPORT_DATA, value);

    if (index < REG_CACHE_SIZE) {
        reg_cache[index] = value;
        reg_cached[index] = true;
    }
}

static inline uint16_t bga_read(uint16_t index) {
    arch::outw(VBE_DISPI_IOPORT_INDEX, index);
    uint16_t val = arch::inw(VBE_DISPI_IOPORT_DATA);

    if (index < REG_CACHE_SIZE) {
        reg_cache[index] = val;
        reg_cached[index] = true;
    }
    return val;
}

static void find_framebuffer_pci() {
    if (framebuffer_phys_addr != 0) return;

    for (uint16_t bus = 0; bus < 8; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            uint32_t val = pci::read_config_dword(static_cast<uint8_t>(bus), slot, 0, 0);
            uint16_t vid = static_cast<uint16_t>(val & 0xFFFF);
            if (vid == 0xFFFF || vid == 0x0000) continue;

            uint32_t rev_class = pci::read_config_dword(static_cast<uint8_t>(bus), slot, 0, 8);
            uint8_t class_id = static_cast<uint8_t>((rev_class >> 24) & 0xFF);

            // QEMU BGA vendor 0x1234 or Display Controller class 0x03
            if (vid == 0x1234 || class_id == 0x03) {
                // If it's VMware (0x15AD), don't treat it as standard BGA
                if (vid == 0x15AD) continue;

                uint32_t bar0 = pci::read_config_dword(static_cast<uint8_t>(bus), slot, 0, 0x10);
                framebuffer_phys_addr = static_cast<uintptr_t>(bar0 & 0xFFFFFFF0);
                vram_size_bytes = 32 * 1024 * 1024;
                return;
            }
        }
    }

    // Default fallback in standard QEMU
    framebuffer_phys_addr = 0xFD000000;
    vram_size_bytes = 32 * 1024 * 1024;
}

bool is_available() {
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    return (id >= 0xB0C0 && id <= 0xB0C6);
}

uint32_t get_version() {
    return bga_read(VBE_DISPI_INDEX_ID);
}

uintptr_t get_framebuffer_addr() {
    if (framebuffer_phys_addr == 0) {
        find_framebuffer_pci();
    }
    return framebuffer_phys_addr;
}

size_t get_vram_size() {
    return vram_size_bytes;
}

void set_video_mode(uint32_t width, uint32_t height, uint32_t bpp, bool clear, bool enable_double_buffering) {
    find_framebuffer_pci();
    flush_register_cache();

    // 1. Disable display while programming registers
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);

    // 2. Set base video resolution
    bga_write(VBE_DISPI_INDEX_XRES, static_cast<uint16_t>(width));
    bga_write(VBE_DISPI_INDEX_YRES, static_cast<uint16_t>(height));
    bga_write(VBE_DISPI_INDEX_BPP, static_cast<uint16_t>(bpp));

    // 3. Configure Hardware Double Buffering via Y-Offset Panning
    if (enable_double_buffering) {
        // Set virtual width and double virtual height (2 * height)
        bga_write(VBE_DISPI_INDEX_VIRT_WIDTH, static_cast<uint16_t>(width));
        bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, static_cast<uint16_t>(height * 2));
        bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
        bga_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    } else {
        bga_write(VBE_DISPI_INDEX_VIRT_WIDTH, static_cast<uint16_t>(width));
        bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, static_cast<uint16_t>(height));
        bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
        bga_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    }

    // 4. Enable Linear Framebuffer (LFB) mode
    uint16_t enable_flags = VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED;
    if (!clear) {
        enable_flags |= VBE_DISPI_NOCLEARMEM;
    }
    bga_write(VBE_DISPI_INDEX_ENABLE, enable_flags);

    // 5. Query back virtual height to verify hardware capability
    uint16_t confirmed_v_height = bga_read(VBE_DISPI_INDEX_VIRT_HEIGHT);
    if (enable_double_buffering && confirmed_v_height >= static_cast<uint16_t>(height * 2)) {
        hardware_double_buffering = true;
    } else {
        hardware_double_buffering = false;
        bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, static_cast<uint16_t>(height));
    }

    current_width = width;
    current_height = height;
    current_bpp = bpp;
    current_page = 0;

    // 6. Ensure PAT Write-Combining is active for entire 32MB VRAM
    pat::set_range_wc(framebuffer_phys_addr, vram_size_bytes);

    // 7. Clear buffers if requested
    if (clear) {
        size_t page_pixels = width * height;
        uint32_t* p0 = get_page_buffer(0);
        if (p0) {
            asm volatile("rep stosl" : "+D"(p0), "+c"(page_pixels) : "a"(0xFF000000) : "memory");
        }
        if (hardware_double_buffering) {
            uint32_t* p1 = get_page_buffer(1);
            if (p1) {
                asm volatile("rep stosl" : "+D"(p1), "+c"(page_pixels) : "a"(0xFF000000) : "memory");
            }
        }
    }
}

void disable() {
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    flush_register_cache();
    current_width = 0;
    current_height = 0;
    current_bpp = 0;
    current_page = 0;
    hardware_double_buffering = false;
}

uint32_t get_width() { return current_width; }
uint32_t get_height() { return current_height; }
uint32_t get_bpp() { return current_bpp; }
uint32_t get_pitch() { return current_width * (current_bpp / 8); }

bool has_hardware_double_buffering() {
    return hardware_double_buffering;
}

int get_current_page() {
    return current_page;
}

void flip_page(int page) {
    if (!hardware_double_buffering || current_height == 0) return;

    page = (page != 0) ? 1 : 0;
    uint16_t y_offset = static_cast<uint16_t>(page * current_height);

    // Zero-copy page flip: exactly 1 register write to hardware display engine!
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, y_offset);
    current_page = page;
}

void swap_buffers() {
    if (hardware_double_buffering) {
        flip_page(1 - current_page);
    }
}

uint32_t* get_page_buffer(int page) {
    if (framebuffer_phys_addr == 0) return nullptr;
    page = (page != 0) ? 1 : 0;
    size_t page_offset_bytes = static_cast<size_t>(page) * current_width * current_height * (current_bpp / 8);
    return reinterpret_cast<uint32_t*>(framebuffer_phys_addr + page_offset_bytes);
}

uint32_t* get_front_buffer() {
    return get_page_buffer(current_page);
}

uint32_t* get_back_buffer() {
    if (hardware_double_buffering) {
        return get_page_buffer(1 - current_page);
    }
    return get_front_buffer();
}

void set_y_offset(uint16_t y_offset) {
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, y_offset);
}

uint16_t get_y_offset() {
    return bga_read(VBE_DISPI_INDEX_Y_OFFSET);
}

void set_x_offset(uint16_t x_offset) {
    bga_write(VBE_DISPI_INDEX_X_OFFSET, x_offset);
}

uint16_t get_x_offset() {
    return bga_read(VBE_DISPI_INDEX_X_OFFSET);
}

} // namespace bga
