#ifndef KERNEL_PIT_HPP
#define KERNEL_PIT_HPP

#include "types.hpp"

namespace pit {

void init(uint32_t frequency_hz = 100);
uint64_t get_ticks();
uint64_t uptime_ms();
uint64_t uptime_seconds();
void sleep(uint32_t ms);

} // namespace pit

#endif // KERNEL_PIT_HPP
