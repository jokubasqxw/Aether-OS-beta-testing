#include "keyboard.hpp"
#include "serial.hpp"
#include "idt.hpp"
#include "pic.hpp"
#include "vga.hpp"
#include "../arch/x86_64/io.hpp"

#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define BUFFER_CAPACITY  256

namespace keyboard {

static char key_buffer[BUFFER_CAPACITY];
static volatile size_t buffer_head = 0;
static volatile size_t buffer_tail = 0;

static bool shift_pressed = false;
static bool caps_lock = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;

static const char SCANCODE_LOWER[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   /* Ctrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   /* Left Shift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   /* Right Shift */
    '*',
    0,   /* Alt */
    ' ', /* Space */
    0,   /* Caps lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* F1-F10 */
    0,   /* Num lock */
    0,   /* Scroll lock */
    0,   /* Home */
    0,   /* Up */
    0,   /* Page Up */
    '-',
    0,   /* Left */
    0,
    0,   /* Right */
    '+',
    0,   /* End */
    0,   /* Down */
    0,   /* Page Down */
    0,   /* Insert */
    0,   /* Delete */
    0, 0, 0,
    0, 0, /* F11, F12 */
    0
};

static const char SCANCODE_UPPER[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   /* Ctrl */
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   /* Left Shift */
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   /* Right Shift */
    '*',
    0,   /* Alt */
    ' ', /* Space */
    0,   /* Caps lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* F1-F10 */
    0,   /* Num lock */
    0,   /* Scroll lock */
    0,   /* Home */
    0,   /* Up */
    0,   /* Page Up */
    '-',
    0,   /* Left */
    0,
    0,   /* Right */
    '+',
    0,   /* End */
    0,   /* Down */
    0,   /* Page Down */
    0,   /* Insert */
    0,   /* Delete */
    0, 0, 0,
    0, 0, /* F11, F12 */
    0
};

static void push_char(char c) {
    size_t next = (buffer_head + 1) % BUFFER_CAPACITY;
    if (next != buffer_tail) {
        key_buffer[buffer_head] = c;
        buffer_head = next;
    }
}

static bool extended_scancode = false;

static void keyboard_irq_handler(idt::Registers*) {
    uint8_t scancode = arch::inb(KBD_DATA_PORT);

    if (scancode == 0xE0) {
        extended_scancode = true;
        return;
    }

    if (scancode & 0x80) {
        // Key released
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) {
            shift_pressed = false;
        } else if (released == 0x1D) {
            ctrl_pressed = false;
        } else if (released == 0x38) {
            alt_pressed = false;
        }
        extended_scancode = false;
        return;
    }

    // Key pressed
    bool ext = extended_scancode;
    extended_scancode = false;

    if (ext) {
        if (scancode == 0x48) { push_char(static_cast<char>(KEY_UP)); return; }
        if (scancode == 0x50) { push_char(static_cast<char>(KEY_DOWN)); return; }
        if (scancode == 0x4B) { push_char(static_cast<char>(KEY_LEFT)); return; }
        if (scancode == 0x4D) { push_char(static_cast<char>(KEY_RIGHT)); return; }
        if (scancode == 0x47) { push_char(static_cast<char>(KEY_HOME)); return; }
        if (scancode == 0x4F) { push_char(static_cast<char>(KEY_END)); return; }
        if (scancode == 0x49) { push_char(static_cast<char>(KEY_PAGE_UP)); return; }
        if (scancode == 0x51) { push_char(static_cast<char>(KEY_PAGE_DOWN)); return; }
        if (scancode == 0x53) { push_char(static_cast<char>(KEY_DELETE)); return; }
    } else {
        if (scancode == 0x48) { push_char(static_cast<char>(KEY_UP)); return; }
        if (scancode == 0x50) { push_char(static_cast<char>(KEY_DOWN)); return; }
        if (scancode == 0x4B) { push_char(static_cast<char>(KEY_LEFT)); return; }
        if (scancode == 0x4D) { push_char(static_cast<char>(KEY_RIGHT)); return; }
        if (scancode == 0x47) { push_char(static_cast<char>(KEY_HOME)); return; }
        if (scancode == 0x4F) { push_char(static_cast<char>(KEY_END)); return; }
        if (scancode == 0x53) { push_char(static_cast<char>(KEY_DELETE)); return; }
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = true;
    } else if (scancode == 0x1D) {
        ctrl_pressed = true;
    } else if (scancode == 0x38) {
        alt_pressed = true;
    } else if (scancode == 0x3A) {
        caps_lock = !caps_lock;
    } else if (scancode < 128) {
        char c = 0;
        bool upper = shift_pressed ^ caps_lock;
        if (upper) {
            c = SCANCODE_UPPER[scancode];
        } else {
            c = SCANCODE_LOWER[scancode];
        }

        if (c != 0) {
            push_char(c);
        }
    }
}

void init() {
    buffer_head = 0;
    buffer_tail = 0;
    shift_pressed = false;
    caps_lock = false;
    ctrl_pressed = false;
    alt_pressed = false;
    extended_scancode = false;

    // Register IRQ1 (vector 33)
    idt::register_handler(33, keyboard_irq_handler);

    // Unmask IRQ1 on PIC
    pic::unmask(1);
}

bool has_char() {
    return (buffer_head != buffer_tail) || serial::has_char();
}

char get_char() {
    if (buffer_head != buffer_tail) {
        char c = key_buffer[buffer_tail];
        buffer_tail = (buffer_tail + 1) % BUFFER_CAPACITY;
        return c;
    }
    if (serial::has_char()) {
        char c = serial::read_char();
        if (c == 27) { // ANSI escape sequence check
            if (serial::has_char()) {
                char c2 = serial::read_char();
                if (c2 == '[') {
                    if (serial::has_char()) {
                        char c3 = serial::read_char();
                        if (c3 == 'A') return static_cast<char>(KEY_UP);
                        if (c3 == 'B') return static_cast<char>(KEY_DOWN);
                        if (c3 == 'D') return static_cast<char>(KEY_LEFT);
                        if (c3 == 'C') return static_cast<char>(KEY_RIGHT);
                    }
                }
            }
            return 27;
        }
        if (c == '\r') c = '\n';
        if (c == 127) c = '\b'; // Del to backspace
        return c;
    }
    return 0;
}

char get_char_blocking() {
    while (!has_char()) {
        arch::hlt();
    }
    return get_char();
}

void read_line(char* buffer, size_t max_len) {
    size_t index = 0;
    while (true) {
        char c = get_char_blocking();

        if (c == '\n' || c == '\r') {
            vga::put_char('\n');
            serial::write("\n");
            buffer[index] = '\0';
            break;
        } else if (c == '\b') {
            if (index > 0) {
                index--;
                vga::put_char('\b');
                serial::write("\b \b");
            }
        } else if (c >= 32 && c <= 126) {
            if (index < max_len - 1) {
                buffer[index++] = c;
                vga::put_char(c);
                serial::write_char(c);
            }
        }
    }
}

} // namespace keyboard
