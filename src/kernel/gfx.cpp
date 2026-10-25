#include "gfx.hpp"
#include "gfx_driver.hpp"
#include "font.hpp"
#include "kstring.hpp"
#include "aetherfs.hpp"
#include "fat16.hpp"
#include "inflate.hpp"

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
    if (y < 0 || y + 16 > static_cast<int>(screen_height)) return;

    const uint8_t* glyph = font::FONT_8X16[static_cast<uint8_t>(c)];
    uint32_t* buf = get_render_target();

    for (int row = 0; row < 16; ++row) {
        if (y + row < 0 || y + row >= static_cast<int>(screen_height)) continue;
        uint32_t* row_ptr = buf + (y + row) * screen_width;
        // Clip horizontally: only draw pixels that fit within screen
        for (int col = 0; col < 8; ++col) {
            int px = x + col;
            if (px < 0 || px >= static_cast<int>(screen_width)) continue;
            if (transparent_bg) {
                if (glyph[row] & (0x80 >> col)) row_ptr[px] = fg;
            } else {
                row_ptr[px] = (glyph[row] & (0x80 >> col)) ? fg : bg;
            }
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

// Image loading and drawing
bool load_bmp_file(const uint8_t* data, size_t size, uint32_t** out_pixels, uint32_t* out_w, uint32_t* out_h) {
    // Simple BMP parser - supports 24-bit and 32-bit BMP
    if (size < 54) return false;  // Minimum BMP header size

    // Check BMP signature
    if (data[0] != 'B' || data[1] != 'M') return false;

    // Read BMP header
    uint32_t file_size = *(uint32_t*)(data + 2);
    uint32_t offset = *(uint32_t*)(data + 10);
    uint32_t width = *(uint32_t*)(data + 18);
    uint32_t height = *(uint32_t*)(data + 22);
    uint16_t bpp = *(uint16_t*)(data + 28);
    uint16_t compression = *(uint16_t*)(data + 30);

    if (bpp != 24 && bpp != 32) return false;  // Only 24/32-bit supported
    if (compression != 0) return false;  // No compression supported

    *out_w = width;
    *out_h = height;

    // Calculate row size (must be multiple of 4)
    uint32_t row_size = (width * bpp / 8 + 3) & ~3;

    // Allocate pixel buffer
    uint32_t* pixels = new uint32_t[width * height];
    if (!pixels) return false;

    // Read pixel data
    const uint8_t* src = data + offset;
    for (uint32_t y = 0; y < height; y++) {
        // BMP stores rows bottom-to-top, flip vertically
        uint32_t src_y = (height - 1 - y) * row_size;

        for (uint32_t x = 0; x < width; x++) {
            uint8_t b = src[src_y + x * (bpp / 8) + 0];
            uint8_t g = src[src_y + x * (bpp / 8) + 1];
            uint8_t r = src[src_y + x * (bpp / 8) + 2];
            uint8_t a = (bpp == 32) ? src[src_y + x * (bpp / 8) + 3] : 255;

            // Convert BGR/BGRA to ARGB
            uint32_t color = (a << 24) | (r << 16) | (g << 8) | b;
            pixels[y * width + x] = color;
        }
    }

    *out_pixels = pixels;
    return true;
}

// PNG loader - supports RGB (type 2) and RGBA (type 6) with 8-bit depth
bool load_png_file(const uint8_t* data, size_t size, uint32_t** out_pixels, uint32_t* out_w, uint32_t* out_h) {
    // Check PNG signature
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8) return false;
    for (int i = 0; i < 8; ++i) {
        if (data[i] != png_sig[i]) return false;
    }
    
    size_t pos = 8;  // Skip signature
    uint32_t width = 0, height = 0;
    uint8_t bit_depth = 0, color_type = 0;
    uint8_t* idat_data = nullptr;
    size_t idat_size = 0;
    size_t idat_cap = 0;
    
    // Parse chunks
    while (pos + 12 <= size) {
        uint32_t chunk_len = (data[pos] << 24) | (data[pos+1] << 16) | (data[pos+2] << 8) | data[pos+3];
        pos += 4;
        char chunk_type[5] = {static_cast<char>(data[pos]), static_cast<char>(data[pos+1]), static_cast<char>(data[pos+2]), static_cast<char>(data[pos+3]), '\0'};
        pos += 4;
        
        if (kstd::strcmp(chunk_type, "IHDR") == 0 && chunk_len >= 13) {
            width = (data[pos] << 24) | (data[pos+1] << 16) | (data[pos+2] << 8) | data[pos+3];
            height = (data[pos+4] << 24) | (data[pos+5] << 16) | (data[pos+6] << 8) | data[pos+7];
            bit_depth = data[pos+8];
            color_type = data[pos+9];
            if (data[pos+12] != 0) {  // interlace not supported
                if (idat_data) delete[] idat_data;
                return false;
            }
        } else if (kstd::strcmp(chunk_type, "IDAT") == 0) {
            if (idat_size + chunk_len > idat_cap) {
                size_t new_cap = idat_cap == 0 ? chunk_len * 2 : idat_cap * 2;
                if (new_cap < idat_size + chunk_len) new_cap = idat_size + chunk_len;
                uint8_t* new_data = new uint8_t[new_cap];
                if (idat_data) {
                    for (size_t i = 0; i < idat_size; ++i) new_data[i] = idat_data[i];
                    delete[] idat_data;
                }
                idat_data = new_data;
                idat_cap = new_cap;
            }
            for (size_t i = 0; i < chunk_len; ++i) {
                idat_data[idat_size + i] = data[pos + i];
            }
            idat_size += chunk_len;
        } else if (kstd::strcmp(chunk_type, "IEND") == 0) {
            break;
        }
        
        pos += chunk_len + 4;  // Skip data + CRC
    }
    
    if (width == 0 || height == 0 || bit_depth != 8) {
        if (idat_data) delete[] idat_data;
        return false;
    }
    if (color_type != 2 && color_type != 6) {
        if (idat_data) delete[] idat_data;
        return false;
    }
    
    int channels = (color_type == 2) ? 3 : 4;
    size_t bytes_per_pixel = channels;
    size_t stride = width * bytes_per_pixel;
    
    // Decompress IDAT data
    size_t out_cap = stride * height + height;
    uint8_t* decompressed = new uint8_t[out_cap];
    if (!decompressed) {
        if (idat_data) delete[] idat_data;
        return false;
    }
    
    size_t decompressed_size = inflate::decompress(idat_data, idat_size, decompressed, out_cap);
    if (idat_data) delete[] idat_data;
    
    if (decompressed_size == 0) {
        delete[] decompressed;
        return false;
    }
    
    // Apply PNG filters and convert to ARGB
    uint32_t* pixels = new uint32_t[width * height];
    if (!pixels) {
        delete[] decompressed;
        return false;
    }
    
    uint8_t* prev_row = new uint8_t[stride];
    for (size_t i = 0; i < stride; ++i) prev_row[i] = 0;
    
    size_t src_pos = 0;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t filter = decompressed[src_pos++];
        uint8_t* row = decompressed + src_pos;
        src_pos += stride;
        
        uint8_t* filtered = new uint8_t[stride];
        for (size_t x = 0; x < stride; ++x) {
            uint8_t left = (x >= bytes_per_pixel) ? filtered[x - bytes_per_pixel] : 0;
            uint8_t up = prev_row[x];
            uint8_t up_left = (x >= bytes_per_pixel) ? prev_row[x - bytes_per_pixel] : 0;
            
            switch (filter) {
                case 0: filtered[x] = row[x]; break;
                case 1: filtered[x] = row[x] + left; break;
                case 2: filtered[x] = row[x] + up; break;
                case 3: filtered[x] = row[x] + (left + up) / 2; break;
                case 4: {
                    int p = left + up - up_left;
                    int pa = (p > left) ? (p - left) : (left - p);
                    int pb = (p > up) ? (p - up) : (up - p);
                    int pc = (p > up_left) ? (p - up_left) : (up_left - p);
                    uint8_t predictor;
                    if (pa <= pb && pa <= pc) predictor = left;
                    else if (pb <= pc) predictor = up;
                    else predictor = up_left;
                    filtered[x] = row[x] + predictor;
                    break;
                }
                default: filtered[x] = row[x]; break;
            }
        }
        
        for (uint32_t x = 0; x < width; ++x) {
            size_t idx = x * bytes_per_pixel;
            uint8_t r = filtered[idx];
            uint8_t g = filtered[idx + 1];
            uint8_t b = filtered[idx + 2];
            uint8_t a = (channels == 4) ? filtered[idx + 3] : 255;
            pixels[y * width + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        
        for (size_t i = 0; i < stride; ++i) prev_row[i] = filtered[i];
        delete[] filtered;
    }
    
    delete[] prev_row;
    delete[] decompressed;
    
    *out_pixels = pixels;
    *out_w = width;
    *out_h = height;
    return true;
}

bool load_image_from_memory(const uint8_t* data, size_t size, Image* img) {
    if (!data || size == 0 || !img) return false;
    
    uint32_t* pixels = nullptr;
    uint32_t w = 0, h = 0;
    
    // Try BMP first
    if (load_bmp_file(data, size, &pixels, &w, &h)) {
        img->pixels = pixels;
        img->width = w;
        img->height = h;
        return true;
    }
    
    // Try PNG
    if (load_png_file(data, size, &pixels, &w, &h)) {
        img->pixels = pixels;
        img->width = w;
        img->height = h;
        return true;
    }
    
    return false;
}

bool load_image(const char* path, Image* img) {
    uint8_t* file_data = nullptr;
    uint32_t file_size = 0;

    // Try AetherFS
    if (aetherfs::mounted()) {
        file_size = aetherfs::file_size(path);
        if (file_size > 0) {
            file_data = new uint8_t[file_size];
            if (aetherfs::read_file(path, file_data, file_size) > 0) {
                if (load_bmp_file(file_data, file_size, &img->pixels, &img->width, &img->height) ||
                    load_png_file(file_data, file_size, &img->pixels, &img->width, &img->height)) {
                    delete[] file_data;
                    return true;
                }
            }
            delete[] file_data;
            return false;
        }
    }

    // Try FAT16
    if (fat16::mounted()) {
        file_size = fat16::file_size(path);
        if (file_size > 0) {
            file_data = new uint8_t[file_size];
            if (fat16::read_file(path, file_data, file_size) > 0) {
                if (load_bmp_file(file_data, file_size, &img->pixels, &img->width, &img->height) ||
                    load_png_file(file_data, file_size, &img->pixels, &img->width, &img->height)) {
                    delete[] file_data;
                    return true;
                }
            }
            delete[] file_data;
            return false;
        }
    }

    return false;
}

void draw_image(const Image& img, int x, int y) {
    if (x >= static_cast<int>(screen_width) || y >= static_cast<int>(screen_height)) return;
    if (img.width == 0 || img.height == 0) return;

    uint32_t draw_w = img.width;
    uint32_t draw_h = img.height;
    int draw_x = x;
    int draw_y = y;

    // Clip to screen
    if (draw_x < 0) { draw_w += draw_x; draw_x = 0; }
    if (draw_y < 0) { draw_h += draw_y; draw_y = 0; }
    if (draw_x + draw_w > screen_width) draw_w = screen_width - draw_x;
    if (draw_y + draw_h > screen_height) draw_h = screen_height - draw_y;
    if (draw_w == 0 || draw_h == 0) return;

    uint32_t* buf = get_render_target();
    for (uint32_t row = 0; row < draw_h; row++) {
        for (uint32_t col = 0; col < draw_w; col++) {
            uint32_t src_idx = (row * img.width + col);
            uint32_t dst_idx = ((row + draw_y) * screen_width) + (col + draw_x);
            buf[dst_idx] = img.pixels[src_idx];
        }
    }
}

void draw_image_scaled(const Image& img, int x, int y, uint32_t w, uint32_t h) {
    if (img.width == 0 || img.height == 0) return;
    if (w == 0 || h == 0) return;

    uint32_t draw_w = w;
    uint32_t draw_h = h;
    int draw_x = x;
    int draw_y = y;

    // Clip to screen
    if (draw_x < 0) { draw_w += draw_x; draw_x = 0; }
    if (draw_y < 0) { draw_h += draw_y; draw_y = 0; }
    if (draw_x + draw_w > screen_width) draw_w = screen_width - draw_x;
    if (draw_y + draw_h > screen_height) draw_h = screen_height - draw_y;
    if (draw_w == 0 || draw_h == 0) return;

    float scale_x = static_cast<float>(draw_w) / img.width;
    float scale_y = static_cast<float>(draw_h) / img.height;

    uint32_t* buf = get_render_target();
    for (uint32_t row = 0; row < draw_h; row++) {
        for (uint32_t col = 0; col < draw_w; col++) {
            uint32_t src_x = static_cast<uint32_t>(col / scale_x);
            uint32_t src_y = static_cast<uint32_t>(row / scale_y);
            if (src_x >= img.width) src_x = img.width - 1;
            if (src_y >= img.height) src_y = img.height - 1;
            uint32_t src_idx = (src_y * img.width + src_x);
            uint32_t dst_idx = (row * screen_width) + (col + draw_x);
            buf[dst_idx] = img.pixels[src_idx];
        }
    }
}

} // namespace gfx
