#define NDEBUG
#define STBTT_STATIC
#define STBTT_ifloor(x) font_ttf_ifloor(x)
#define STBTT_iceil(x) font_ttf_iceil(x)
#define STBTT_sqrt(x) font_ttf_sqrt(x)
#define STBTT_pow(x,y) font_ttf_pow(x,y)
#define STBTT_fmod(x,y) font_ttf_fmod(x,y)
#define STBTT_cos(x) font_ttf_cos(x)
#define STBTT_acos(x) font_ttf_acos(x)
#define STBTT_fabs(x) font_ttf_fabs(x)
#define STBTT_malloc(x,u) font_ttf_malloc(x)
#define STBTT_free(x,u) font_ttf_free(x)

#include "stb_truetype.h"
#include "font_data.hpp"
#include "heap.hpp"

extern "C" {
static inline int font_ttf_ifloor(float x) { return (int)(x >= 0 ? (int)x : (int)x - (x != (int)x)); }
static inline int font_ttf_iceil(float x) { return (int)(x >= 0 ? (int)x + (x != (int)x) : (int)x); }
static inline float font_ttf_fabs(float x) { return x < 0 ? -x : x; }
static float font_ttf_sqrt(float x) {
    if (x <= 0) return 0;
    float r = x;
    for (int i = 0; i < 10; i++) r = (r + x / r) * 0.5f;
    return r;
}
static float font_ttf_pow(float x, float y) {
    if (y == 0) return 1;
    if (y == 1) return x;
    // Simple: x^y = exp(y * ln(x)) - approximate with repeated multiplication for integer y
    if (y == (int)y) {
        float r = 1;
        int iy = (int)y;
        if (iy < 0) { x = 1/x; iy = -iy; }
        for (int i = 0; i < iy; i++) r *= x;
        return r;
    }
    return 1; // fallback
}
static float font_ttf_fmod(float x, float y) {
    if (y == 0) return 0;
    int q = (int)(x / y);
    return x - q * y;
}
static float font_ttf_cos(float x) {
    // Normalize to [-pi, pi]
    while (x > 3.14159f) x -= 6.28318f;
    while (x < -3.14159f) x += 6.28318f;
    float x2 = x * x;
    return 1.0f - x2/2.0f + x2*x2/24.0f - x2*x2*x2/720.0f;
}
static float font_ttf_acos(float x) {
    if (x >= 1) return 0;
    if (x <= -1) return 3.14159f;
    return 1.5708f - (x + x*x*x/6.0f + 3*x*x*x*x*x/40.0f); // approximate
}
static void* font_ttf_malloc(size_t size) { return heap::malloc(size); }
static void font_ttf_free(void* ptr) { heap::free(ptr); }
}

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "font_ttf.hpp"
#include "gfx.hpp"

namespace font {
namespace ttf {

static stbtt_fontinfo font_info;
static bool initialized = false;
static const uint8_t* font_data = nullptr;
static int font_data_size = 0;

bool init() {
    font_data = font::TTF_FONT_DATA;
    font_data_size = (int)font::TTF_FONT_SIZE;
    
    int offset = stbtt_GetFontOffsetForIndex(font_data, 0);
    if (offset < 0) return false;
    
    if (!stbtt_InitFont(&font_info, font_data, offset)) return false;
    
    initialized = true;
    return true;
}

int font_height(int font_size) {
    if (!initialized) return 16;
    float scale = stbtt_ScaleForPixelHeight(&font_info, (float)font_size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &lineGap);
    return (int)((ascent - descent + lineGap) * scale);
}

int text_width(const char* text, int font_size) {
    if (!initialized || !text) return 0;
    float scale = stbtt_ScaleForPixelHeight(&font_info, (float)font_size);
    int width = 0;
    int prev = 0;
    for (const char* p = text; *p; p++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font_info, *p, &advance, &lsb);
        width += (int)(advance * scale);
        if (prev) {
            width += (int)(stbtt_GetCodepointKernAdvance(&font_info, prev, *p) * scale);
        }
        prev = *p;
    }
    return width;
}

int render_text(const char* text, uint32_t* buffer, int buf_w, int buf_h,
                int x, int y, uint32_t fg_color, int font_size) {
    if (!initialized || !text || !buffer) return 0;
    
    float scale = stbtt_ScaleForPixelHeight(&font_info, (float)font_size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &lineGap);
    int baseline = y + (int)(ascent * scale);
    
    int cx = x;
    int prev = 0;
    for (const char* p = text; *p; p++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font_info, *p, &advance, &lsb);
        
        if (prev) {
            cx += (int)(stbtt_GetCodepointKernAdvance(&font_info, prev, *p) * scale);
        }
        
        int w, h, xoff, yoff;
        unsigned char* bitmap = stbtt_GetCodepointBitmap(&font_info, scale, scale, *p, &w, &h, &xoff, &yoff);
        
        if (bitmap) {
            for (int j = 0; j < h; j++) {
                for (int i = 0; i < w; i++) {
                    int px = cx + xoff + i;
                    int py = baseline + yoff + j;
                    if (px >= 0 && px < buf_w && py >= 0 && py < buf_h) {
                        uint8_t alpha = bitmap[j * w + i];
                        if (alpha > 0) {
                            // Blend with existing pixel
                            uint32_t bg = buffer[py * buf_w + px];
                            uint8_t br = (bg >> 16) & 0xFF;
                            uint8_t bg_g = (bg >> 8) & 0xFF;
                            uint8_t bb = bg & 0xFF;
                            uint8_t fg_r = (fg_color >> 16) & 0xFF;
                            uint8_t fg_g = (fg_color >> 8) & 0xFF;
                            uint8_t fg_b = fg_color & 0xFF;
                            uint8_t a = alpha;
                            uint8_t r = (fg_r * a + br * (255 - a)) / 255;
                            uint8_t g = (fg_g * a + bg_g * (255 - a)) / 255;
                            uint8_t b = (fg_b * a + bb * (255 - a)) / 255;
                            buffer[py * buf_w + px] = 0xFF000000 | (r << 16) | (g << 8) | b;
                        }
                    }
                }
            }
            stbtt_FreeBitmap(bitmap, nullptr);
        }
        
        cx += (int)(advance * scale);
        prev = *p;
    }
    
    return cx - x;
}

} // namespace ttf
} // namespace font