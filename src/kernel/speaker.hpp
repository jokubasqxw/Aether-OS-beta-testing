#ifndef KERNEL_SPEAKER_HPP
#define KERNEL_SPEAKER_HPP

#include "types.hpp"

namespace speaker {

void play(uint32_t freq_hz);
void stop();
void beep(uint32_t freq_hz = 750, uint32_t duration_ms = 100);
void play_tune();

} // namespace speaker

#endif // KERNEL_SPEAKER_HPP
