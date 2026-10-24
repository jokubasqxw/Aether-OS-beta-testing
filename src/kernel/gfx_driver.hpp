#ifndef KERNEL_GFX_DRIVER_HPP
#define KERNEL_GFX_DRIVER_HPP

#include "types.hpp"

namespace gfx_driver {

enum class DriverType {
    None = 0,
    VMwareSVGA,
    BochsVBE,
    FallbackVBE
};

struct DriverInfo {
    DriverType type;
    const char* name;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uintptr_t fb_phys;
    uint32_t* frontbuffer;
    uint32_t* backbuffer;
    bool hw_double_buffer;
    bool hw_2d_accel;
    bool hw_cursor;
    bool write_combining;
};

// Subsystem Lifecycle & Auto-Detection
bool is_supported();
DriverType auto_detect();
DriverType get_driver_type();
const char* get_driver_name();
const DriverInfo& get_driver_info();

bool init(uint32_t width = 1920, uint32_t height = 1080, uint32_t bpp = 32);
void shutdown();

// Screen Dimensions & Buffers
uint32_t get_width();
uint32_t get_height();
uint32_t get_pitch();
uint32_t* get_frontbuffer();
uint32_t* get_backbuffer();

// Frame Swapping & Rect Flushing
void swap_buffers();
void swap_rect(int x, int y, int w, int h);

// Hardware Accelerated 2D Operations
void clear(uint32_t color);
void fill_rect(int x, int y, int w, int h, uint32_t color, uint32_t* target = nullptr);
void copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h, uint32_t* target = nullptr);

// Hardware Cursor Controls
bool has_hardware_cursor();
void set_cursor_position(int x, int y);
void set_cursor_visible(bool visible);
void define_cursor(int width, int height, int hotspot_x, int hotspot_y, const uint32_t* rgba_data);

} // namespace gfx_driver

#endif // KERNEL_GFX_DRIVER_HPP
