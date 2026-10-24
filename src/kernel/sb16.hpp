#ifndef KERNEL_SB16_HPP
#define KERNEL_SB16_HPP

#include "types.hpp"

namespace sb16 {

bool init();
bool is_available();
uint16_t get_base_port();
uint8_t get_version_major();
uint8_t get_version_minor();

void set_master_volume(uint8_t volume_percent); // 0-100%
uint8_t get_master_volume();
void set_mute(bool mute);
bool is_muted();

} // namespace sb16

#endif // KERNEL_SB16_HPP
