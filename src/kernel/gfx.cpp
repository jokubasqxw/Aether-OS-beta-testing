#include "gfx.hpp"
#include "gfx_driver.hpp"
#include "font.hpp"
#include "kstring.hpp"

namespace gfx {

static uint32_t screen_width = 0;
static uint32_t screen_height = 0;
static uint32_t* frontbuffer = nullptr;
static uint32_t* backbuffer = nullptr;
static uint32_t* active_target = nullptr;
static bool active = false;

static inline int abs_val(int v) {
    return v < 0 ? -v : v;
}

bool is_supported() {
    return gfx_driver::is_supported();
}

bool is_active() {
    return active;
}

const char* get_driver_name() {
    return gfx_driver::get_driver_name();
}

bool has_hardware_double_buffering() {
    return gfx_driver::get_driver_info().hw_double_buffer;
}

bool has_hardware_2d() {
    return gfx_driver::get_driver_info().hw_2d_accel;
}

bool has_hardware_cursor() {
    return gfx_driver::has_hardware_cursor();
}

void set_cursor_position(int x, int y) {
    gfx_driver::set_cursor_position(x, y);
}

void set_cursor_visible(bool visible) {
    gfx_driver::set_cursor_visible(visible);
}

uint32_t get_width() { return screen_width; }
uint32_t get_height() { return screen_height; }
uint32_t* get_frontbuffer() { return frontbuffer; }

uint32_t* get_backbuffer() {
    if (active_target) return active_target;
    return backbuffer ? backbuffer : frontbuffer;
}

void set_render_target(uint32_t* target) {
    active_target = target;
}

uint32_t* get_render_target() {
    return active_target ? active_target : get_backbuffer();
}

bool init(uint32_t width, uint32_t height) {
    if (!gfx_driver::init(width, height, 32)) {
        return false;
    }

    screen_width = gfx_driver::get_width();
    screen_height = gfx_driver::get_height();
    frontbuffer = gfx_driver::get_frontbuffer();
    backbuffer = gfx_driver::get_backbuffer();

    active_target = nullptr;
    active = true;
    return true;
}

void exit() {
    if (!active) return;
    gfx_driver::shutdown();
    active_target = nullptr;
    frontbuffer = nullptr;
    backbuffer = nullptr;
    screen_width = 0;
    screen_height = 0;
    active = false;
}

void clear(uint32_t color) {
    uint32_t* dst = get_render_target();
    if (dst == frontbuffer && gfx_driver::get_driver_info().hw_2d_accel) {
        gfx_driver::clear(color);
        return;
    }

    size_t count = screen_width * screen_height;
    asm volatile(
        "rep stosl"
        : "+D"(dst), "+c"(count)
        : "a"(color)
        : "memory"
    );
}

void swap_buffers() {
    gfx_driver::swap_buffers();
    frontbuffer = gfx_driver::get_frontbuffer();
    backbuffer = gfx_driver::get_backbuffer();
}

void swap_rect(int x, int y, int w, int h) {
    gfx_driver::swap_rect(x, y, w, h);
}

void put_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < static_cast<int>(screen_width) &&
        y >= 0 && y < static_cast<int>(screen_height)) {
        uint32_t* buf = get_render_target();
        buf[y * screen_width + x] = color;
    }
}

uint32_t get_pixel(int x, int y) {
    if (x >= 0 && x < static_cast<int>(screen_width) &&
        y >= 0 && y < static_cast<int>(screen_height)) {
        uint32_t* buf = get_render_target();
        return buf[y * screen_width + x];
    }
    return 0;
}

void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int sw = static_cast<int>(screen_width);
    int sh = static_cast<int>(screen_height);
    uint32_t* buf = get_render_target();

    // Fast horizontal line using hardware rep stosl
    if (y0 == y1) {
        if (y0 < 0 || y0 >= sh) return;
        int min_x = x0 < x1 ? x0 : x1;
        int max_x = x0 > x1 ? x0 : x1;
        if (min_x < 0) min_x = 0;
        if (max_x >= sw) max_x = sw - 1;
        if (min_x <= max_x) {
            uint32_t* row = buf + y0 * sw + min_x;
            size_t count = max_x - min_x + 1;
            asm volatile("rep stosl" : "+D"(row), "+c"(count) : "a"(color) : "memory");
        }
        return;
    }

    // Fast vertical line
    if (x0 == x1) {
        if (x0 < 0 || x0 >= sw) return;
        int min_y = y0 < y1 ? y0 : y1;
        int max_y = y0 > y1 ? y0 : y1;
        if (min_y < 0) min_y = 0;
        if (max_y >= sh) max_y = sh - 1;
        uint32_t* ptr = buf + min_y * sw + x0;
        for (int y = min_y; y <= max_y; ++y) {
            *ptr = color;
            ptr += sw;
        }
        return;
    }

    // General Bresenham's algorithm with bounds checking
    int dx = abs_val(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs_val(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        if (x0 >= 0 && x0 < sw && y0 >= 0 && y0 < sh) {
            buf[y0 * sw + x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    draw_line(x, y, x + w - 1, y, color);
    draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    draw_line(x, y, x, y + h - 1, color);
    draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    uint32_t* target = get_render_target();

    if (target == frontbuffer && gfx_driver::get_driver_info().hw_2d_accel) {
        gfx_driver::fill_rect(x, y, w, h, color, frontbuffer);
        return;
    }

    int x_start = x < 0 ? 0 : x;
    int y_start = y < 0 ? 0 : y;
    int x_end = (x + w) > static_cast<int>(screen_width) ? screen_width : (x + w);
    int y_end = (y + h) > static_cast<int>(screen_height) ? screen_height : (y + h);
    if (x_start >= x_end || y_start >= y_end) return;

    size_t count = x_end - x_start;
    for (int cy = y_start; cy < y_end; ++cy) {
        uint32_t* row = target + cy * screen_width + x_start;
        size_t c = count;
        asm volatile(
            "rep stosl"
            : "+D"(row), "+c"(c)
            : "a"(color)
            : "memory"
        );
    }
}

void draw_circle(int cx, int cy, int radius, uint32_t color) {
    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        put_pixel(cx + x, cy + y, color);
        put_pixel(cx + y, cy + x, color);
        put_pixel(cx - y, cy + x, color);
        put_pixel(cx - x, cy + y, color);
        put_pixel(cx - x, cy - y, color);
        put_pixel(cx - y, cy - x, color);
        put_pixel(cx + y, cy - x, color);
        put_pixel(cx + x, cy - y, color);

        if (err <= 0) {
            y += 1;
            err += 2 * y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

void fill_circle(int cx, int cy, int radius, uint32_t color) {
    int sw = static_cast<int>(screen_width);
    int sh = static_cast<int>(screen_height);
    uint32_t* buf = get_render_target();
    int r2 = radius * radius;

    for (int dy = -radius; dy <= radius; ++dy) {
        int py = cy + dy;
        if (py < 0 || py >= sh) continue;

        int dx_max = 0;
        while ((dx_max + 1) * (dx_max + 1) + dy * dy <= r2) {
            dx_max++;
        }

        int x_start = cx - dx_max;
        int x_end = cx + dx_max;
        if (x_start < 0) x_start = 0;
        if (x_end >= sw) x_end = sw - 1;

        if (x_start <= x_end) {
            uint32_t* row = buf + py * sw + x_start;
            size_t count = x_end - x_start + 1;
            asm volatile("rep stosl" : "+D"(row), "+c"(count) : "a"(color) : "memory");
        }
    }
}

void draw_gradient_v(int x, int y, int w, int h, uint32_t top_color, uint32_t bottom_color) {
    if (w <= 0 || h <= 0) return;

    int x_start = x < 0 ? 0 : x;
    int y_start = y < 0 ? 0 : y;
    int x_end = (x + w) > static_cast<int>(screen_width) ? screen_width : (x + w);
    int y_end = (y + h) > static_cast<int>(screen_height) ? screen_height : (y + h);
    if (x_start >= x_end || y_start >= y_end) return;

    size_t count = x_end - x_start;
    uint32_t* buf = get_render_target();

    int r1 = (top_color >> 16) & 0xFF;
    int g1 = (top_color >> 8) & 0xFF;
    int b1 = top_color & 0xFF;

    int r2 = (bottom_color >> 16) & 0xFF;
    int g2 = (bottom_color >> 8) & 0xFF;
    int b2 = bottom_color & 0xFF;

    int dr = r2 - r1;
    int dg = g2 - g1;
    int db = b2 - b1;

    for (int row = y_start - y; row < y_end - y; ++row) {
        int r = r1 + (dr * row) / h;
        int g = g1 + (dg * row) / h;
        int b = b1 + (db * row) / h;
        uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;

        int cy = y + row;
        uint32_t* row_dst = buf + cy * screen_width + x_start;
        size_t c = count;
        asm volatile(
            "rep stosl"
            : "+D"(row_dst), "+c"(c)
            : "a"(color)
            : "memory"
        );
    }
}

void draw_char(char c, int x, int y, uint32_t fg, uint32_t bg, bool transparent_bg) {
    if (static_cast<unsigned char>(c) >= 128) return;
    if (x < 0 || x + 8 > static_cast<int>(screen_width) ||
        y < 0 || y + 16 > static_cast<int>(screen_height)) return;

    const uint8_t* glyph = font::FONT_8X16[static_cast<uint8_t>(c)];
    uint32_t* buf = get_render_target();

    for (int row = 0; row < 16; ++row) {
        uint8_t bits = glyph[row];
        uint32_t* row_ptr = buf + (y + row) * screen_width + x;
        if (transparent_bg) {
            if (bits & 0x80) row_ptr[0] = fg;
            if (bits & 0x40) row_ptr[1] = fg;
            if (bits & 0x20) row_ptr[2] = fg;
            if (bits & 0x10) row_ptr[3] = fg;
            if (bits & 0x08) row_ptr[4] = fg;
            if (bits & 0x04) row_ptr[5] = fg;
            if (bits & 0x02) row_ptr[6] = fg;
            if (bits & 0x01) row_ptr[7] = fg;
        } else {
            row_ptr[0] = (bits & 0x80) ? fg : bg;
            row_ptr[1] = (bits & 0x40) ? fg : bg;
            row_ptr[2] = (bits & 0x20) ? fg : bg;
            row_ptr[3] = (bits & 0x10) ? fg : bg;
            row_ptr[4] = (bits & 0x08) ? fg : bg;
            row_ptr[5] = (bits & 0x04) ? fg : bg;
            row_ptr[6] = (bits & 0x02) ? fg : bg;
            row_ptr[7] = (bits & 0x01) ? fg : bg;
        }
    }
}

void draw_string(const char* str, int x, int y, uint32_t fg, uint32_t bg, bool transparent_bg) {
    if (!str) return;
    int curr_x = x;
    int curr_y = y;

    while (*str) {
        if (*str == '\n') {
            curr_x = x;
            curr_y += 18;
        } else if (*str == '\t') {
            curr_x += 4 * 8;
        } else {
            draw_char(*str, curr_x, curr_y, fg, bg, transparent_bg);
            curr_x += 8;
        }
        str++;
    }
}

void draw_char_scaled(char c, int x, int y, uint32_t fg, int scale) {
    if (static_cast<unsigned char>(c) >= 128) return;
    if (scale <= 1) {
        draw_char(c, x, y, fg);
        return;
    }

    const uint8_t* glyph = font::FONT_8X16[static_cast<uint8_t>(c)];

    for (int row = 0; row < 16; ++row) {
        uint8_t bits = glyph[row];
        int col = 0;
        while (col < 8) {
            if (!(bits & (0x80 >> col))) {
                ++col;
                continue;
            }
            int run = 1;
            while (col + run < 8 && (bits & (0x80 >> (col + run)))) ++run;
            fill_rect(x + col * scale, y + row * scale, run * scale, scale, fg);
            col += run;
        }
    }
}

void draw_string_scaled(const char* str, int x, int y, uint32_t fg, int scale) {
    if (!str) return;
    if (scale <= 1) {
        draw_string(str, x, y, fg);
        return;
    }

    int curr_x = x;
    int curr_y = y;

    while (*str) {
        if (*str == '\n') {
            curr_x = x;
            curr_y += 18 * scale;
        } else if (*str == '\t') {
            curr_x += 4 * 8 * scale;
        } else {
            draw_char_scaled(*str, curr_x, curr_y, fg, scale);
            curr_x += 8 * scale;
        }
        str++;
    }
}

} // namespace gfx
