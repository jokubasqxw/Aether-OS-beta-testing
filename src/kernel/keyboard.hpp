#ifndef KERNEL_KEYBOARD_HPP
#define KERNEL_KEYBOARD_HPP

#include "types.hpp"

namespace keyboard {

enum KeyCode : uint8_t {
    KEY_NONE = 0,
    KEY_BACKSPACE = '\b',
    KEY_TAB = '\t',
    KEY_ENTER = '\n',
    KEY_ESC = 27,
    KEY_UP = 0x80,
    KEY_DOWN = 0x81,
    KEY_LEFT = 0x82,
    KEY_RIGHT = 0x83,
    KEY_HOME = 0x84,
    KEY_END = 0x85,
    KEY_PAGE_UP = 0x86,
    KEY_PAGE_DOWN = 0x87,
    KEY_INSERT = 0x88,
    KEY_DELETE = 0x89
};

void init();
bool has_char();
char get_char();
char get_char_blocking();
void read_line(char* buffer, size_t max_len);

} // namespace keyboard

#endif // KERNEL_KEYBOARD_HPP
