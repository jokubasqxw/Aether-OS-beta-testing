#ifndef KERNEL_FONT_HPP
#define KERNEL_FONT_HPP

#include "types.hpp"

namespace font {

constexpr size_t FONT_WIDTH = 8;
constexpr size_t FONT_HEIGHT = 16;

extern const uint8_t FONT_8X16[128][16];

} // namespace font

#endif // KERNEL_FONT_HPP
