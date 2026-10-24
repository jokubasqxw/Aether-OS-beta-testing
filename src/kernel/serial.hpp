#ifndef KERNEL_SERIAL_HPP
#define KERNEL_SERIAL_HPP

#include "types.hpp"

namespace serial {

void init();
void write_char(char c);
void write(const char* str);
void write_hex(uint64_t val);
void write_dec(uint64_t val);
bool has_char();
char read_char();

} // namespace serial

#endif // KERNEL_SERIAL_HPP
