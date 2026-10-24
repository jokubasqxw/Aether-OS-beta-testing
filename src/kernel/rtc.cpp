#include "rtc.hpp"
#include "printk.hpp"
#include "../arch/x86_64/io.hpp"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

namespace rtc {

static inline bool is_update_in_progress() {
    arch::outb(CMOS_ADDRESS, 0x0A);
    return (arch::inb(CMOS_DATA) & 0x80) != 0;
}

static inline uint8_t read_register(uint8_t reg) {
    arch::outb(CMOS_ADDRESS, reg);
    return arch::inb(CMOS_DATA);
}

void init() {
    // CMOS RTC ready
}

DateTime get_datetime() {
    DateTime dt;

    // Wait until RTC is not updating
    while (is_update_in_progress()) {}

    uint8_t second = read_register(0x00);
    uint8_t minute = read_register(0x02);
    uint8_t hour   = read_register(0x04);
    uint8_t day    = read_register(0x07);
    uint8_t month  = read_register(0x08);
    uint8_t year   = read_register(0x09);
    uint8_t register_b = read_register(0x0B);

    // Convert BCD to binary if necessary
    if (!(register_b & 0x04)) {
        second = ((second & 0xF0) >> 1) + ((second & 0xF0) >> 3) + (second & 0x0F);
        minute = ((minute & 0xF0) >> 1) + ((minute & 0xF0) >> 3) + (minute & 0x0F);
        hour   = (((hour & 0xF0) >> 1) + ((hour & 0xF0) >> 3) + (hour & 0x0F));
        day    = ((day & 0xF0) >> 1) + ((day & 0xF0) >> 3) + (day & 0x0F);
        month  = ((month & 0xF0) >> 1) + ((month & 0xF0) >> 3) + (month & 0x0F);
        year   = ((year & 0xF0) >> 1) + ((year & 0xF0) >> 3) + (year & 0x0F);
    }

    // Convert 12 hour clock to 24 hour clock if necessary
    if (!(register_b & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    dt.second = second;
    dt.minute = minute;
    dt.hour = hour;
    dt.day = day;
    dt.month = month;
    dt.year = 2000 + year; // Century 2000s

    return dt;
}

void print_datetime() {
    DateTime dt = get_datetime();
    printk("Date/Time: %u-%s%u-%s%u %s%u:%s%u:%s%u UTC\n",
           dt.year,
           dt.month < 10 ? "0" : "", dt.month,
           dt.day < 10 ? "0" : "", dt.day,
           dt.hour < 10 ? "0" : "", dt.hour,
           dt.minute < 10 ? "0" : "", dt.minute,
           dt.second < 10 ? "0" : "", dt.second);
}

} // namespace rtc
