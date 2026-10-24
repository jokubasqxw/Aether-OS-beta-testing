#ifndef KERNEL_RTC_HPP
#define KERNEL_RTC_HPP

#include "types.hpp"

namespace rtc {

struct DateTime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

void init();
DateTime get_datetime();
void print_datetime();

} // namespace rtc

#endif // KERNEL_RTC_HPP
