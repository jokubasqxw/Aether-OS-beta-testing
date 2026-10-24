#include "kstring.hpp"

extern "C" {

void* memset(void* dest, int val, size_t len) {
    uint8_t* ptr = static_cast<uint8_t*>(dest);
    uint8_t byte = static_cast<uint8_t>(val);

    // Align to 8 bytes
    while (len && (reinterpret_cast<uintptr_t>(ptr) & 7)) {
        *ptr++ = byte;
        len--;
    }

    size_t qwords = len / 8;
    len %= 8;

    if (qwords) {
        uint64_t val64 = 0x0101010101010101ULL * byte;
        uint64_t* ptr64 = reinterpret_cast<uint64_t*>(ptr);
        while (qwords--) {
            *ptr64++ = val64;
        }
        ptr = reinterpret_cast<uint8_t*>(ptr64);
    }

    while (len--) {
        *ptr++ = byte;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t len) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // If both pointers have the same 8-byte alignment, align to 8 bytes and copy 64-bit qwords
    if ((reinterpret_cast<uintptr_t>(d) & 7) == (reinterpret_cast<uintptr_t>(s) & 7)) {
        while (len && (reinterpret_cast<uintptr_t>(d) & 7)) {
            *d++ = *s++;
            len--;
        }
        size_t qwords = len / 8;
        len %= 8;
        uint64_t* d64 = reinterpret_cast<uint64_t*>(d);
        const uint64_t* s64 = reinterpret_cast<const uint64_t*>(s);
        while (qwords--) {
            *d64++ = *s64++;
        }
        d = reinterpret_cast<uint8_t*>(d64);
        s = reinterpret_cast<const uint8_t*>(s64);
    }

    while (len--) {
        *d++ = *s++;
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t len) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        for (size_t i = 0; i < len; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = len; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t len) {
    const uint8_t* p1 = static_cast<const uint8_t*>(s1);
    const uint8_t* p2 = static_cast<const uint8_t*>(s2);
    for (size_t i = 0; i < len; ++i) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return static_cast<unsigned char>(*s1) - static_cast<unsigned char>(*s2);
}

int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (s1[i] != s2[i]) {
            return static_cast<unsigned char>(s1[i]) - static_cast<unsigned char>(s2[i]);
        }
        if (s1[i] == '\0') {
            break;
        }
    }
    return 0;
}

char* strcpy(char* dest, const char* src) {
    char* ret = dest;
    while ((*dest++ = *src++) != '\0') {}
    return ret;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
    for (; i < n; ++i) {
        dest[i] = '\0';
    }
    return dest;
}

char* strchr(const char* s, int c) {
    while (*s != '\0') {
        if (*s == static_cast<char>(c)) {
            return const_cast<char*>(s);
        }
        s++;
    }
    if (c == '\0') return const_cast<char*>(s);
    return nullptr;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return const_cast<char*>(haystack);
    for (; *haystack; ++haystack) {
        if (*haystack == *needle) {
            const char* h = haystack;
            const char* n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) return const_cast<char*>(haystack);
        }
    }
    return nullptr;
}

} // extern "C"

namespace kstd {

char* utoa(uint64_t val, char* buf, int base) {
    if (base < 2 || base > 36) {
        *buf = '\0';
        return buf;
    }
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }

    char tmp[65];
    int pos = 0;
    const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    while (val != 0) {
        tmp[pos++] = digits[val % base];
        val /= base;
    }

    int i = 0;
    while (pos > 0) {
        buf[i++] = tmp[--pos];
    }
    buf[i] = '\0';
    return buf;
}

char* itoa(int64_t val, char* buf, int base) {
    if (base == 10 && val < 0) {
        buf[0] = '-';
        utoa(static_cast<uint64_t>(-val), buf + 1, base);
        return buf;
    }
    return utoa(static_cast<uint64_t>(val), buf, base);
}

bool isdigit(char c) {
    return c >= '0' && c <= '9';
}

bool isspace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isalpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

char toupper(char c) {
    if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    return c;
}

uint64_t parse_hex(const char* str) {
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }
    uint64_t val = 0;
    while (*str) {
        char c = *str++;
        uint8_t d = 0;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        val = (val << 4) | d;
    }
    return val;
}

int64_t parse_int(const char* str) {
    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    int64_t val = 0;
    while (isdigit(*str)) {
        val = val * 10 + (*str - '0');
        str++;
    }
    return sign * val;
}

} // namespace kstd
