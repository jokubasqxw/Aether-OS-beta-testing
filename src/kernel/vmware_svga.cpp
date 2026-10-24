#include "vmware_svga.hpp"
#include "pci.hpp"
#include "printk.hpp"
#include "pat.hpp"
#include "../arch/x86_64/io.hpp"

namespace vmware_svga {

static bool device_detected = false;
static pci::Device svga_pci_dev;
static uint16_t io_base = 0;
static uintptr_t fb_phys_addr = 0;
static uintptr_t fifo_phys_addr = 0;
static size_t fb_size = 0;
static size_t fifo_size = 0;
static volatile uint32_t* fifo_base = nullptr;

static uint32_t current_width = 0;
static uint32_t current_height = 0;
static uint32_t current_bpp = 0;
static uint32_t capabilities = 0;
static uint32_t negotiated_id = 0;

uint32_t read_reg(uint32_t index) {
    if (io_base == 0) return 0;
    arch::outl(io_base + 0, index);
    return arch::inl(io_base + 1);
}

void write_reg(uint32_t index, uint32_t val) {
    if (io_base == 0) return;
    arch::outl(io_base + 0, index);
    arch::outl(io_base + 1, val);
}

bool is_available() {
    if (device_detected) return true;

    pci::Device dev;
    if (pci::find_device(PCI_VENDOR_VMWARE, PCI_DEVICE_SVGA2, &dev)) {
        svga_pci_dev = dev;
        device_detected = true;
        return true;
    }
    return false;
}

bool init(uint32_t width, uint32_t height, uint32_t bpp) {
    if (!is_available()) {
        return false;
    }

    // 1. Enable Bus Mastering and IO/Memory Decode on PCI Device
    pci::enable_bus_mastering(svga_pci_dev);

    // 2. Map PCI Base Address Registers (BARs)
    uint32_t bar0 = pci::get_bar(svga_pci_dev, 0); // BAR0: I/O Ports
    uint32_t bar1 = pci::get_bar(svga_pci_dev, 1); // BAR1: Framebuffer / VRAM
    uint32_t bar2 = pci::get_bar(svga_pci_dev, 2); // BAR2: FIFO Memory Buffer

    io_base = static_cast<uint16_t>(bar0 & ~0x3);
    fb_phys_addr = static_cast<uintptr_t>(bar1 & ~0xF);
    fifo_phys_addr = static_cast<uintptr_t>(bar2 & ~0xF);

    if (io_base == 0 || fb_phys_addr == 0 || fifo_phys_addr == 0) {
        printk("[VMware SVGA] Error: Invalid PCI BAR addresses (IO: 0x%x, FB: %p, FIFO: %p)\n",
               io_base, fb_phys_addr, fifo_phys_addr);
        return false;
    }

    // 3. Negotiate SVGA Device Version / ID
    write_reg(SVGA_REG_ID, SVGA_ID_2);
    negotiated_id = read_reg(SVGA_REG_ID);

    if (negotiated_id != SVGA_ID_2) {
        // Fallback to SVGA Version 1
        write_reg(SVGA_REG_ID, SVGA_ID_1);
        negotiated_id = read_reg(SVGA_REG_ID);
        if (negotiated_id != SVGA_ID_1) {
            printk("[VMware SVGA] Error: Version negotiation failed (Got 0x%x)\n", negotiated_id);
            return false;
        }
    }

    // 4. Query Hardware Capabilities & Physical Sizes
    capabilities = read_reg(SVGA_REG_CAPABILITIES);

    uint32_t hw_fb_start = read_reg(SVGA_REG_FB_START);
    if (hw_fb_start != 0) fb_phys_addr = hw_fb_start;

    fb_size = read_reg(SVGA_REG_FB_SIZE);
    if (fb_size == 0) fb_size = read_reg(SVGA_REG_VRAM_SIZE);
    if (fb_size == 0) fb_size = 32 * 1024 * 1024; // Default fallback to 32MB

    uint32_t hw_fifo_start = read_reg(SVGA_REG_MEM_START);
    if (hw_fifo_start != 0) fifo_phys_addr = hw_fifo_start;

    fifo_size = read_reg(SVGA_REG_MEM_SIZE);
    if (fifo_size == 0) fifo_size = 256 * 1024; // Default fallback to 256KB

    fifo_base = reinterpret_cast<volatile uint32_t*>(fifo_phys_addr);

    // 5. Configure IA32_PAT Write-Combining on Framebuffer & FIFO regions
    pat::set_range_wc(fb_phys_addr, fb_size);
    pat::set_range_wc(fifo_phys_addr, 2 * 1024 * 1024);

    // 6. Set Video Resolution & Color Depth
    write_reg(SVGA_REG_ENABLE, 0);
    write_reg(SVGA_REG_WIDTH, width);
    write_reg(SVGA_REG_HEIGHT, height);
    write_reg(SVGA_REG_BITS_PER_PIXEL, bpp);
    write_reg(SVGA_REG_ENABLE, 1);

    current_width = width;
    current_height = height;
    current_bpp = bpp;

    // 7. Initialize VMware SVGA Command FIFO
    uint32_t min = SVGA_FIFO_NUM_REGS * sizeof(uint32_t);
    uint32_t max = static_cast<uint32_t>(fifo_size);

    fifo_base[SVGA_FIFO_MIN] = min;
    fifo_base[SVGA_FIFO_MAX] = max;
    fifo_base[SVGA_FIFO_NEXT_CMD] = min;
    fifo_base[SVGA_FIFO_STOP] = min;

    asm volatile("mfence" ::: "memory");
    write_reg(SVGA_REG_CONFIG_DONE, 1);

    // 8. Clear Framebuffer to solid dark/black and sync
    uint32_t* fb = get_framebuffer();
    if (fb) {
        size_t count = width * height;
        asm volatile("rep stosl" : "+D"(fb), "+c"(count) : "a"(0xFF000000) : "memory");
    }
    update(0, 0, static_cast<int>(width), static_cast<int>(height));
    sync();

    printk("[VMware SVGA] Initialized SVGA-II Hardware Driver (%ux%u @ %ubpp, Caps: 0x%x)\n",
           width, height, bpp, capabilities);
    return true;
}

void disable() {
    if (io_base != 0) {
        write_reg(SVGA_REG_ENABLE, 0);
    }
    current_width = 0;
    current_height = 0;
    current_bpp = 0;
}

uint32_t get_width() { return current_width; }
uint32_t get_height() { return current_height; }
uint32_t get_bpp() { return current_bpp; }
uint32_t get_pitch() { return current_width * (current_bpp / 8); }
uint32_t get_capabilities() { return capabilities; }
uintptr_t get_framebuffer_addr() { return fb_phys_addr; }
uint32_t* get_framebuffer() { return reinterpret_cast<uint32_t*>(fb_phys_addr); }
size_t get_framebuffer_size() { return fb_size; }
uintptr_t get_fifo_addr() { return fifo_phys_addr; }
size_t get_fifo_size() { return fifo_size; }

static void fifo_write_cmd(const uint32_t* cmd_words, size_t count) {
    if (!fifo_base || count == 0) return;

    volatile uint32_t* fifo = fifo_base;
    uint32_t min = fifo[SVGA_FIFO_MIN];
    uint32_t max = fifo[SVGA_FIFO_MAX];
    uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t stop = fifo[SVGA_FIFO_STOP];

    // Compute free space in FIFO ring buffer
    uint32_t free_bytes = 0;
    if (next >= stop) {
        free_bytes = (max - next) + (stop - min) - sizeof(uint32_t);
    } else {
        free_bytes = (stop - next) - sizeof(uint32_t);
    }

    size_t bytes_needed = count * sizeof(uint32_t);
    if (free_bytes < bytes_needed) {
        sync();
        next = fifo[SVGA_FIFO_NEXT_CMD];
    }

    // Write command packet with circular ring-buffer boundary wrapping
    for (size_t i = 0; i < count; ++i) {
        *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<volatile uint8_t*>(fifo) + next) = cmd_words[i];
        next += sizeof(uint32_t);
        if (next >= max) {
            next = min;
        }
    }

    // Ensure memory store order before committing write pointer
    asm volatile("mfence" ::: "memory");
    fifo[SVGA_FIFO_NEXT_CMD] = next;

    // Signal host device to process FIFO
    write_reg(SVGA_REG_SYNC, 1);
}

void update(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int sw = static_cast<int>(current_width);
    int sh = static_cast<int>(current_height);

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;

    uint32_t cmd[5] = {
        SVGA_CMD_UPDATE,
        static_cast<uint32_t>(x),
        static_cast<uint32_t>(y),
        static_cast<uint32_t>(w),
        static_cast<uint32_t>(h)
    };
    fifo_write_cmd(cmd, 5);
}

static bool fifo_fill_enabled = false;

void set_fifo_fill_enabled(bool enabled) {
    fifo_fill_enabled = enabled;
}

bool is_fifo_fill_enabled() {
    return fifo_fill_enabled;
}

void fifo_rect_fill(uint32_t color, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int sw = static_cast<int>(current_width);
    int sh = static_cast<int>(current_height);

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;

    uint32_t cmd[6] = {
        SVGA_CMD_RECT_FILL,
        color,
        static_cast<uint32_t>(x),
        static_cast<uint32_t>(y),
        static_cast<uint32_t>(w),
        static_cast<uint32_t>(h)
    };
    fifo_write_cmd(cmd, 6);
}

void fifo_rect_copy(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int sw = static_cast<int>(current_width);
    int sh = static_cast<int>(current_height);

    if (dst_x < 0) { int d = -dst_x; src_x += d; dst_x = 0; w -= d; }
    if (dst_y < 0) { int d = -dst_y; src_y += d; dst_y = 0; h -= d; }
    if (dst_x + w > sw) w = sw - dst_x;
    if (dst_y + h > sh) h = sh - dst_y;
    if (w <= 0 || h <= 0) return;

    uint32_t cmd[7] = {
        SVGA_CMD_RECT_COPY,
        static_cast<uint32_t>(src_x),
        static_cast<uint32_t>(src_y),
        static_cast<uint32_t>(dst_x),
        static_cast<uint32_t>(dst_y),
        static_cast<uint32_t>(w),
        static_cast<uint32_t>(h)
    };
    fifo_write_cmd(cmd, 7);
}

void rect_fill(uint32_t color, int x, int y, int w, int h) {
    if (fifo_fill_enabled) {
        fifo_rect_fill(color, x, y, w, h);
    } else {
        uint32_t* fb = get_framebuffer();
        int sw = static_cast<int>(current_width);
        int sh = static_cast<int>(current_height);

        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > sw) w = sw - x;
        if (y + h > sh) h = sh - y;
        if (w <= 0 || h <= 0) return;

        if (fb) {
            size_t count = w;
            for (int cy = y; cy < y + h; ++cy) {
                uint32_t* row = fb + cy * sw + x;
                size_t c = count;
                asm volatile("rep stosl" : "+D"(row), "+c"(c) : "a"(color) : "memory");
            }
        }
        update(x, y, w, h);
    }
}

void rect_copy(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    if (fifo_fill_enabled) {
        fifo_rect_copy(src_x, src_y, dst_x, dst_y, w, h);
    } else {
        uint32_t* fb = get_framebuffer();
        int sw = static_cast<int>(current_width);
        int sh = static_cast<int>(current_height);

        if (dst_x < 0) { int d = -dst_x; src_x += d; dst_x = 0; w -= d; }
        if (dst_y < 0) { int d = -dst_y; src_y += d; dst_y = 0; h -= d; }
        if (dst_x + w > sw) w = sw - dst_x;
        if (dst_y + h > sh) h = sh - dst_y;
        if (w <= 0 || h <= 0) return;

        if (fb) {
            size_t copy_qwords = (w * sizeof(uint32_t)) / 8;
            size_t copy_rem = (w * sizeof(uint32_t)) % 8;

            if (dst_y < src_y) {
                for (int r = 0; r < h; ++r) {
                    uint32_t* d_ptr = fb + (dst_y + r) * sw + dst_x;
                    uint32_t* s_ptr = fb + (src_y + r) * sw + src_x;
                    size_t q = copy_qwords;
                    asm volatile("rep movsq" : "+D"(d_ptr), "+S"(s_ptr), "+c"(q) : : "memory");
                    if (copy_rem) {
                        uint8_t* d8 = reinterpret_cast<uint8_t*>(d_ptr + q * 2);
                        uint8_t* s8 = reinterpret_cast<uint8_t*>(s_ptr + q * 2);
                        for (size_t b = 0; b < copy_rem; ++b) d8[b] = s8[b];
                    }
                }
            } else {
                for (int r = h - 1; r >= 0; --r) {
                    uint32_t* d_ptr = fb + (dst_y + r) * sw + dst_x;
                    uint32_t* s_ptr = fb + (src_y + r) * sw + src_x;
                    size_t q = copy_qwords;
                    asm volatile("rep movsq" : "+D"(d_ptr), "+S"(s_ptr), "+c"(q) : : "memory");
                    if (copy_rem) {
                        uint8_t* d8 = reinterpret_cast<uint8_t*>(d_ptr + q * 2);
                        uint8_t* s8 = reinterpret_cast<uint8_t*>(s_ptr + q * 2);
                        for (size_t b = 0; b < copy_rem; ++b) d8[b] = s8[b];
                    }
                }
            }
        }
        update(dst_x, dst_y, w, h);
    }
}

void sync() {
    if (io_base == 0) return;
    write_reg(SVGA_REG_SYNC, 1);
    while (read_reg(SVGA_REG_BUSY) != 0) {
        asm volatile("pause");
    }
}

bool has_hardware_cursor() {
    return (capabilities & (SVGA_CAP_CURSOR | SVGA_CAP_CURSOR_BYPASS | SVGA_CAP_CURSOR_BYPASS_2)) != 0;
}

void set_cursor_position(int x, int y) {
    if (io_base == 0) return;
    write_reg(SVGA_REG_CURSOR_X, static_cast<uint32_t>(x));
    write_reg(SVGA_REG_CURSOR_Y, static_cast<uint32_t>(y));
}

void set_cursor_visible(bool visible) {
    if (io_base == 0) return;
    write_reg(SVGA_REG_CURSOR_ON, visible ? 1 : 0);
}

void define_alpha_cursor(int width, int height, int hotspot_x, int hotspot_y, const uint32_t* rgba_data) {
    if (!has_hardware_cursor() || !rgba_data) return;

    // Use SVGA_CMD_DEFINE_ALPHA_CURSOR via FIFO
    size_t pixel_count = width * height;

    // Write header
    uint32_t header[6] = {
        SVGA_CMD_DEFINE_ALPHA_CURSOR,
        0, // Cursor ID
        static_cast<uint32_t>(hotspot_x),
        static_cast<uint32_t>(hotspot_y),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height)
    };

    // Push header and pixel data
    fifo_write_cmd(header, 6);
    fifo_write_cmd(rgba_data, pixel_count);
    sync();
}

} // namespace vmware_svga
