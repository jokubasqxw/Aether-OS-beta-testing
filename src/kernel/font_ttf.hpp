#ifndef KERNEL_FONT_TTF_HPP
#define KERNEL_FONT_TTF_HPP

#include "types.hpp"

namespace font {
namespace ttf {

// Initialize TrueType font system (call once at boot)
bool init();

// Render a string to a pixel buffer (ARGB32)
// Returns the width of the rendered text
int render_text(const char* text, uint32_t* buffer, int buf_w, int buf_h,
                int x, int y, uint32_t fg_color, int font_size);

// Get text width in pixels
int text_width(const char* text, int font_size);

// Get font height in pixels
int font_height(int font_size);

} // namespace ttf
} // namespace font

#endif // KERNEL_FONT_TTF_HPP