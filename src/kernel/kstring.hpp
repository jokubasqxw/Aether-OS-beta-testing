#ifndef KERNEL_KSTRING_HPP
#define KERNEL_KSTRING_HPP

#include "types.hpp"

extern "C" {
    void* memset(void* dest, int val, size_t len);
    void* memcpy(void* dest, const void* src, size_t len);
    void* memmove(void* dest, const void* src, size_t len);
    int memcmp(const void* s1, const void* s2, size_t len);
    size_t strlen(const char* str);
    int strcmp(const char* s1, const char* s2);
    int strncmp(const char* s1, const char* s2, size_t n);
    char* strcpy(char* dest, const char* src);
    char* strncpy(char* dest, const char* src, size_t n);
    char* strchr(const char* s, int c);
    char* strstr(const char* haystack, const char* needle);
}

namespace kstd {
    using ::memset;
    using ::memcpy;
    using ::memmove;
    using ::memcmp;
    using ::strlen;
    using ::strcmp;
    using ::strncmp;
    using ::strcpy;
    using ::strncpy;
    using ::strchr;
    using ::strstr;

    char* utoa(uint64_t val, char* buf, int base);
    char* itoa(int64_t val, char* buf, int base);
    bool isdigit(char c);
    bool isspace(char c);
    bool isalpha(char c);
    char tolower(char c);
    char toupper(char c);
    uint64_t parse_hex(const char* str);
    int64_t parse_int(const char* str);
}

#endif // KERNEL_KSTRING_HPP
