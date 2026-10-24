#include "gfx_driver.hpp"
#include "vmware_svga.hpp"
#include "bga.hpp"
#include "vga.hpp"
#include "heap.hpp"
#include "kstring.hpp"
#include "printk.hpp"
#include "pat.hpp"

namespace gfx_driver {

static DriverInfo active_driver = {
    DriverType::None,
    "None",
    0, 0, 0, 0, 0,
    nullptr, nullptr,
    false, false, false, false
};

static uint32_t* allocated_ram_buffer = nullptr;
static bool initialized = false;

DriverType auto_detect() {
    // 1st Priority: VMware SVGA II Hardware Acceleration
    if (vmware_svga::is_available()) {
        return DriverType::VMwareSVGA;
    }

    // 2nd Priority: Bochs VBE (BGA) software backbuffer with FIFO transfer
    if (bga::is_available()) {
        return DriverType::BochsVBE;
    }

    // 3rd Priority: Fallback Standard Linear Framebuffer
    return DriverType::FallbackVBE;
}

bool is_supported() {
    return auto_detect() != DriverType::None;
}

DriverType get_driver_type() {
    return active_driver.type;
}

const char* get_driver_name() {
    return active_driver.name;
}

const DriverInfo& get_driver_info() {
    return active_driver;
}

bool init(uint32_t width, uint32_t height, uint32_t bpp) {
    DriverType best_type = auto_detect();

    if (best_type == DriverType::VMwareSVGA) {
        if (!vmware_svga::init(width, height, bpp)) {
            printk("[GFX HAL] VMware SVGA II init failed, attempting fallback to BGA...\n");
            best_type = DriverType::BochsVBE;
        } else {
            active_driver.type = DriverType::VMwareSVGA;
            active_driver.name = "VMware SVGA-II Hardware 2D Accelerator";
            active_driver.width = vmware_svga::get_width();
            active_driver.height = vmware_svga::get_height();
            active_driver.bpp = vmware_svga::get_bpp();
            active_driver.pitch = vmware_svga::get_pitch();
            active_driver.fb_phys = vmware_svga::get_framebuffer_addr();
            active_driver.frontbuffer = vmware_svga::get_framebuffer();

            // Allocate high-speed system RAM backbuffer for zero-latency compositing
            size_t buf_bytes = active_driver.width * active_driver.height * sizeof(uint32_t);
            allocated_ram_buffer = static_cast<uint32_t*>(kmalloc(buf_bytes));
            if (allocated_ram_buffer) {
                active_driver.backbuffer = allocated_ram_buffer;
                memset(active_driver.backbuffer, 0, buf_bytes);
            } else {
                active_driver.backbuffer = active_driver.frontbuffer;
            }

            active_driver.hw_double_buffer = false;
            active_driver.hw_2d_accel = true;
            active_driver.hw_cursor = vmware_svga::has_hardware_cursor();
            active_driver.write_combining = true;

            initialized = true;
            return true;
        }
    }

    if (best_type == DriverType::BochsVBE) {
        if (!bga::is_available()) {
            return false;
        }

        // Set video mode with single VRAM surface (disable hardware page-flipping panning)
        bga::set_video_mode(width, height, bpp, true, false);

        active_driver.type = DriverType::BochsVBE;
        active_driver.name = "Bochs/QEMU VBE Single-Surface LFB (PAT WC)";
        active_driver.width = bga::get_width();
        active_driver.height = bga::get_height();
        active_driver.bpp = bga::get_bpp();
        active_driver.pitch = bga::get_pitch();
        active_driver.fb_phys = bga::get_framebuffer_addr();
        active_driver.frontbuffer = bga::get_front_buffer();
        active_driver.hw_double_buffer = false;
        active_driver.hw_2d_accel = false;
        active_driver.hw_cursor = false;
        active_driver.write_combining = true;

        // Always allocate backbuffer in high-speed Write-Back cached System RAM for canonical compositing
        size_t buf_bytes = active_driver.width * active_driver.height * sizeof(uint32_t);
        allocated_ram_buffer = static_cast<uint32_t*>(kmalloc(buf_bytes));
        if (allocated_ram_buffer) {
            active_driver.backbuffer = allocated_ram_buffer;
            memset(active_driver.backbuffer, 0, buf_bytes);
        } else {
            active_driver.backbuffer = active_driver.frontbuffer;
        }

        initialized = true;
        return true;
    }

    // Fallback: unsupported
    return false;
}

void shutdown() {
    if (!initialized) return;

    if (allocated_ram_buffer) {
        kfree(allocated_ram_buffer);
        allocated_ram_buffer = nullptr;
    }

    if (active_driver.type == DriverType::VMwareSVGA) {
        vmware_svga::disable();
    } else if (active_driver.type == DriverType::BochsVBE) {
        bga::disable();
    }

    vga::restore_text_mode();

    active_driver.type = DriverType::None;
    active_driver.name = "None";
    active_driver.width = 0;
    active_driver.height = 0;
    active_driver.frontbuffer = nullptr;
    active_driver.backbuffer = nullptr;
    initialized = false;
}

uint32_t get_width() { return active_driver.width; }
uint32_t get_height() { return active_driver.height; }
uint32_t get_pitch() { return active_driver.pitch; }
uint32_t* get_frontbuffer() { return active_driver.frontbuffer; }
uint32_t* get_backbuffer() { return active_driver.backbuffer; }

void swap_buffers() {
    if (!initialized) return;

    if (active_driver.type == DriverType::VMwareSVGA) {
        if (active_driver.backbuffer && active_driver.frontbuffer && active_driver.backbuffer != active_driver.frontbuffer) {
            uint32_t* dst = active_driver.frontbuffer;
            uint32_t* src = active_driver.backbuffer;
            size_t qwords = (active_driver.width * active_driver.height * sizeof(uint32_t)) / 8;
            asm volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(qwords) : : "memory");
        }
        vmware_svga::update(0, 0, static_cast<int>(active_driver.width), static_cast<int>(active_driver.height));
        vmware_svga::sync();
        return;
    }

    // High-speed RAM backbuffer to VRAM frontbuffer full frame blit using rep movsq with PAT WC
    if (active_driver.backbuffer && active_driver.frontbuffer && active_driver.backbuffer != active_driver.frontbuffer) {
        uint32_t* dst = active_driver.frontbuffer;
        uint32_t* src = active_driver.backbuffer;
        size_t qwords = (active_driver.width * active_driver.height * sizeof(uint32_t)) / 8;
        asm volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(qwords) : : "memory");
    }
}

void swap_rect(int x, int y, int w, int h) {
    if (!initialized || w <= 0 || h <= 0) return;
    int sw = static_cast<int>(active_driver.width);
    int sh = static_cast<int>(active_driver.height);

    int x_start = x < 0 ? 0 : x;
    int y_start = y < 0 ? 0 : y;
    int x_end = (x + w) > sw ? sw : (x + w);
    int y_end = (y + h) > sh ? sh : (y + h);
    if (x_start >= x_end || y_start >= y_end) return;

    // Fast copy dirty scanlines from backbuffer to frontbuffer
    if (active_driver.backbuffer && active_driver.frontbuffer && active_driver.backbuffer != active_driver.frontbuffer) {
        size_t copy_qwords = ((x_end - x_start) * sizeof(uint32_t)) / 8;
        size_t copy_bytes_rem = ((x_end - x_start) * sizeof(uint32_t)) % 8;

        for (int cy = y_start; cy < y_end; ++cy) {
            uint32_t* dst = active_driver.frontbuffer + cy * sw + x_start;
            uint32_t* src = active_driver.backbuffer + cy * sw + x_start;
            size_t q = copy_qwords;
            asm volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(q) : : "memory");
            if (copy_bytes_rem) {
                uint8_t* d8 = reinterpret_cast<uint8_t*>(dst + q * 2);
                uint8_t* s8 = reinterpret_cast<uint8_t*>(src + q * 2);
                for (size_t b = 0; b < copy_bytes_rem; ++b) {
                    d8[b] = s8[b];
                }
            }
        }
    }

    // VMware SVGA II: Issue hardware FIFO SVGA_CMD_UPDATE
    if (active_driver.type == DriverType::VMwareSVGA) {
        vmware_svga::update(x_start, y_start, x_end - x_start, y_end - y_start);
    }
}

void clear(uint32_t color) {
    if (!initialized) return;

    if (active_driver.type == DriverType::VMwareSVGA && active_driver.backbuffer == active_driver.frontbuffer) {
        vmware_svga::rect_fill(color, 0, 0, static_cast<int>(active_driver.width), static_cast<int>(active_driver.height));
        return;
    }

    uint32_t* dst = active_driver.backbuffer ? active_driver.backbuffer : active_driver.frontbuffer;
    size_t count = active_driver.width * active_driver.height;
    asm volatile("rep stosl" : "+D"(dst), "+c"(count) : "a"(color) : "memory");
}

void fill_rect(int x, int y, int w, int h, uint32_t color, uint32_t* target) {
    if (!initialized || w <= 0 || h <= 0) return;

    if (!target) {
        target = active_driver.backbuffer ? active_driver.backbuffer : active_driver.frontbuffer;
    }

    // If writing directly to frontbuffer on VMware SVGA, use hardware 2D fill command
    if (active_driver.type == DriverType::VMwareSVGA && target == active_driver.frontbuffer) {
        vmware_svga::rect_fill(color, x, y, w, h);
        return;
    }

    int sw = static_cast<int>(active_driver.width);
    int sh = static_cast<int>(active_driver.height);
    int x_start = x < 0 ? 0 : x;
    int y_start = y < 0 ? 0 : y;
    int x_end = (x + w) > sw ? sw : (x + w);
    int y_end = (y + h) > sh ? sh : (y + h);
    if (x_start >= x_end || y_start >= y_end) return;

    size_t count = x_end - x_start;
    for (int cy = y_start; cy < y_end; ++cy) {
        uint32_t* row = target + cy * sw + x_start;
        size_t c = count;
        asm volatile("rep stosl" : "+D"(row), "+c"(c) : "a"(color) : "memory");
    }
}

void copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h, uint32_t* target) {
    if (!initialized || w <= 0 || h <= 0) return;

    if (!target) {
        target = active_driver.backbuffer ? active_driver.backbuffer : active_driver.frontbuffer;
    }

    if (active_driver.type == DriverType::VMwareSVGA && target == active_driver.frontbuffer) {
        vmware_svga::rect_copy(src_x, src_y, dst_x, dst_y, w, h);
        return;
    }

    int sw = static_cast<int>(active_driver.width);
    int sh = static_cast<int>(active_driver.height);

    if (dst_x < 0) { int d = -dst_x; src_x += d; dst_x = 0; w -= d; }
    if (dst_y < 0) { int d = -dst_y; src_y += d; dst_y = 0; h -= d; }
    if (dst_x + w > sw) w = sw - dst_x;
    if (dst_y + h > sh) h = sh - dst_y;
    if (w <= 0 || h <= 0) return;

    size_t copy_qwords = (w * sizeof(uint32_t)) / 8;
    size_t copy_rem = (w * sizeof(uint32_t)) % 8;

    if (dst_y < src_y) {
        // Forward copy
        for (int r = 0; r < h; ++r) {
            uint32_t* dst_ptr = target + (dst_y + r) * sw + dst_x;
            uint32_t* src_ptr = target + (src_y + r) * sw + src_x;
            size_t q = copy_qwords;
            asm volatile("rep movsq" : "+D"(dst_ptr), "+S"(src_ptr), "+c"(q) : : "memory");
            if (copy_rem) {
                uint8_t* d8 = reinterpret_cast<uint8_t*>(dst_ptr + q * 2);
                uint8_t* s8 = reinterpret_cast<uint8_t*>(src_ptr + q * 2);
                for (size_t b = 0; b < copy_rem; ++b) d8[b] = s8[b];
            }
        }
    } else {
        // Backward copy for overlapping vertical moves
        for (int r = h - 1; r >= 0; --r) {
            uint32_t* dst_ptr = target + (dst_y + r) * sw + dst_x;
            uint32_t* src_ptr = target + (src_y + r) * sw + src_x;
            size_t q = copy_qwords;
            asm volatile("rep movsq" : "+D"(dst_ptr), "+S"(src_ptr), "+c"(q) : : "memory");
            if (copy_rem) {
                uint8_t* d8 = reinterpret_cast<uint8_t*>(dst_ptr + q * 2);
                uint8_t* s8 = reinterpret_cast<uint8_t*>(src_ptr + q * 2);
                for (size_t b = 0; b < copy_rem; ++b) d8[b] = s8[b];
            }
        }
    }
}

bool has_hardware_cursor() {
    return active_driver.hw_cursor;
}

void set_cursor_position(int x, int y) {
    if (active_driver.type == DriverType::VMwareSVGA) {
        vmware_svga::set_cursor_position(x, y);
    }
}

void set_cursor_visible(bool visible) {
    if (active_driver.type == DriverType::VMwareSVGA) {
        vmware_svga::set_cursor_visible(visible);
    }
}

void define_cursor(int width, int height, int hotspot_x, int hotspot_y, const uint32_t* rgba_data) {
    if (active_driver.type == DriverType::VMwareSVGA) {
        vmware_svga::define_alpha_cursor(width, height, hotspot_x, hotspot_y, rgba_data);
    }
}

} // namespace gfx_driver
