#include "printk.hpp"
#include "vga.hpp"
#include "serial.hpp"
#include "kstring.hpp"

void printk_va(const char* fmt, va_list args) {
    char buf[65];

    while (*fmt) {
        if (*fmt != '%') {
            vga::put_char(*fmt);
            serial::write_char(*fmt);
            fmt++;
            continue;
        }

        fmt++; // skip '%'
        if (*fmt == '\0') break;

        switch (*fmt) {
            case 'c': {
                char c = static_cast<char>(va_arg(args, int));
                vga::put_char(c);
                serial::write_char(c);
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                vga::write(s);
                serial::write(s);
                break;
            }
            case 'd':
            case 'i': {
                int64_t val = va_arg(args, int64_t);
                kstd::itoa(val, buf, 10);
                vga::write(buf);
                serial::write(buf);
                break;
            }
            case 'u': {
                uint64_t val = va_arg(args, uint64_t);
                kstd::utoa(val, buf, 10);
                vga::write(buf);
                serial::write(buf);
                break;
            }
            case 'x': {
                uint64_t val = va_arg(args, uint64_t);
                kstd::utoa(val, buf, 16);
                vga::write(buf);
                serial::write(buf);
                break;
            }
            case 'X': {
                uint64_t val = va_arg(args, uint64_t);
                kstd::utoa(val, buf, 16);
                for (int i = 0; buf[i]; ++i) {
                    buf[i] = kstd::toupper(buf[i]);
                }
                vga::write(buf);
                serial::write(buf);
                break;
            }
            case 'p': {
                uint64_t val = va_arg(args, uint64_t);
                vga::write("0x");
                serial::write("0x");
                kstd::utoa(val, buf, 16);
                // Zero pad to 16 hex digits for 64-bit pointer
                size_t len = strlen(buf);
                for (size_t z = len; z < 16; ++z) {
                    vga::put_char('0');
                    serial::write_char('0');
                }
                vga::write(buf);
                serial::write(buf);
                break;
            }
            case '%': {
                vga::put_char('%');
                serial::write_char('%');
                break;
            }
            default: {
                vga::put_char('%');
                vga::put_char(*fmt);
                serial::write_char('%');
                serial::write_char(*fmt);
                break;
            }
        }
        fmt++;
    }
}

void printk(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printk_va(fmt, args);
    va_end(args);
}
