#ifndef KERNEL_MOUSE_HPP
#define KERNEL_MOUSE_HPP

#include "types.hpp"

namespace mouse {

void init();
void set_bounds(int max_x, int max_y);
void set_position(int x, int y);
int get_x();
int get_y();
bool is_left_down();
bool is_right_down();
bool is_middle_down();

// Returns the accumulated scroll-wheel delta since the last call, then resets
// it. Positive = scrolled up, negative = scrolled down. Returns 0 if the
// attached device has no IntelliMouse wheel.
int take_wheel();

} // namespace mouse

#endif // KERNEL_MOUSE_HPP
