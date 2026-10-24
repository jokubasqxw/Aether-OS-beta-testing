#ifndef KERNEL_GUI_HPP
#define KERNEL_GUI_HPP

#include "types.hpp"

namespace gui {

void run_desktop(uint32_t width = 1920, uint32_t height = 1080, uint32_t target_fps = 144);
void run_demo(uint32_t width = 1920, uint32_t height = 1080);
void set_initial_browser_url(const char* url);

} // namespace gui

#endif // KERNEL_GUI_HPP
