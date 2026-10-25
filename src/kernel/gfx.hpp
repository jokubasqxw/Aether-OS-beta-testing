#ifndef KERNEL_GFX_HPP
#define KERNEL_GFX_HPP

#include "types.hpp"

namespace gfx {

namespace Colors {
    constexpr uint32_t Black       = 0xFF000000;
    constexpr uint32_t White       = 0xFFFFFFFF;
    constexpr uint32_t Red         = 0xFFFF3333;
    constexpr uint32_t Green       = 0xFF33CC33;
    constexpr uint32_t Blue        = 0xFF3366FF;
    constexpr uint32_t Cyan        = 0xFF00CCFF;
    constexpr uint32_t Magenta     = 0xFFFF00FF;
    constexpr uint32_t Yellow      = 0xFFFFCC00;
    constexpr uint32_t DarkGrey    = 0xFF2A2A2A;
    constexpr uint32_t Grey        = 0xFF555555;
    constexpr uint32_t LightGrey   = 0xFFAAAAAA;
    constexpr uint32_t Navy        = 0xFF0A192F;
    constexpr uint32_t Slate       = 0xFF172A45;
    constexpr uint32_t LightNavy   = 0xFF203A63;
    constexpr uint32_t Teal        = 0xFF64FFDA;
    constexpr uint32_t Orange      = 0xFFFF9900;
    constexpr uint32_t Purple      = 0xFF9933FF;

    inline constexpr uint32_t RGB(uint8_t r, uint8_t g, uint8_t b) {
        return 0xFF000000 | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }

    inline constexpr uint32_t RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
}

bool is_supported();
bool init(uint32_t width = 1920, uint32_t height = 1080);
void exit();
bool is_active();

// Hardware Driver Info
const char* get_driver_name();
bool has_hardware_double_buffering();
bool has_hardware_2d();
bool has_hardware_cursor();
void set_cursor_position(int x, int y);
void set_cursor_visible(bool visible);

uint32_t get_width();
uint32_t get_height();
uint32_t* get_frontbuffer();
uint32_t* get_backbuffer();

void set_render_target(uint32_t* target);
uint32_t* get_render_target();

void clear(uint32_t color = Colors::Black);
void swap_buffers();
void swap_rect(int x, int y, int w, int h);

void put_pixel(int x, int y, uint32_t color);
uint32_t get_pixel(int x, int y);

void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void draw_rect(int x, int y, int w, int h, uint32_t color);
void fill_rect(int x, int y, int w, int h, uint32_t color);
void draw_circle(int cx, int cy, int radius, uint32_t color);
void fill_circle(int cx, int cy, int radius, uint32_t color);
void draw_gradient_v(int x, int y, int w, int h, uint32_t top_color, uint32_t bottom_color);

void draw_char(char c, int x, int y, uint32_t fg, uint32_t bg = 0, bool transparent_bg = true);
void draw_string(const char* str, int x, int y, uint32_t fg, uint32_t bg = 0, bool transparent_bg = true);
void draw_char_scaled(char c, int x, int y, uint32_t fg, int scale);
void draw_string_scaled(const char* str, int x, int y, uint32_t fg, int scale);

// Image loading and drawing
struct Image {
    uint32_t width;
    uint32_t height;
    uint32_t* pixels;  // ARGB32 format
};

bool load_image(const char* path, Image* img);
void draw_image(const Image& img, int x, int y);
void draw_image_scaled(const Image& img, int x, int y, uint32_t w, uint32_t h);

} // namespace gfx

#endif // KERNEL_GFX_HPP
