#ifndef KERNEL_VGA_HPP
#define KERNEL_VGA_HPP

#include "types.hpp"

namespace vga {

enum class Color : uint8_t {
    Black = 0,
    Blue = 1,
    Green = 2,
    Cyan = 3,
    Red = 4,
    Magenta = 5,
    Brown = 6,
    LightGrey = 7,
    DarkGrey = 8,
    LightBlue = 9,
    LightGreen = 10,
    LightCyan = 11,
    LightRed = 12,
    LightMagenta = 13,
    Yellow = 14,
    White = 15
};

constexpr size_t WIDTH = 80;
constexpr size_t HEIGHT = 25;

inline constexpr uint8_t make_color(Color fg, Color bg) {
    return static_cast<uint8_t>(fg) | (static_cast<uint8_t>(bg) << 4);
}

inline constexpr uint16_t make_entry(unsigned char c, uint8_t color) {
    return static_cast<uint16_t>(c) | (static_cast<uint16_t>(color) << 8);
}

void init();
void clear();
void set_color(Color fg, Color bg);
void set_color_raw(uint8_t color);
uint8_t get_color_raw();
void put_char(char c);
void write(const char* str);
void write_colored(const char* str, Color fg, Color bg = Color::Black);
void set_cursor(size_t x, size_t y);
void get_cursor(size_t& x, size_t& y);
void put_entry_at(char c, uint8_t color, size_t x, size_t y);
char get_char_at(size_t x, size_t y);
void enable_cursor(uint8_t cursor_start, uint8_t cursor_end);
void disable_cursor();
void restore_text_mode();

} // namespace vga

#endif // KERNEL_VGA_HPP
