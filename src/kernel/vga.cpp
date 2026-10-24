#include "vga.hpp"
#include "bga.hpp"
#include "font.hpp"
#include "../arch/x86_64/io.hpp"

namespace vga {

static uint16_t* const VGA_BUFFER = reinterpret_cast<uint16_t*>(0xB8000);
static size_t cursor_x = 0;
static size_t cursor_y = 0;
static uint8_t current_color = make_color(Color::White, Color::Black);

static void update_hardware_cursor() {
    uint16_t pos = static_cast<uint16_t>(cursor_y * WIDTH + cursor_x);
    arch::outb(0x3D4, 0x0F);
    arch::outb(0x3D5, static_cast<uint8_t>(pos & 0xFF));
    arch::outb(0x3D4, 0x0E);
    arch::outb(0x3D5, static_cast<uint8_t>((pos >> 8) & 0xFF));
}

void enable_cursor(uint8_t cursor_start, uint8_t cursor_end) {
    arch::outb(0x3D4, 0x0A);
    arch::outb(0x3D5, (arch::inb(0x3D5) & 0xC0) | cursor_start);
    arch::outb(0x3D4, 0x0B);
    arch::outb(0x3D5, (arch::inb(0x3D5) & 0xE0) | cursor_end);
}

void disable_cursor() {
    arch::outb(0x3D4, 0x0A);
    arch::outb(0x3D5, 0x20);
}

void init() {
    cursor_x = 0;
    cursor_y = 0;
    current_color = make_color(Color::White, Color::Black);
    enable_cursor(14, 15);
    clear();
}

void clear() {
    for (size_t y = 0; y < HEIGHT; ++y) {
        for (size_t x = 0; x < WIDTH; ++x) {
            VGA_BUFFER[y * WIDTH + x] = make_entry(' ', current_color);
        }
    }
    cursor_x = 0;
    cursor_y = 0;
    update_hardware_cursor();
}

void set_color(Color fg, Color bg) {
    current_color = make_color(fg, bg);
}

void set_color_raw(uint8_t color) {
    current_color = color;
}

uint8_t get_color_raw() {
    return current_color;
}

static void scroll() {
    for (size_t y = 0; y < HEIGHT - 1; ++y) {
        for (size_t x = 0; x < WIDTH; ++x) {
            VGA_BUFFER[y * WIDTH + x] = VGA_BUFFER[(y + 1) * WIDTH + x];
        }
    }
    for (size_t x = 0; x < WIDTH; ++x) {
        VGA_BUFFER[(HEIGHT - 1) * WIDTH + x] = make_entry(' ', current_color);
    }
    cursor_y = HEIGHT - 1;
}

void put_char(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            VGA_BUFFER[cursor_y * WIDTH + cursor_x] = make_entry(' ', current_color);
        } else if (cursor_y > 0) {
            cursor_y--;
            cursor_x = WIDTH - 1;
            VGA_BUFFER[cursor_y * WIDTH + cursor_x] = make_entry(' ', current_color);
        }
    } else if (c == '\t') {
        size_t tab_stop = 4;
        size_t spaces = tab_stop - (cursor_x % tab_stop);
        for (size_t i = 0; i < spaces; ++i) {
            put_char(' ');
        }
    } else {
        VGA_BUFFER[cursor_y * WIDTH + cursor_x] = make_entry(static_cast<unsigned char>(c), current_color);
        cursor_x++;
        if (cursor_x >= WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
    }

    if (cursor_y >= HEIGHT) {
        scroll();
    }

    update_hardware_cursor();
}

void write(const char* str) {
    while (*str) {
        put_char(*str++);
    }
}

void write_colored(const char* str, Color fg, Color bg) {
    uint8_t old = current_color;
    set_color(fg, bg);
    write(str);
    current_color = old;
}

void set_cursor(size_t x, size_t y) {
    if (x >= WIDTH) x = WIDTH - 1;
    if (y >= HEIGHT) y = HEIGHT - 1;
    cursor_x = x;
    cursor_y = y;
    update_hardware_cursor();
}

void get_cursor(size_t& x, size_t& y) {
    x = cursor_x;
    y = cursor_y;
}

void put_entry_at(char c, uint8_t color, size_t x, size_t y) {
    if (x < WIDTH && y < HEIGHT) {
        VGA_BUFFER[y * WIDTH + x] = make_entry(static_cast<unsigned char>(c), color);
    }
}

char get_char_at(size_t x, size_t y) {
    if (x < WIDTH && y < HEIGHT) {
        return static_cast<char>(VGA_BUFFER[y * WIDTH + x] & 0xFF);
    }
    return ' ';
}

void restore_text_mode() {
    // 1. Disable BGA high-res mode
    bga::disable();

    // 2. Unlock CRTC registers (clear bit 7 of register 0x11)
    arch::outb(0x3D4, 0x11);
    uint8_t crtc_val = arch::inb(0x3D5);
    arch::outb(0x3D5, crtc_val & 0x7F);

    // 3. Miscellaneous Output register (28MHz clock, 400 lines, page 0, enable RAM)
    arch::outb(0x3C2, 0x67);

    // 4. Sequencer registers (5 registers for 80x25 text mode)
    const uint8_t seq_regs[5] = { 0x03, 0x00, 0x03, 0x00, 0x02 };
    for (uint8_t i = 0; i < 5; ++i) {
        arch::outb(0x3C4, i);
        arch::outb(0x3C5, seq_regs[i]);
    }

    // 5. CRTC registers (25 registers for 80x25 text mode)
    const uint8_t crtc_regs[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
        0x00, 0x4F, 0x0E, 0x0F, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
        0xFF
    };
    for (uint8_t i = 0; i < 25; ++i) {
        arch::outb(0x3D4, i);
        arch::outb(0x3D5, crtc_regs[i]);
    }

    // 6. Graphics Controller registers (9 registers)
    const uint8_t gc_regs[9] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
    };
    for (uint8_t i = 0; i < 9; ++i) {
        arch::outb(0x3CE, i);
        arch::outb(0x3CF, gc_regs[i]);
    }

    // 7. Attribute Controller registers (21 registers)
    arch::inb(0x3DA); // Reset attribute controller flip-flop
    for (uint8_t i = 0; i < 16; ++i) {
        arch::outb(0x3C0, i);
        arch::outb(0x3C0, i); // Identity palette
    }
    const uint8_t ac_regs[5] = { 0x0C, 0x00, 0x0F, 0x08, 0x00 };
    for (uint8_t i = 0; i < 5; ++i) {
        arch::outb(0x3C0, 16 + i);
        arch::outb(0x3C0, ac_regs[i]);
    }
    arch::outb(0x3C0, 0x20); // Enable video output

    // 8. Reload 8x16 font into Plane 2 memory (0xA0000)
    auto seq_write = [](uint8_t idx, uint8_t val) {
        arch::outb(0x3C4, idx);
        arch::outb(0x3C5, val);
    };
    auto gc_write = [](uint8_t idx, uint8_t val) {
        arch::outb(0x3CE, idx);
        arch::outb(0x3CF, val);
    };

    seq_write(0, 0x01); // Synchronous reset
    seq_write(2, 0x04); // Write to plane 2 only
    seq_write(4, 0x07); // Sequential addressing
    seq_write(0, 0x03); // Clear reset

    gc_write(4, 0x02); // Read plane 2
    gc_write(5, 0x00); // Disable odd/even
    gc_write(6, 0x00); // Map 0xA0000 (64K)

    uint8_t* font_vram = reinterpret_cast<uint8_t*>(0xA0000);
    for (int c = 0; c < 128; ++c) {
        for (int r = 0; r < 16; ++r) {
            font_vram[c * 32 + r] = font::FONT_8X16[c][r];
        }
        for (int p = 16; p < 32; ++p) {
            font_vram[c * 32 + p] = 0;
        }
    }

    // Restore sequencer to text mode (planes 0 and 1)
    seq_write(0, 0x01);
    seq_write(2, 0x03);
    seq_write(4, 0x03);
    seq_write(0, 0x03);

    // Restore graphics controller to text mode (0xB8000)
    gc_write(4, 0x00);
    gc_write(5, 0x10);
    gc_write(6, 0x0E);

    // 9. Standard 16-color VGA DAC Palette (6-bit values 0..63)
    const uint8_t dac_colors[16][3] = {
        {  0,  0,  0 }, {  0,  0, 42 }, {  0, 42,  0 }, {  0, 42, 42 },
        { 42,  0,  0 }, { 42,  0, 42 }, { 42, 21,  0 }, { 42, 42, 42 },
        { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
        { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 }
    };
    arch::outb(0x3C8, 0x00); // Start at palette index 0
    for (int i = 0; i < 16; ++i) {
        arch::outb(0x3C9, dac_colors[i][0]);
        arch::outb(0x3C9, dac_colors[i][1]);
        arch::outb(0x3C9, dac_colors[i][2]);
    }

    // 10. Re-initialize software cursor and clear 0xB8000
    init();
}

} // namespace vga
