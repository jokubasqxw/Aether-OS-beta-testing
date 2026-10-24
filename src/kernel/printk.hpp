#ifndef KERNEL_PRINTK_HPP
#define KERNEL_PRINTK_HPP

#include "types.hpp"
#include <stdarg.h>

void printk(const char* fmt, ...);
void printk_va(const char* fmt, va_list args);

#endif // KERNEL_PRINTK_HPP
