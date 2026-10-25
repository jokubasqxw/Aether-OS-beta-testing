#include "gui.hpp"
#include "../arch/x86_64/io.hpp"
#include "gfx.hpp"
#include "keyboard.hpp"
#include "mouse.hpp"
#include "pit.hpp"
#include "rtc.hpp"
#include "pmm.hpp"
#include "heap.hpp"
#include "printk.hpp"
#include "kstring.hpp"
#include "ramfs.hpp"
#include "e1000.hpp"
#include "net.hpp"
#include "wifi.hpp"
#include "browser_engine.hpp"
#include "sound.hpp"
#include "pat.hpp"
#include "bga.hpp"
#include "updater.hpp"
#include "fat16.hpp"
#include "aetherfs.hpp"
#include "apphub.hpp"

namespace gui {

// Fixed-point Sine lookup table for quadrant 0..64 (scaled by 256)
static const int SIN_Q[65] = {
    0, 6, 12, 18, 25, 31, 37, 43, 50, 56, 62, 68, 74, 80, 86, 92,
    98, 103, 109, 115, 120, 126, 131, 136, 142, 147, 152, 157, 162, 167, 171, 176,
    181, 185, 189, 193, 197, 201, 205, 208, 212, 215, 219, 222, 225, 228, 231, 233,
    236, 238, 240, 242, 244, 246, 247, 249, 250, 251, 252, 253, 254, 255, 255, 256
};

static inline int sin_fp(int angle) {
    angle &= 0xFF;
    if (angle <= 64) return SIN_Q[angle];
    if (angle <= 128) return SIN_Q[128 - angle];
    if (angle <= 192) return -SIN_Q[angle - 128];
    return -SIN_Q[256 - angle];
}

static inline int cos_fp(int angle) {
    return sin_fp(angle + 64);
}

struct Point3D {
    int x, y, z;
};

struct Point2D {
    int x, y;
};

// Window state structure for comprehensive window management
struct WindowState {
    int id;
    const char* title;
    uint32_t title_color;
    int x, y, w, h;
    int saved_x, saved_y, saved_w, saved_h;
    bool visible;
    bool minimized;
    bool maximized;
    bool active;
};

// Button style configuration: exactly ONE set of buttons per window
enum ButtonStyle {
    BUTTONS_CIRCLES = 0, // Sleek macOS style traffic lights on top-left: Red [X], Yellow [-], Green [+]
    BUTTONS_SQUARES = 1  // Classic Windows style controls on top-right: [_], [□], [✕]
};

// Menu bar position configuration
enum BarPosition {
    BAR_TOP = 0,    // Sleek macOS / GNOME Top Menu Bar (default, never cut off)
    BAR_BOTTOM = 1  // Classic Windows / KDE Bottom Taskbar
};

enum CursorType {
    CURSOR_POINTER = 0,
    CURSOR_RESIZE_NWSE = 1,
    CURSOR_RESIZE_EW = 2,
    CURSOR_RESIZE_NS = 3,
    CURSOR_RESIZE_NESW = 4
};

enum ResizeZone {
    RESIZE_NONE = 0,
    RESIZE_SE = 1, // Bottom-right corner
    RESIZE_SW = 2, // Bottom-left corner
    RESIZE_E  = 3, // Right edge
    RESIZE_W  = 4, // Left edge
    RESIZE_S  = 5  // Bottom edge
};

// 12x19 Classic Hardware Pointer Cursor Bitmap
static const uint8_t MOUSE_CURSOR_POINTER[19][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,1,1,1,1,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0}
};

// 17x9 Horizontal Resize Cursor Bitmap (E-W)
static const uint8_t MOUSE_CURSOR_EW[9][17] = {
    {0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0},
    {0,0,0,1,2,1,0,0,0,0,0,0,1,2,1,0,0},
    {0,0,1,2,2,2,1,0,0,0,0,1,2,2,2,1,0},
    {0,1,2,2,2,2,2,1,1,1,1,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1},
    {0,1,2,2,2,2,2,1,1,1,1,2,2,2,2,2,1},
    {0,0,1,2,2,2,1,0,0,0,0,1,2,2,2,1,0},
    {0,0,0,1,2,1,0,0,0,0,0,0,1,2,1,0,0},
    {0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0}
};

// 9x17 Vertical Resize Cursor Bitmap (N-S)
static const uint8_t MOUSE_CURSOR_NS[17][9] = {
    {0,0,0,0,1,0,0,0,0},
    {0,0,0,1,2,1,0,0,0},
    {0,0,1,2,2,2,1,0,0},
    {0,1,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,1},
    {0,1,1,1,2,1,1,1,0},
    {0,0,0,1,2,1,0,0,0},
    {0,0,0,1,2,1,0,0,0},
    {0,0,0,1,2,1,0,0,0},
    {0,0,0,1,2,1,0,0,0},
    {0,0,0,1,2,1,0,0,0},
    {0,1,1,1,2,1,1,1,0},
    {1,2,2,2,2,2,2,2,1},
    {0,1,2,2,2,2,2,1,0},
    {0,0,1,2,2,2,1,0,0},
    {0,0,0,1,2,1,0,0,0},
    {0,0,0,0,1,0,0,0,0}
};

// 15x15 Diagonal Resize Cursor Bitmap (NW-SE)
static const uint8_t MOUSE_CURSOR_NWSE[15][15] = {
    {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,1,2,2,2,1,0,0,0,0,0,0,0,0},
    {1,1,0,1,2,2,2,1,0,0,0,0,0,0,0},
    {0,0,0,0,1,2,2,2,1,0,0,0,0,0,0},
    {0,0,0,0,0,1,2,2,2,1,0,0,0,0,0},
    {0,0,0,0,0,0,1,2,2,2,1,0,0,0,0},
    {0,0,0,0,0,0,0,1,2,2,2,1,0,1,1},
    {0,0,0,0,0,0,0,0,1,2,2,2,1,2,1},
    {0,0,0,0,0,0,0,0,0,1,2,2,2,2,1},
    {0,0,0,0,0,0,0,0,0,0,1,2,2,2,1},
    {0,0,0,0,0,0,0,0,0,1,2,2,2,2,1},
    {0,0,0,0,0,0,0,0,0,1,1,1,1,1,1}
};

// 15x15 Diagonal Resize Cursor Bitmap (NE-SW)
static const uint8_t MOUSE_CURSOR_NESW[15][15] = {
    {0,0,0,0,0,0,0,0,0,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,0,0,1,2,2,2,2,1},
    {0,0,0,0,0,0,0,0,0,0,1,2,2,2,1},
    {0,0,0,0,0,0,0,0,0,1,2,2,2,2,1},
    {0,0,0,0,0,0,0,0,1,2,2,2,1,2,1},
    {0,0,0,0,0,0,0,1,2,2,2,1,0,1,1},
    {0,0,0,0,0,0,1,2,2,2,1,0,0,0,0},
    {0,0,0,0,0,1,2,2,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,2,2,1,0,0,0,0,0,0},
    {1,1,0,1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,1,2,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0}
};

static inline uint8_t get_cursor_pixel(CursorType type, int r, int c) {
    switch (type) {
        case CURSOR_POINTER:
            return (r >= 0 && r < 19 && c >= 0 && c < 12) ? MOUSE_CURSOR_POINTER[r][c] : 0;
        case CURSOR_RESIZE_EW:
            return (r >= 0 && r < 9 && c >= 0 && c < 17) ? MOUSE_CURSOR_EW[r][c] : 0;
        case CURSOR_RESIZE_NS:
            return (r >= 0 && r < 17 && c >= 0 && c < 9) ? MOUSE_CURSOR_NS[r][c] : 0;
        case CURSOR_RESIZE_NWSE:
            return (r >= 0 && r < 15 && c >= 0 && c < 15) ? MOUSE_CURSOR_NWSE[r][c] : 0;
        case CURSOR_RESIZE_NESW:
            return (r >= 0 && r < 15 && c >= 0 && c < 15) ? MOUSE_CURSOR_NESW[r][c] : 0;
        default:
            return 0;
    }
}

static inline void get_cursor_info(CursorType type, int& hx, int& hy, int& cw, int& ch) {
    switch (type) {
        case CURSOR_RESIZE_EW:
            hx = 8; hy = 4; cw = 17; ch = 9; break;
        case CURSOR_RESIZE_NS:
            hx = 4; hy = 8; cw = 9; ch = 17; break;
        case CURSOR_RESIZE_NWSE:
            hx = 7; hy = 7; cw = 15; ch = 15; break;
        case CURSOR_RESIZE_NESW:
            hx = 7; hy = 7; cw = 15; ch = 15; break;
        case CURSOR_POINTER:
        default:
            hx = 0; hy = 0; cw = 12; ch = 19; break;
    }
}

// Ultra-fast cursor background buffer for zero-overhead mouse movement
static uint32_t cursor_saved_bg[32][32];
static int saved_cursor_draw_x = -1;
static int saved_cursor_draw_y = -1;
static int saved_cursor_w = 0;
static int saved_cursor_h = 0;
static bool cursor_has_saved_bg = false;

static void restore_cursor_background() {
    if (!cursor_has_saved_bg || saved_cursor_draw_x < 0) return;
    int sw = static_cast<int>(gfx::get_width());
    int sh = static_cast<int>(gfx::get_height());
    uint32_t* buf = gfx::get_backbuffer();

    for (int r = 0; r < saved_cursor_h; ++r) {
        int py = saved_cursor_draw_y + r;
        if (py < 0 || py >= sh) continue;
        for (int c = 0; c < saved_cursor_w; ++c) {
            int px = saved_cursor_draw_x + c;
            if (px >= 0 && px < sw) {
                buf[py * sw + px] = cursor_saved_bg[r][c];
            }
        }
    }
    cursor_has_saved_bg = false;
}

static void save_and_draw_cursor(int mx, int my, CursorType type = CURSOR_POINTER) {
    int sw = static_cast<int>(gfx::get_width());
    int sh = static_cast<int>(gfx::get_height());
    uint32_t* buf = gfx::get_backbuffer();

    int hx, hy, cw, ch;
    get_cursor_info(type, hx, hy, cw, ch);

    int draw_x = mx - hx;
    int draw_y = my - hy;
    int total_w = cw + 2;
    int total_h = ch + 2;
    if (total_w > 32) total_w = 32;
    if (total_h > 32) total_h = 32;

    // 1. Save background under cursor
    for (int r = 0; r < total_h; ++r) {
        int py = draw_y + r;
        for (int c = 0; c < total_w; ++c) {
            int px = draw_x + c;
            if (px >= 0 && px < sw && py >= 0 && py < sh) {
                cursor_saved_bg[r][c] = buf[py * sw + px];
            } else {
                cursor_saved_bg[r][c] = 0xFF000000;
            }
        }
    }
    saved_cursor_draw_x = draw_x;
    saved_cursor_draw_y = draw_y;
    saved_cursor_w = total_w;
    saved_cursor_h = total_h;
    cursor_has_saved_bg = true;

    // 2. Cursor drop shadow
    for (int r = 0; r < ch; ++r) {
        for (int c = 0; c < cw; ++c) {
            if (get_cursor_pixel(type, r, c) != 0) {
                int px = draw_x + c + 2;
                int py = draw_y + r + 2;
                if (px >= 0 && px < sw && py >= 0 && py < sh) {
                    gfx::put_pixel(px, py, 0xFF050B14);
                }
            }
        }
    }

    // 3. Cursor body
    for (int r = 0; r < ch; ++r) {
        for (int c = 0; c < cw; ++c) {
            uint8_t pixel = get_cursor_pixel(type, r, c);
            if (pixel != 0) {
                int px = draw_x + c;
                int py = draw_y + r;
                if (px >= 0 && px < sw && py >= 0 && py < sh) {
                    if (pixel == 1) {
                        gfx::put_pixel(px, py, 0xFF000000); // Black outline
                    } else if (pixel == 2) {
                        gfx::put_pixel(px, py, 0xFFFFFFFF); // White fill
                    }
                }
            }
        }
    }
}

// Visual Resize Grip in bottom-right corner (3 diagonal accent lines /// in teal/navy)
static void draw_resize_grip(const WindowState& win) {
    if (!win.visible || win.minimized || win.maximized) return;

    int rx = win.x + win.w - 3;
    int ry = win.y + win.h - 3;
    uint32_t c_shadow = 0xFF0E1A29; // Deep Navy shadow
    uint32_t c_accent = win.active ? gfx::Colors::Teal : 0xFF2A5570; // Vibrant Teal when active

    int lines[3] = { 3, 7, 11 };
    for (int i = 0; i < 3; ++i) {
        int k = lines[i];
        gfx::draw_line(rx - k - 1, ry, rx, ry - k - 1, c_shadow);
        gfx::draw_line(rx - k, ry, rx, ry - k, c_accent);
    }
}

// Comprehensive edge and corner hit-testing for window resizing
static ResizeZone get_window_resize_zone(const WindowState& win, int mx, int my) {
    if (!win.visible || win.minimized || win.maximized) return RESIZE_NONE;

    int x = win.x;
    int y = win.y;
    int w = win.w;
    int h = win.h;

    const int CS = 16; // Corner hit-test size
    const int BW = 6;  // Border hit-test width
    const int OUT = 3; // Outer tolerance

    // Check if within outer bounding box
    if (mx < x - OUT || mx > x + w + OUT || my < y || my > y + h + OUT) {
        return RESIZE_NONE;
    }

    // 1. Bottom-Right Corner (SE)
    if (mx >= x + w - CS && mx <= x + w + OUT && my >= y + h - CS && my <= y + h + OUT) {
        return RESIZE_SE;
    }

    // 2. Bottom-Left Corner (SW)
    if (mx >= x - OUT && mx <= x + CS && my >= y + h - CS && my <= y + h + OUT) {
        return RESIZE_SW;
    }

    // 3. Right Edge (E) - below titlebar
    if (mx >= x + w - BW && mx <= x + w + OUT && my >= y + 28 && my < y + h - CS) {
        return RESIZE_E;
    }

    // 4. Left Edge (W) - below titlebar
    if (mx >= x - OUT && mx <= x + BW && my >= y + 28 && my < y + h - CS) {
        return RESIZE_W;
    }

    // 5. Bottom Edge (S) - between corners
    if (my >= y + h - BW && my <= y + h + OUT && mx > x + CS && mx < x + w - CS) {
        return RESIZE_S;
    }

    return RESIZE_NONE;
}

// Layered (fake-blurred) drop shadow toward bottom-right (only when not maximized).
// Kept separate so the fast window-drag path can repaint the shadow without
// re-rendering the (potentially expensive) window contents.
static void draw_window_shadow(const WindowState& win) {
    if (!win.visible || win.minimized || win.maximized) return;
    int x = win.x;
    int y = win.y;
    int w = win.w;
    int h = win.h;
    gfx::fill_rect(x + 8, y + 8, w, h, 0xFF04070D);
    gfx::fill_rect(x + 5, y + 5, w, h, 0xFF070D17);
    gfx::fill_rect(x + 3, y + 3, w, h, 0xFF0A1422);
}

// Draw window frame with EXACTLY ONE set of buttons (Circles on left OR Squares on right)
static void draw_window_frame(const WindowState& win, ButtonStyle style) {
    if (!win.visible || win.minimized) return;

    int x = win.x;
    int y = win.y;
    int w = win.w;
    int h = win.h;

    draw_window_shadow(win);

    // Window interior background
    gfx::fill_rect(x, y, w, h, gfx::Colors::Slate);

    // Titlebar
    uint32_t t_top = win.active ? win.title_color : 0xFF1C2738;
    uint32_t t_bot = win.active ? gfx::Colors::Navy : 0xFF101722;
    gfx::draw_gradient_v(x, y, w, 28, t_top, t_bot);
    // Subtle glossy highlight + separator under the titlebar
    gfx::draw_line(x + 1, y + 1, x + w - 2, y + 1, win.active ? 0x55FFFFFF : 0x22FFFFFF);
    gfx::draw_line(x, y + 28, x + w - 1, y + 28, win.active ? 0xFF2E5A8A : 0xFF16202E);

    // Border and top accent glow
    gfx::draw_rect(x, y, w, h, win.active ? gfx::Colors::Teal : gfx::Colors::LightNavy);
    if (win.active) {
        // Highlight active window borders so resizing affordance is immediately clear
        gfx::draw_line(x + 1, y + 1, x + w - 2, y + 1, 0xFF4DA3FF);
        gfx::draw_line(x + 1, y + 28, x + 1, y + h - 2, 0xFF00B4D8);
        gfx::draw_line(x + w - 2, y + 28, x + w - 2, y + h - 2, 0xFF00B4D8);
        gfx::draw_line(x + 1, y + h - 2, x + w - 2, y + h - 2, 0xFF00B4D8);
    }

    if (style == BUTTONS_CIRCLES) {
        // ==========================================
        // ONLY CIRCLES: macOS Style Traffic Lights
        // ==========================================
        // 1. Red Close Button [X] at (x + 14, y + 14, r = 6)
        gfx::fill_circle(x + 14, y + 14, 6, 0xFFFF4545);
        gfx::draw_circle(x + 14, y + 14, 6, 0xFFCC2222);
        gfx::draw_line(x + 12, y + 12, x + 16, y + 16, gfx::Colors::White);
        gfx::draw_line(x + 16, y + 12, x + 12, y + 16, gfx::Colors::White);

        // 2. Yellow Minimize Button [-] at (x + 34, y + 14, r = 6)
        gfx::fill_circle(x + 34, y + 14, 6, 0xFFFFBB33);
        gfx::draw_circle(x + 34, y + 14, 6, 0xFFCC9900);
        gfx::draw_line(x + 31, y + 14, x + 37, y + 14, gfx::Colors::White);

        // 3. Green Maximize/Restore Button [+] at (x + 54, y + 14, r = 6)
        gfx::fill_circle(x + 54, y + 14, 6, 0xFF00DD66);
        gfx::draw_circle(x + 54, y + 14, 6, 0xFF00AA44);
        if (win.maximized) {
            gfx::draw_rect(x + 51, y + 11, 5, 5, gfx::Colors::White);
            gfx::draw_rect(x + 53, y + 13, 5, 5, gfx::Colors::White);
        } else {
            gfx::draw_line(x + 51, y + 14, x + 57, y + 14, gfx::Colors::White);
            gfx::draw_line(x + 54, y + 11, x + 54, y + 17, gfx::Colors::White);
        }

        // Title text positioned cleanly after circles
        uint32_t t_fg = win.active ? gfx::Colors::White : 0xFF88A0B8;
        gfx::draw_string(win.title, x + 72, y + 6, t_fg);
    } else {
        // ==========================================
        // ONLY SQUARES: Windows Style Controls
        // ==========================================
        if (w >= 240) {
            // 1. Minimize Button [_] at (x + w - 68, y + 4, 18x20)
            gfx::fill_rect(x + w - 68, y + 4, 18, 20, 0xFF14243A);
            gfx::draw_rect(x + w - 68, y + 4, 18, 20, 0xFF244265);
            gfx::draw_line(x + w - 64, y + 17, x + w - 54, y + 17, gfx::Colors::White);

            // 2. Maximize/Restore Button [□] at (x + w - 47, y + 4, 18x20)
            gfx::fill_rect(x + w - 47, y + 4, 18, 20, 0xFF14243A);
            gfx::draw_rect(x + w - 47, y + 4, 18, 20, 0xFF244265);
            if (win.maximized) {
                gfx::draw_rect(x + w - 42, y + 8, 7, 7, gfx::Colors::White);
                gfx::draw_rect(x + w - 39, y + 11, 7, 7, gfx::Colors::White);
            } else {
                gfx::draw_rect(x + w - 42, y + 9, 8, 8, gfx::Colors::White);
            }

            // 3. Close Button [✕] at (x + w - 26, y + 4, 22x20)
            gfx::fill_rect(x + w - 26, y + 4, 22, 20, win.active ? 0xFF882222 : 0xFF351C24);
            gfx::draw_rect(x + w - 26, y + 4, 22, 20, 0xFFAA3333);
            gfx::draw_line(x + w - 19, y + 9, x + w - 11, y + 17, gfx::Colors::White);
            gfx::draw_line(x + w - 11, y + 9, x + w - 19, y + 17, gfx::Colors::White);
        }

        // Title text positioned on the left
        uint32_t t_fg = win.active ? gfx::Colors::White : 0xFF88A0B8;
        gfx::draw_string(win.title, x + 16, y + 6, t_fg);
    }

    // Modern visual Resize Grip in bottom-right corner
    if (!win.maximized) {
        draw_resize_grip(win);
    }
}

// ---------------------------------------------------------------------------
// Desktop application icons: round, gradient-tinted launcher tiles with a
// hand-drawn glyph per application. Kept vector-only so they stay crisp at any
// resolution and cost almost nothing to redraw.
// ---------------------------------------------------------------------------
static void draw_app_icon(int cx, int cy, int r, int kind, bool hovered) {
    uint32_t fill, glyph;
    switch (kind) {
        case 0:  fill = 0xFF0E2A42; glyph = gfx::Colors::Green;   break; // Terminal
        case 1:  fill = 0xFF1E3A8A; glyph = 0xFFBFD7FF;            break; // Browser
        case 2:  fill = 0xFF341B63; glyph = gfx::Colors::Teal;     break; // 3D Engine
        case 4:  fill = 0xFF3A1E3A; glyph = 0xFFF0B6F0;            break; // Task Manager
        case 5:  fill = 0xFF23313A; glyph = 0xFFCFE9FF;            break; // Settings
        default: fill = 0xFF0F3D34; glyph = 0xFFBFFFE8;            break; // System Monitor
    }

    // Soft drop shadow + tinted disc
    gfx::fill_circle(cx + 2, cy + 3, r, 0xFF03060C);
    gfx::fill_circle(cx, cy, r, fill);

    // Inner highlight ring and outer ring give the tile depth
    gfx::draw_circle(cx, cy - 1, r - 4, hovered ? 0xFF9BD1FF : 0xFF294866);
    gfx::draw_circle(cx, cy, r, hovered ? gfx::Colors::Teal : 0xFF37608A);
    if (hovered) {
        gfx::draw_circle(cx, cy + 1, r + 2, gfx::Colors::Teal);
    }

    if (kind == 0) {                    // Terminal prompt ">_"
        gfx::draw_string(">_", cx - 12, cy - 6, glyph);
    } else if (kind == 1) {             // Globe
        int rr = r - 15;
        gfx::draw_circle(cx, cy, rr, glyph);
        gfx::draw_line(cx - rr, cy, cx + rr, cy, glyph);
        gfx::draw_line(cx, cy - rr, cx, cy + rr, glyph);
    } else if (kind == 2) {             // Isometric cube
        int s = r - 17;
        gfx::draw_rect(cx - s, cy - s, 2 * s, 2 * s, glyph);
        gfx::draw_rect(cx - s + 7, cy - s - 7, 2 * s, 2 * s, glyph);
        gfx::draw_line(cx - s, cy - s, cx - s + 7, cy - s - 7, glyph);
        gfx::draw_line(cx + s, cy - s, cx + s + 7, cy - s - 7, glyph);
        gfx::draw_line(cx - s, cy + s, cx - s + 7, cy + s - 7, glyph);
        gfx::draw_line(cx + s, cy + s, cx + s + 7, cy + s - 7, glyph);
    } else if (kind == 3) {             // Monitor (3-bar chart)
        gfx::fill_rect(cx - 15, cy + 3, 9, 13, glyph);
        gfx::fill_rect(cx - 4, cy - 5, 9, 21, glyph);
        gfx::fill_rect(cx + 7, cy - 11, 9, 27, glyph);
    } else if (kind == 4) {             // Checklist (Task Manager)
        for (int r = 0; r < 3; ++r) {
            int ry = cy - 11 + r * 11;
            gfx::draw_rect(cx - 13, ry, 8, 8, glyph);
            gfx::draw_line(cx - 12, ry + 1, cx - 7, ry + 6, glyph);
            gfx::draw_line(cx - 7, ry + 1, cx - 2, ry + 6, glyph);
            gfx::draw_line(cx + 1, ry + 4, cx + 13, ry + 4, glyph);
        }
    } else if (kind == 5) {             // Gear (Settings)
        gfx::draw_circle(cx, cy, r - 12, glyph);
        gfx::draw_circle(cx, cy, r - 19, glyph);
        for (int i = 0; i < 4; ++i) {
            int ox = (i == 0) ? (r - 12) : (i == 1) ? -(r - 12) : 0;
            int oy = (i == 2) ? (r - 12) : (i == 3) ? -(r - 12) : 0;
            gfx::fill_rect(cx + ox - 3, cy + oy - 3, 6, 6, glyph);
        }
    } else if (kind == 6) {             // App grid (App Hub)
        for (int gy = 0; gy < 2; ++gy) {
            for (int gx = 0; gx < 2; ++gx) {
                gfx::draw_rect(cx - 14 + gx * 15, cy - 14 + gy * 15, 12, 12, glyph);
            }
        }
    } else {                            // Bar chart
        gfx::fill_rect(cx - 15, cy + 3, 9, 13, glyph);
        gfx::fill_rect(cx - 4, cy - 5, 9, 21, glyph);
        gfx::fill_rect(cx + 7, cy - 11, 9, 27, glyph);
    }
}

// ============================================================================
// Interactive Terminal Buffer & Data Structures for Window 2
// ============================================================================
constexpr size_t TERM_MAX_LINES = 64;
constexpr size_t TERM_LINE_LEN = 160;

struct TermLine {
    char text[TERM_LINE_LEN];
    uint32_t color;
};

struct GuiTerminal {
    TermLine lines[TERM_MAX_LINES];
    size_t line_count;
    char input_buf[128];
    size_t input_len;
    size_t cursor_pos;

    void init() {
        line_count = 0;
        input_buf[0] = '\0';
        input_len = 0;
        cursor_pos = 0;

        add_line("AetherOS Interactive Desktop Terminal [v1.0]", gfx::Colors::Teal);
        add_line("Type 'help' for commands, 'fetch' for system overview.", gfx::Colors::LightGrey);
        add_line("", gfx::Colors::White);
    }

    void clear() {
        line_count = 0;
    }

    void add_line(const char* str, uint32_t color = gfx::Colors::White) {
        if (!str) return;
        if (line_count < TERM_MAX_LINES) {
            kstd::strncpy(lines[line_count].text, str, TERM_LINE_LEN - 1);
            lines[line_count].text[TERM_LINE_LEN - 1] = '\0';
            lines[line_count].color = color;
            line_count++;
        } else {
            for (size_t i = 0; i < TERM_MAX_LINES - 1; ++i) {
                lines[i] = lines[i + 1];
            }
            kstd::strncpy(lines[TERM_MAX_LINES - 1].text, str, TERM_LINE_LEN - 1);
            lines[TERM_MAX_LINES - 1].text[TERM_LINE_LEN - 1] = '\0';
            lines[TERM_MAX_LINES - 1].color = color;
        }
    }

    void write(const char* multiline_str, uint32_t color = gfx::Colors::White) {
        if (!multiline_str) return;
        char temp[TERM_LINE_LEN];
        size_t col = 0;
        for (size_t i = 0; multiline_str[i] != '\0'; ++i) {
            char ch = multiline_str[i];
            if (ch == '\n') {
                temp[col] = '\0';
                add_line(temp, color);
                col = 0;
            } else if (ch == '\r') {
                // skip
            } else {
                if (col < TERM_LINE_LEN - 1) {
                    temp[col++] = ch;
                } else {
                    temp[col] = '\0';
                    add_line(temp, color);
                    col = 0;
                    temp[col++] = ch;
                }
            }
        }
        if (col > 0) {
            temp[col] = '\0';
            add_line(temp, color);
        }
    }
};

static GuiTerminal term;

static void gui_print_callback(const char* line, uint32_t color) {
    term.add_line(line, color);
}

static char g_initial_browser_url[128] = "about:home";

void set_initial_browser_url(const char* url) {
    if (url && url[0]) {
        kstd::strncpy(g_initial_browser_url, url, sizeof(g_initial_browser_url) - 1);
        g_initial_browser_url[sizeof(g_initial_browser_url) - 1] = '\0';
    }
}

void run_desktop(uint32_t width, uint32_t height, uint32_t target_fps) {
    if (!gfx::init(width, height)) {
        if (!gfx::init(1280, 720)) {
            if (!gfx::init(1024, 768)) {
                printk("[GFX] Failed to initialize graphics mode!\n");
                return;
            }
        }
    }

    width = gfx::get_width();
    height = gfx::get_height();
    mouse::set_bounds(width, height);
    mouse::set_position(width / 2, height / 2);

    // Default configuration:
    // 1. Menu Bar at TOP (avoids being cut off by host OS window borders / panels)
    // 2. Button style: Circles (clean macOS traffic lights on top-left, zero duplicates)
    BarPosition current_bar_pos = BAR_TOP;
    ButtonStyle current_button_style = BUTTONS_CIRCLES;

    // Initialize the application windows. Nothing is launched at boot: the
    // desktop starts empty and every app is spawned on demand from a launcher
    // icon or the AetherOS menu.
    WindowState win1, win2, win3, win4, win5, win6, win7;

    win1.id = 1;
    win1.title = "AetherOS System Monitor";
    win1.title_color = gfx::Colors::LightNavy;
    win1.visible = false;
    win1.minimized = false;
    win1.maximized = false;
    win1.active = false;

    win2.id = 2;
    win2.title = "Interactive Terminal";
    win2.title_color = 0xFF1B3A5A;
    win2.visible = false;
    win2.minimized = false;
    win2.maximized = false;
    win2.active = false;

    win3.id = 3;
    win3.title = "3D Hardware Rendering Engine";
    win3.title_color = 0xFF2A1C50;
    win3.visible = false;
    win3.minimized = false;
    win3.maximized = false;
    win3.active = false;

    win4.id = 4;
    win4.title = "Aether Web Browser";
    win4.title_color = 0xFF14304E;
    win4.visible = false;
    win4.minimized = false;
    win4.maximized = false;
    win4.active = false;

    win5.id = 5;
    win5.title = "Task Manager";
    win5.title_color = 0xFF3A1E3A;
    win5.visible = false;
    win5.minimized = false;
    win5.maximized = false;
    win5.active = false;

    win6.id = 6;
    win6.title = "Settings";
    win6.title_color = 0xFF24323C;
    win6.visible = false;
    win6.minimized = false;
    win6.maximized = false;
    win6.active = false;

    win7.id = 7;
    win7.title = "App Hub";
    win7.title_color = 0xFF1E3A2C;
    win7.visible = false;
    win7.minimized = false;
    win7.maximized = false;
    win7.active = false;

    auto reset_window_layout = [&]() {
        // A left strip (the desktop icon dock) is kept clear so launchers stay
        // visible while the app windows tile the rest of the screen.
        if (width >= 1600) {
            if (current_bar_pos == BAR_TOP) {
                win1.x = 104; win1.y = 48;  win1.w = 540; win1.h = 420;
                win2.x = 104; win2.y = 490; win2.w = 540; win2.h = height - 510;
                win3.x = 664; win3.y = 48;  win3.w = width - 704; win3.h = height - 70;
                win4.x = 664; win4.y = 48;  win4.w = width - 704; win4.h = height - 70;
            } else {
                win1.x = 104; win1.y = 24;  win1.w = 540; win1.h = 420;
                win2.x = 104; win2.y = 466; win2.w = 540; win2.h = height - 510;
                win3.x = 664; win3.y = 24;  win3.w = width - 704; win3.h = height - 70;
                win4.x = 664; win4.y = 24;  win4.w = width - 704; win4.h = height - 70;
            }
        } else if (width >= 1280) {
            if (current_bar_pos == BAR_TOP) {
                win1.x = 104; win1.y = 46;  win1.w = 440; win1.h = 320;
                win2.x = 104; win2.y = 386; win2.w = 440; win2.h = height - 410;
                win3.x = 560; win3.y = 46;  win3.w = width - 590; win3.h = height - 66;
                win4.x = 560; win4.y = 46;  win4.w = width - 590; win4.h = height - 66;
            } else {
                win1.x = 104; win1.y = 20;  win1.w = 440; win1.h = 320;
                win2.x = 104; win2.y = 360; win2.w = 440; win2.h = height - 410;
                win3.x = 560; win3.y = 20;  win3.w = width - 590; win3.h = height - 66;
                win4.x = 560; win4.y = 20;  win4.w = width - 590; win4.h = height - 66;
            }
        } else {
            if (current_bar_pos == BAR_TOP) {
                win1.x = 92;  win1.y = 44;  win1.w = 400; win1.h = 300;
                win2.x = 92;  win2.y = 364; win2.w = 400; win2.h = height - 380;
                win3.x = 512; win3.y = 44;  win3.w = width - 532; win3.h = height - 60;
                win4.x = 512; win4.y = 44;  win4.w = width - 532; win4.h = height - 60;
            } else {
                win1.x = 92;  win1.y = 16;  win1.w = 400; win1.h = 300;
                win2.x = 92;  win2.y = 336; win2.w = 400; win2.h = height - 380;
                win3.x = 512; win3.y = 16;  win3.w = width - 532; win3.h = height - 60;
                win4.x = 512; win4.y = 16;  win4.w = width - 532; win4.h = height - 60;
            }
        }
        // Task Manager floats as a compact centered window (clamped on small screens)
        win5.w = 460; win5.h = 320;
        if (win5.w > static_cast<int>(width) - 40) win5.w = static_cast<int>(width) - 40;
        if (win5.h > static_cast<int>(height) - 60) win5.h = static_cast<int>(height) - 60;
        win5.x = (static_cast<int>(width) - win5.w) / 2;
        win5.y = ((static_cast<int>(height) - win5.h) / 2) + ((current_bar_pos == BAR_TOP) ? 18 : -18);

        win1.saved_x = win1.x; win1.saved_y = win1.y; win1.saved_w = win1.w; win1.saved_h = win1.h;
        win2.saved_x = win2.x; win2.saved_y = win2.y; win2.saved_w = win2.w; win2.saved_h = win2.h;
        win3.saved_x = win3.x; win3.saved_y = win3.y; win3.saved_w = win3.w; win3.saved_h = win3.h;
        win4.saved_x = win4.x; win4.saved_y = win4.y; win4.saved_w = win4.w; win4.saved_h = win4.h;
        win5.saved_x = win5.x; win5.saved_y = win5.y; win5.saved_w = win5.w; win5.saved_h = win5.h;

        // Settings floats as a compact centered window (clamped on small screens)
        win6.w = 560; win6.h = 360;
        if (win6.w > static_cast<int>(width) - 40) win6.w = static_cast<int>(width) - 40;
        if (win6.h > static_cast<int>(height) - 60) win6.h = static_cast<int>(height) - 60;
        win6.x = (static_cast<int>(width) - win6.w) / 2;
        win6.y = ((static_cast<int>(height) - win6.h) / 2) - ((current_bar_pos == BAR_TOP) ? 18 : -18);
        win6.saved_x = win6.x; win6.saved_y = win6.y; win6.saved_w = win6.w; win6.saved_h = win6.h;

        // App Hub: a wider catalog window, centered and clamped on small screens.
        win7.w = 640; win7.h = 520;
        if (win7.w > static_cast<int>(width) - 40) win7.w = static_cast<int>(width) - 40;
        if (win7.h > static_cast<int>(height) - 60) win7.h = static_cast<int>(height) - 60;
        win7.x = (static_cast<int>(width) - win7.w) / 2;
        win7.y = ((static_cast<int>(height) - win7.h) / 2) + ((current_bar_pos == BAR_TOP) ? 18 : -18);
        win7.saved_x = win7.x; win7.saved_y = win7.y; win7.saved_w = win7.w; win7.saved_h = win7.h;
    };

    reset_window_layout();
    term.init();

    // Z-Order Stack: win_stack[0] bottom, win_stack[NWIN-1] top (active)
    WindowState* win_stack[7] = { &win1, &win2, &win3, &win4, &win5, &win6, &win7 };
    win1.active = false;
    win2.active = false;
    win3.active = false;
    win4.active = false;
    win5.active = false;
    win6.active = false;
    win7.active = false;

    // Refresh rate mode list: 60Hz, 120Hz, 144Hz, 240Hz, 0 (MAX / Unlocked)
    const uint32_t fps_modes[5] = { 60, 120, 144, 240, 0 };
    const char* const fps_labels[5] = { "60 Hz", "120 Hz", "144 Hz", "240 Hz", "MAX FPS" };
    int current_fps_idx = 2; // Default 144 Hz
    if (target_fps == 60)  current_fps_idx = 0;
    else if (target_fps == 120) current_fps_idx = 1;
    else if (target_fps == 144) current_fps_idx = 2;
    else if (target_fps == 240) current_fps_idx = 3;
    else if (target_fps == 0)   current_fps_idx = 4;

    struct BoundingBox {
        int min_x, min_y, max_x, max_y;
        bool valid;
    };
    BoundingBox win3_cube_prev = { 0, 0, 0, 0, false };
    BoundingBox win3_cube_curr = { 0, 0, 0, 0, false };
    bool win3_hud_dirty = true;
    uint32_t win3_last_hud_fps = 0xFFFFFFFF;
    int win3_last_hud_mode = -1;

    int last_clock_second = -1;
    uint32_t menu_last_fps = 0xFFFFFFFF;

    // Settings / updater state
    char settings_status[192] = "Idle. Check for or install the latest kernel.";
    int settings_status_kind = 0; // 0 neutral, 1 ok, 2 error

    // App Hub / independent app updates
    char hub_status[192] = "Press Refresh Catalog to fetch available apps.";
    int hub_status_kind = 0;
    int hub_selected = 0;
    struct HubBuiltin { const char* name; int icon; int win_id; };
    const HubBuiltin hub_builtins[] = {
        { "Terminal", 0, 2 }, { "Browser", 1, 4 }, { "3D", 2, 3 },
        { "Monitor", 3, 1 }, { "Tasks", 4, 5 }, { "Settings", 5, 6 },
        { "App Hub", 6, 7 },
    };
    constexpr int HUB_BUILTINS = 7;
    if (apphub::count() > 0) {
        char num[8];
        kstd::strcpy(hub_status, "Catalog ready - ");
        kstd::utoa(static_cast<uint64_t>(apphub::count()), num, 10);
        kstd::strcpy(hub_status + kstd::strlen(hub_status), num);
        kstd::strcpy(hub_status + kstd::strlen(hub_status), " apps");
        hub_status_kind = 1;
    }

    auto cycle_fps = [&]() {
        current_fps_idx = (current_fps_idx + 1) % 5;
        win3_hud_dirty = true;
    };

    // 3D Cube Edges
    const int cube_edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };

    int angle_x = 0;
    int angle_y = 0;
    int angle_z = 0;

    // Web Browser Engine State
    browser::Document* browser_doc = browser::create_document();
    char browser_history[16][128];
    int browser_history_count = 0;
    int browser_history_pos = -1;
    char browser_url_input[128];
    kstd::strncpy(browser_url_input, g_initial_browser_url, sizeof(browser_url_input) - 1);
    browser_url_input[sizeof(browser_url_input) - 1] = '\0';
    int browser_url_len = static_cast<int>(kstd::strlen(browser_url_input));
    bool browser_url_active = false;
    char browser_hover_url[128] = {0};
    int prev_browser_hover_box = -1;
    bool browser_scroll_drag = false;
    int browser_scroll_grab = 0;
    bool win4_dirty = true;
    bool full_redraw_needed = true;

    // Per-window content dirty flags so idle windows skip redraw + MMIO flush
    bool win1_dirty = true;
    bool win2_dirty = true;
    int prev_win1_mx = -1;
    int prev_win1_my = -1;
    uint64_t last_win1_ms = 0;
    uint32_t last_win1_fps = 0xFFFFFFFF;
    int last_term_blink = -1;

    auto browser_navigate = [&](const char* url, bool push_history = true) {
        if (!url || !url[0]) return;
        int vw = win4.w - 20;
        if (vw < 100) vw = 100;
        browser::load_url(browser_doc, url, vw);
        kstd::strncpy(browser_url_input, browser_doc->current_url, sizeof(browser_url_input) - 1);
        browser_url_input[sizeof(browser_url_input) - 1] = '\0';
        browser_url_len = static_cast<int>(kstd::strlen(browser_url_input));
        browser_hover_url[0] = '\0';
        browser_url_active = false;
        if (push_history) {
            if (browser_history_pos + 1 < 16) {
                browser_history_pos++;
                kstd::strncpy(browser_history[browser_history_pos], browser_doc->current_url, 127);
                browser_history[browser_history_pos][127] = '\0';
                browser_history_count = browser_history_pos + 1;
            }
        }
        win4_dirty = true;
        full_redraw_needed = true;
    };

    browser_navigate(g_initial_browser_url, true);

    // Mouse drag & resize state
    int dragging_win = 0;
    int drag_offset_x = 0;
    int drag_offset_y = 0;

    // Fast window-drag path: instead of re-rendering and swapping the whole
    // desktop every frame, repaint only the rectangle covering the window's old
    // and new footprints, then swap just that rectangle.
    int drag_prev_x = 0, drag_prev_y = 0, drag_prev_w = 0, drag_prev_h = 0;
    bool drag_prev_valid = false;
    int drag_dmg_x = 0, drag_dmg_y = 0, drag_dmg_w = 0, drag_dmg_h = 0;
    bool drag_dmg_valid = false;

    // Begin dragging a window: remember the pointer grab offset and seed the
    // damaged region with the window's current footprint so the very first drag
    // frame erases the window's pre-drag position.
    auto begin_drag = [&](WindowState* win, int cx, int cy) {
        dragging_win = win->id;
        drag_offset_x = cx - win->x;
        drag_offset_y = cy - win->y;
        drag_prev_x = win->x - 10;
        drag_prev_y = win->y - 10;
        drag_prev_w = win->w + 20;
        drag_prev_h = win->h + 20;
        drag_prev_valid = true;
    };

    // Cached wallpaper (gradient + glow + grid) so full redraws don't recompute
    // a per-pixel gradient over the whole screen every frame.
    uint32_t* wp_cache = nullptr;
    bool wp_cache_valid = false;

    int resizing_win = 0;
    ResizeZone resizing_zone = RESIZE_NONE;
    int resize_start_mx = 0;
    int resize_start_my = 0;
    int resize_start_x = 0;
    int resize_start_w = 0;
    int resize_start_h = 0;
    uint64_t last_title_click = 0;
    int last_clicked_win = 0;

    bool start_menu_open = false;
    bool prev_left_click = false;

    // Cursor tracking & shape
    CursorType current_cursor_type = CURSOR_POINTER;
    CursorType prev_cursor_type = CURSOR_POINTER;
    int prev_draw_x = -1;
    int prev_draw_y = -1;
    int prev_draw_w = 0;
    int prev_draw_h = 0;
    cursor_has_saved_bg = false;

    // Performance tracking
    uint64_t last_fps_time = pit::uptime_ms();
    uint64_t frame_count = 0;
    uint32_t measured_fps = fps_modes[current_fps_idx] ? fps_modes[current_fps_idx] : 144;
    uint32_t frame_time_ms = 1;

    // Window Management Actions
    constexpr int NWIN = 7;          // total windows
    constexpr int TOP = NWIN - 1;    // top of the z-order stack

    auto bring_to_front = [&](WindowState* win) {
        int idx = -1;
        for (int i = 0; i < NWIN; ++i) {
            if (win_stack[i] == win) {
                idx = i;
                break;
            }
        }
        if (idx >= 0 && idx < TOP) {
            for (int i = idx; i < TOP; ++i) {
                win_stack[i] = win_stack[i + 1];
            }
            win_stack[TOP] = win;
        }
        win1.active = (&win1 == win_stack[TOP]);
        win2.active = (&win2 == win_stack[TOP]);
        win3.active = (&win3 == win_stack[TOP]);
        win4.active = (&win4 == win_stack[TOP]);
        win5.active = (&win5 == win_stack[TOP]);
        win6.active = (&win6 == win_stack[TOP]);
        win7.active = (&win7 == win_stack[TOP]);
    };

    auto close_window = [&](WindowState* win) {
        win->visible = false;
        win->minimized = false;
        win->active = false;
        // Focus the topmost remaining running window (if any).
        for (int i = TOP; i >= 0; --i) {
            if (win_stack[i]->visible && !win_stack[i]->minimized) {
                bring_to_front(win_stack[i]);
                break;
            }
        }
        full_redraw_needed = true;
    };

    auto minimize_window = [&](WindowState* win) {
        win->minimized = true;
        win->active = false;
        sound::play_gui_cue_minimize();
        for (int i = TOP; i >= 0; --i) {
            if (win_stack[i]->visible && !win_stack[i]->minimized) {
                bring_to_front(win_stack[i]);
                break;
            }
        }
        full_redraw_needed = true;
    };

    auto maximize_toggle_window = [&](WindowState* win) {
        if (!win->maximized) {
            win->saved_x = win->x; win->saved_y = win->y;
            win->saved_w = win->w; win->saved_h = win->h;
            win->x = 0;
            win->y = (current_bar_pos == BAR_TOP) ? 38 : 0;
            win->w = width;
            win->h = height - 40;
            win->maximized = true;
            sound::play_gui_cue_maximize();
        } else {
            win->x = win->saved_x; win->y = win->saved_y;
            win->w = win->saved_w; win->h = win->saved_h;
            win->maximized = false;
            sound::play_gui_cue_minimize();
        }
        bring_to_front(win);
        full_redraw_needed = true;
    };

    auto launch_win3 = [&]() {
        win3.visible = true;
        win3.minimized = false;
        win3_hud_dirty = true;
        bring_to_front(&win3);
        sound::play_gui_cue_maximize();
        full_redraw_needed = true;
    };

    auto launch_win4 = [&]() {
        win4.visible = true;
        win4.minimized = false;
        bring_to_front(&win4);
        sound::play_gui_cue_maximize();
        full_redraw_needed = true;
    };

    auto launch_win5 = [&]() {
        win5.visible = true;
        win5.minimized = false;
        bring_to_front(&win5);
        sound::play_gui_cue_maximize();
        full_redraw_needed = true;
    };

    auto launch_win6 = [&]() {
        win6.visible = true;
        win6.minimized = false;
        bring_to_front(&win6);
        sound::play_gui_cue_maximize();
        full_redraw_needed = true;
    };

    auto launch_win7 = [&]() {
        win7.visible = true;
        win7.minimized = false;
        bring_to_front(&win7);
        sound::play_gui_cue_maximize();
        full_redraw_needed = true;
    };

    // App Hub entries for built-in windows use the same launch path as the
    // dock/Start menu, so every app is reachable through the hub.
    auto open_builtin_app = [&](int win_id) {
        WindowState* w = win_id == 1 ? &win1 : win_id == 2 ? &win2 :
                         win_id == 3 ? &win3 : win_id == 4 ? &win4 :
                         win_id == 5 ? &win5 : win_id == 6 ? &win6 : &win7;
        w->visible = true;
        w->minimized = false;
        bring_to_front(w);
        sound::play_gui_cue_maximize();
        win1_dirty = true;
        win2_dirty = true;
        win3_hud_dirty = true;
        win4_dirty = true;
        full_redraw_needed = true;
    };

    // Browser viewport geometry (mirrors draw_win4_content layout metrics)
    auto browser_geom = [&](const WindowState& win, int& vp_x, int& vp_y, int& vp_w, int& vp_h,
                            int& tb_y, int& bm_y) {
        int win_in_x = win.x + 2;
        int win_in_y = win.y + 28;
        int win_in_w = win.w - 4;
        tb_y = win_in_y;
        bm_y = win_in_y + 28;
        int bm_h = 24;
        int sb_stat_y = win.y + win.h - 22;
        vp_x = win_in_x;
        vp_y = bm_y + bm_h;
        vp_w = win_in_w - 16;
        vp_h = sb_stat_y - vp_y;
    };

    // Compute the browser vertical scrollbar metrics (mirrors draw_win4_content)
    auto browser_scroll_metrics = [&](const WindowState& win, int vp_y, int vp_h,
                                      int& sb_x, int& sb_y, int& sb_w, int& sb_h,
                                      int& thumb_h, int& max_scroll) {
        int doc_h = (browser_doc->page_height > vp_h) ? browser_doc->page_height : vp_h;
        max_scroll = doc_h - vp_h;
        if (max_scroll < 0) max_scroll = 0;
        thumb_h = (vp_h * vp_h) / doc_h;
        if (thumb_h < 18) thumb_h = 18;
        if (thumb_h > vp_h) thumb_h = vp_h;
        sb_x = win.x + win.w - 16;
        sb_y = vp_y;
        sb_w = 14;
        sb_h = vp_h;
    };

    // Set the browser scroll offset from a desired thumb top position.
    auto browser_set_scroll_from_thumb = [&](int thumb_top, int sb_y, int sb_h,
                                             int thumb_h, int max_scroll) {
        int track = sb_h - thumb_h;
        int pos = thumb_top - sb_y;
        if (pos < 0) pos = 0;
        if (pos > track) pos = track;
        browser_doc->scroll_y = (track > 0) ? (pos * max_scroll) / track : 0;
        win4_dirty = true;
    };

    // True when a higher window in the Z-stack overlaps the given window.
    auto win_obscured = [&](const WindowState& w) -> bool {
        for (int i = TOP; i >= 0; --i) {
            WindowState* o = win_stack[i];
            if (o == &w) break;
            if (!o->visible || o->minimized) continue;
            if (o->x < w.x + w.w && o->x + o->w > w.x &&
                o->y < w.y + w.h && o->y + o->h > w.y) {
                return true;
            }
        }
        return false;
    };

    // Handle a mouse click inside the browser window. Returns true if consumed.
    auto handle_browser_click = [&](int cx, int cy) -> bool {
        if (!win4.visible || win4.minimized) return false;
        const WindowState& win = win4;
        int vp_x, vp_y, vp_w, vp_h, tb_y, bm_y;
        browser_geom(win, vp_x, vp_y, vp_w, vp_h, tb_y, bm_y);

        // 1. Toolbar: Back / Forward / Reload / Home / Address Bar / Go
        if (cy >= tb_y && cy < tb_y + 28) {
            if (cx >= win.x + 6 && cx < win.x + 30) {
                if (browser_history_pos > 0) {
                    browser_history_pos--;
                    browser_navigate(browser_history[browser_history_pos], false);
                }
                return true;
            }
            if (cx >= win.x + 34 && cx < win.x + 58) {
                if (browser_history_pos + 1 < browser_history_count) {
                    browser_history_pos++;
                    browser_navigate(browser_history[browser_history_pos], false);
                }
                return true;
            }
            if (cx >= win.x + 62 && cx < win.x + 86) {
                browser_navigate(browser_doc->current_url, false);
                return true;
            }
            if (cx >= win.x + 90 && cx < win.x + 114) {
                browser_navigate("about:home", true);
                return true;
            }
            int ab_x = win.x + 120;
            int ab_w = win.w - 176;
            if (ab_w < 50) ab_w = 50;
            if (cx >= ab_x && cx < ab_x + ab_w) {
                browser_url_active = true;
                win4_dirty = true;
                return true;
            }
            int go_x = win.x + win.w - 50;
            if (cx >= go_x && cx < go_x + 44) {
                browser_navigate(browser_url_input, true);
                return true;
            }
            return true;
        }

        // 2. Bookmarks bar
        if (cy >= bm_y && cy < bm_y + 24) {
            struct BookmarkChip { int x; int w; const char* target; };
            const BookmarkChip chips[] = {
                { 8,   70, "about:home" },
                { 82,  68, "/index.html" },
                { 154, 58, "/docs.html" },
                { 216, 58, "/acid.html" },
                { 278, 58, "/news.html" },
                { 340, 68, "about:about" },
            };
            for (const BookmarkChip& chip : chips) {
                int bx = win.x + chip.x;
                if (bx + chip.w > win.x + win.w - 10) continue;
                if (cx >= bx && cx < bx + chip.w) {
                    browser_navigate(chip.target, true);
                    return true;
                }
            }
            return true;
        }

        // 3. Vertical scrollbar (press to jump, then drag the thumb)
        int sb_x, sb_y, sb_w, sb_h, thumb_h, max_scroll;
        browser_scroll_metrics(win, vp_y, vp_h, sb_x, sb_y, sb_w, sb_h, thumb_h, max_scroll);
        if (cx >= sb_x && cx < sb_x + sb_w && cy >= sb_y && cy < sb_y + sb_h) {
            int thumb_top = sb_y + (max_scroll > 0 ? (browser_doc->scroll_y * (sb_h - thumb_h)) / max_scroll : 0);
            if (cy >= thumb_top && cy <= thumb_top + thumb_h) {
                browser_scroll_grab = cy - thumb_top;
            } else {
                browser_scroll_grab = thumb_h / 2;
            }
            browser_scroll_drag = true;
            browser_set_scroll_from_thumb(cy - browser_scroll_grab, sb_y, sb_h, thumb_h, max_scroll);
            return true;
        }

        // 4. Web viewport: hit-test hyperlinks and buttons
        if (vp_w > 20 && vp_h > 20 &&
            cx >= vp_x && cx < vp_x + vp_w && cy >= vp_y && cy < vp_y + vp_h) {
            const browser::LayoutBox* box = browser::hit_test(*browser_doc, cx - vp_x, cy - vp_y, browser_doc->scroll_y);
            if (box) {
                if (box->is_clickable && box->link_url[0] != '\0') {
                    browser_navigate(box->link_url, true);
                } else if (box->is_button) {
                    browser_navigate(browser_doc->current_url, false);
                }
                return true;
            }
        }
        return false;
    };

    auto cycle_focus = [&]() {
        for (int i = 0; i < TOP; ++i) {
            if (win_stack[i]->visible && !win_stack[i]->minimized) {
                bring_to_front(win_stack[i]);
                return;
            }
        }
        bring_to_front(win_stack[0]);
    };

    // --- Task Manager (win5) ------------------------------------------------
    struct TmEntry { WindowState* win; const char* name; };
    const TmEntry tm_tasks[NWIN] = {
        { &win1, "System Monitor" },
        { &win2, "Terminal" },
        { &win3, "3D Engine" },
        { &win4, "Web Browser" },
        { &win5, "Task Manager" },
        { &win6, "Settings" },
        { &win7, "App Hub" },
    };

    auto tm_row_y = [&](const WindowState& w, int i) { return w.y + 52 + i * 30; };

    auto draw_win5_content = [&](const WindowState& win) {
        int cx = win.x + 2;
        int cy = win.y + 28;
        int cw = win.w - 4;
        int ch = win.h - 28 - 22;
        gfx::fill_rect(cx, cy, cw, ch, 0xFF141428);
        gfx::fill_rect(cx, cy, cw, 22, 0xFF232345);
        gfx::draw_string("PID", cx + 20, cy + 4, gfx::Colors::LightGrey);
        gfx::draw_string("TASK", cx + 60, cy + 4, gfx::Colors::LightGrey);
        gfx::draw_string("STATE", cx + cw - 186, cy + 4, gfx::Colors::LightGrey);
        gfx::draw_line(cx, cy + 22, cx + cw - 1, cy + 22, 0xFF3A3A66);

        int running = 0;
        for (int i = 0; i < NWIN; ++i) {
            WindowState* t = tm_tasks[i].win;
            int ry = tm_row_y(win, i);
            if (ry + 30 > cy + ch) break;
            gfx::fill_rect(cx + 1, ry, cw - 2, 29, (i % 2) ? 0xFF171730 : 0xFF141428);
            char pid[8];
            kstd::itoa(t->id, pid, 10);
            gfx::draw_string(pid, cx + 24, ry + 7, gfx::Colors::Teal);
            gfx::draw_string(tm_tasks[i].name, cx + 60, ry + 7, gfx::Colors::White);

            bool vis = t->visible && !t->minimized;
            const char* state;
            uint32_t state_col;
            if (vis) { state = "Running"; state_col = gfx::Colors::Green; ++running; }
            else if (t->visible && t->minimized) { state = "Suspended"; state_col = gfx::Colors::Yellow; }
            else { state = "Stopped"; state_col = 0xFF8888AA; }
            gfx::draw_string(state, cx + cw - 186, ry + 7, state_col);

            if (t->id != 5 && t->visible) {
                int bx = cx + cw - 62, by = ry + 4;
                gfx::fill_rect(bx, by, 54, 21, 0xFF7A1F1F);
                gfx::draw_rect(bx, by, 54, 21, 0xFFE05555);
                gfx::draw_string("Kill", bx + 13, by + 4, gfx::Colors::White);
            }
        }

        int sy = win.y + win.h - 22;
        gfx::fill_rect(cx, sy, cw, 22, 0xFF232345);
        char buf[48];
        kstd::strcpy(buf, "Running tasks: ");
        kstd::itoa(running, buf + kstd::strlen(buf), 10);
        gfx::draw_string(buf, cx + 10, sy + 4, gfx::Colors::LightGrey);
    };

    // Returns true if a Kill control consumed the click.
    auto handle_tm_click = [&](int mx, int my) -> bool {
        if (!win5.visible || win5.minimized) return false;
        for (int i = 0; i < NWIN; ++i) {
            WindowState* t = tm_tasks[i].win;
            if (t->id == 5 || !t->visible) continue;
            int ry = tm_row_y(win5, i);
            int bx = win5.x + 2 + (win5.w - 4) - 62;
            if (mx >= bx && mx < bx + 54 && my >= ry + 4 && my < ry + 25) {
                close_window(t);
                return true;
            }
        }
        return false;
    };

    auto handle_taskbar_click = [&](WindowState* win) {
        if (!win->visible) {
            win->visible = true;
            win->minimized = false;
            if (win->id == 3) win3_hud_dirty = true;
            if (win->id == 4) win4_dirty = true;
            sound::play_gui_cue_maximize();
            bring_to_front(win);
        } else if (win->minimized) {
            win->minimized = false;
            if (win->id == 3) win3_hud_dirty = true;
            if (win->id == 4) win4_dirty = true;
            sound::play_gui_cue_maximize();
            bring_to_front(win);
        } else {
            bring_to_front(win);
        }
        full_redraw_needed = true;
    };

    // --- Settings (win6) ----------------------------------------------------
    auto settings_btn_geom = [&](const WindowState& win, int& check_x, int& install_x, int& restart_x, int& by, int& bw, int& bh) {
        bw = 156;
        bh = 34;
        by = win.y + 206;
        check_x = win.x + 20;
        install_x = check_x + bw + 16;
        restart_x = install_x + bw + 16;
    };

    auto draw_win6_content = [&](const WindowState& win) {
        int cx = win.x + 2;
        int cy = win.y + 28;
        int cw = win.w - 4;
        int ch = win.h - 28 - 22;
        gfx::fill_rect(cx, cy, cw, ch, 0xFF141B22);
        gfx::fill_rect(cx, cy, cw, 26, 0xFF1E2A33);
        gfx::draw_string("Software Update", win.x + 20, win.y + 38, gfx::Colors::Teal);
        gfx::draw_line(cx, cy + 26, cx + cw - 1, cy + 26, 0xFF2E4656);

        char buf[192];
        int ksz = fat16::file_size("AETHEROSBIN");
        buf[0] = '\0';
        kstd::strcpy(buf, "Kernel image: AETHEROS.BIN");
        if (ksz > 0) {
            kstd::strcpy(buf + kstd::strlen(buf), " (");
            kstd::itoa(static_cast<uint64_t>(ksz), buf + kstd::strlen(buf), 10);
            kstd::strcpy(buf + kstd::strlen(buf), " bytes)");
        } else {
            kstd::strcpy(buf + kstd::strlen(buf), " (not found)");
        }
        gfx::draw_string(buf, win.x + 20, win.y + 74, gfx::Colors::LightGrey);

        // Version string displayed next to the kernel image info.
        gfx::draw_string("Version: v1.0.0",
                         win.x + 20 + kstd::strlen(buf) * 8 + 8,
                         win.y + 74, 0xFFB8C8D4);

        const updater::Config& cfg = updater::config();
        buf[0] = '\0';
        kstd::strcpy(buf, "Update source: ");
        kstd::strcpy(buf + kstd::strlen(buf), cfg.host);
        kstd::strcpy(buf + kstd::strlen(buf), ":");
        kstd::itoa(static_cast<uint64_t>(cfg.port), buf + kstd::strlen(buf), 10);
        kstd::strcpy(buf + kstd::strlen(buf), cfg.path);
        gfx::draw_string(buf, win.x + 20, win.y + 98, 0xFF9FB8CC);

        gfx::draw_string("Installing replaces the boot kernel and keeps a rollback snapshot.",
                         win.x + 20, win.y + 126, 0xFF7F97A8);

        int check_x, install_x, restart_x, by, bw, bh;
        settings_btn_geom(win, check_x, install_x, restart_x, by, bw, bh);

        bool can_update = fat16::mounted() && aetherfs::mounted();
        gfx::fill_rect(check_x, by, bw, bh, can_update ? 0xFF1F4A63 : 0xFF2A2F33);
        gfx::draw_rect(check_x, by, bw, bh, can_update ? gfx::Colors::Teal : 0xFF4A555C);
        gfx::draw_string("Check for Updates", check_x + 16, by + 10,
                         can_update ? gfx::Colors::White : 0xFF777F85);

        gfx::fill_rect(install_x, by, bw, bh, can_update ? 0xFF2E7D32 : 0xFF2A2F33);
        gfx::draw_rect(install_x, by, bw, bh, can_update ? 0xFF66D46B : 0xFF4A555C);
        gfx::draw_string("Install Update", install_x + 28, by + 10,
                         can_update ? gfx::Colors::White : 0xFF777F85);

        gfx::fill_rect(restart_x, by, bw, bh, 0xFF2E7D32);
        gfx::draw_rect(restart_x, by, bw, bh, 0xFF66D46B);
        gfx::draw_string("Restart", restart_x + 50, by + 10, gfx::Colors::White);

        uint32_t scol = (settings_status_kind == 1) ? gfx::Colors::Green
                       : (settings_status_kind == 2) ? 0xFFFF7070
                       : 0xFFB8C8D4;
        gfx::draw_string(settings_status, win.x + 20, by + bh + 16, scol);

        int sy = win.y + win.h - 22;
        gfx::fill_rect(cx, sy, cw, 22, 0xFF1E2A33);
        gfx::draw_string("AetherOS Settings", cx + 10, sy + 4, 0xFF8FA6B6);
    };

    // Returns true if a Settings control consumed the click.
    auto handle_settings_click = [&](int mx, int my) -> bool {
        if (!win6.visible || win6.minimized) return false;
        int check_x, install_x, restart_x, by, bw, bh;
        settings_btn_geom(win6, check_x, install_x, restart_x, by, bw, bh);

        if (mx >= check_x && mx < check_x + bw && my >= by && my < by + bh) {
            char msg[128];
            msg[0] = '\0';
            updater::Result r = updater::fetch_verify(msg, sizeof(msg));
            if (r == updater::Result::Ok)
                kstd::strcpy(settings_status, "Update available - ");
            else if (r == updater::Result::UpToDate)
                kstd::strcpy(settings_status, "Up to date - ");
            else
                kstd::strcpy(settings_status, "Check failed - ");
            kstd::strcpy(settings_status + kstd::strlen(settings_status),
                         (r == updater::Result::Ok || r == updater::Result::UpToDate)
                             ? (msg[0] ? msg : "done") : updater::result_name(r));
            settings_status_kind = (r == updater::Result::Ok || r == updater::Result::UpToDate) ? 1 : 2;
            full_redraw_needed = true;
            return true;
        }
        if (mx >= install_x && mx < install_x + bw && my >= by && my < by + bh) {
            char msg[128];
            msg[0] = '\0';
            updater::Result r = updater::apply(msg, sizeof(msg));
            if (r == updater::Result::Ok) {
                kstd::strcpy(settings_status, "Update installed - reboot to apply");
                settings_status_kind = 1;
            } else if (r == updater::Result::UpToDate) {
                kstd::strcpy(settings_status, "Already up to date - ");
                kstd::strcpy(settings_status + kstd::strlen(settings_status), msg[0] ? msg : "no change");
                settings_status_kind = 1;
            } else {
                kstd::strcpy(settings_status, "Update failed - ");
                kstd::strcpy(settings_status + kstd::strlen(settings_status), updater::result_name(r));
                settings_status_kind = 2;
            }
            full_redraw_needed = true;
            return true;
        }
        if (mx >= restart_x && mx < restart_x + bw && my >= by && my < by + bh) {
            // Restart: reset the CPU via the 8042 keyboard controller.
            arch::outb(0x64, 0xFE);
            asm volatile("lidt 0; int $3");
            return true;
        }
        return false;
    };

    // --- App Hub (win7) -----------------------------------------------------
    // Apps are HTML/JS bundles stored in AetherFS, so installing or updating
    // one never touches the kernel image.
    auto hub_geom = [&](const WindowState& win, int& lx, int& ly, int& lw, int& rh,
                        int& rows, int& by, int& bw, int& bh,
                        int& rx, int& ix, int& ox,
                        int& bstart, int& bcell, int& bsize) {
        lx = win.x + 16;
        lw = win.w - 32;
        bcell = lw / HUB_BUILTINS;
        if (bcell < 44) bcell = 44;
        bsize = bcell / 3;
        if (bsize > 20) bsize = 20;
        if (bsize < 12) bsize = 12;
        bstart = win.y + 118;
        ly = bstart + 2 * bsize + 44;
        by = win.y + win.h - 60;
        bh = 28;
        bw = (lw - 16) / 3;
        if (bw < 60) bw = 60;
        rx = lx;
        ix = lx + bw + 8;
        ox = lx + 2 * (bw + 8);
        int avail = by - 26 - ly;
        rows = avail / 52;
        if (rows < 1) rows = 1;
        if (rows > 5) rows = 5;
        rh = 52;
    };

    auto draw_win7_content = [&](const WindowState& win) {
        int lx, ly, lw, rh, rows, by, bw, bh, rx, ix, ox, bstart, bcell, bsize;
        hub_geom(win, lx, ly, lw, rh, rows, by, bw, bh, rx, ix, ox, bstart, bcell, bsize);

        int cx = win.x + 2;
        int cy = win.y + 28;
        int cw = win.w - 4;
        int ch = win.h - 28;
        gfx::fill_rect(cx, cy, cw, ch, 0xFF101820);
        gfx::fill_rect(cx, cy, cw, 64, 0xFF16302A);
        gfx::draw_line(cx, cy + 64, cx + cw, cy + 64, 0xFF2A5C4A);

        gfx::draw_string("APP HUB", win.x + 18, win.y + 38, gfx::Colors::Teal, 0, true);
        gfx::draw_string("Built-in apps and downloadable bundles.", win.x + 18, win.y + 62,
                         0xFF8FA6B8);
        gfx::draw_string("BUILT-IN", lx, win.y + 98, 0xFF7F98AD);
        for (int i = 0; i < HUB_BUILTINS; ++i) {
            int icx = lx + i * bcell + bcell / 2;
            int icy = bstart + bsize;
            draw_app_icon(icx, icy, bsize, hub_builtins[i].icon, false);
            int tw = static_cast<int>(kstd::strlen(hub_builtins[i].name)) * 8;
            gfx::draw_string(hub_builtins[i].name, icx - tw / 2,
                             bstart + 2 * bsize + 4, 0xFFD6E4F0);
        }

        gfx::draw_string("DOWNLOADABLE APPS", lx, ly - 20, 0xFF7F98AD);

        int n = apphub::count();
        if (n == 0) {
            gfx::draw_string("No app catalog loaded.", lx, ly + 16, gfx::Colors::LightGrey);
            gfx::draw_string("Press Refresh Catalog to fetch available apps.", lx, ly + 40,
                             0xFF8FA6B8);
        }

        for (int i = 0; i < rows && i < n; ++i) {
            const apphub::App* a = apphub::get(i);
            if (!a) break;
            int ry = ly + i * rh;
            bool sel = (i == hub_selected);

            gfx::fill_rect(lx, ry, lw, rh - 6, sel ? 0xFF1E3A5C : 0xFF0D1A26);
            gfx::draw_rect(lx, ry, lw, rh - 6, sel ? gfx::Colors::Teal : 0xFF24384A);

            gfx::draw_string(a->name, lx + 10, ry + 6,
                             sel ? gfx::Colors::White : 0xFFD6E4F0, 0, true);

            bool has_update = a->installed && a->version[0] &&
                              kstd::strcmp(a->installed_version, a->version) != 0;
            const char* badge = !a->installed ? "[ Get ]"
                              : (has_update ? "[ Update ]" : "[ Installed ]");
            uint32_t badge_col = !a->installed ? gfx::Colors::Cyan
                               : (has_update ? gfx::Colors::Yellow : gfx::Colors::Green);
            int bl = static_cast<int>(kstd::strlen(badge)) * 8;
            gfx::draw_string(badge, lx + lw - bl - 10, ry + 6, badge_col);

            char ver_line[40];
            kstd::strcpy(ver_line, "v");
            kstd::strcpy(ver_line + kstd::strlen(ver_line),
                         a->version[0] ? a->version : "?");
            if (a->installed && a->installed_version[0]) {
                kstd::strcpy(ver_line + kstd::strlen(ver_line), "  (installed ");
                kstd::strcpy(ver_line + kstd::strlen(ver_line), a->installed_version);
                kstd::strcpy(ver_line + kstd::strlen(ver_line), ")");
            }
            gfx::draw_string(ver_line, lx + 10, ry + 26, 0xFF7F98AD);

            if (a->desc[0]) {
                char desc[40];
                kstd::strncpy(desc, a->desc, sizeof(desc) - 1);
                desc[sizeof(desc) - 1] = '\0';
                int dl = static_cast<int>(kstd::strlen(desc)) * 8;
                int dx = lx + lw - dl - 10;
                if (dx > lx + 10 + 160) gfx::draw_string(desc, dx, ry + 26, 0xFF5F7A90);
            }
        }

        uint32_t scol = (hub_status_kind == 1) ? gfx::Colors::Green
                       : (hub_status_kind == 2) ? 0xFFFF7070
                       : 0xFFB8C6D4;
        gfx::draw_string(hub_status, lx, by - 22, scol);

        bool has_sel = (hub_selected >= 0 && hub_selected < n);
        bool sel_installed = has_sel && apphub::get(hub_selected) &&
                             apphub::get(hub_selected)->installed;

        gfx::fill_rect(rx, by, bw, bh, 0xFF1B3A5A);
        gfx::draw_rect(rx, by, bw, bh, gfx::Colors::Cyan);
        gfx::draw_string("Refresh Catalog", rx + 10, by + 8, gfx::Colors::White);

        gfx::fill_rect(ix, by, bw, bh, has_sel ? 0xFF2A5C3A : 0xFF1A2A22);
        gfx::draw_rect(ix, by, bw, bh, has_sel ? gfx::Colors::Green : 0xFF35513F);
        gfx::draw_string("Install / Update", ix + 8, by + 8,
                         has_sel ? gfx::Colors::White : 0xFF6E8578);

        gfx::fill_rect(ox, by, bw, bh, sel_installed ? 0xFF243A5C : 0xFF1A222C);
        gfx::draw_rect(ox, by, bw, bh, sel_installed ? gfx::Colors::Teal : 0xFF33404C);
        gfx::draw_string("Open App", ox + 24, by + 8,
                         sel_installed ? gfx::Colors::White : 0xFF66727E);
    };

    auto handle_hub_click = [&](int mx, int my) -> bool {
        if (!win7.visible || win7.minimized) return false;
        int lx, ly, lw, rh, rows, by, bw, bh, rx, ix, ox, bstart, bcell, bsize;
        hub_geom(win7, lx, ly, lw, rh, rows, by, bw, bh, rx, ix, ox, bstart, bcell, bsize);

        int n = apphub::count();

        if (my >= bstart && my < ly && mx >= lx && mx < lx + bcell * HUB_BUILTINS) {
            int idx = (mx - lx) / bcell;
            if (idx >= 0 && idx < HUB_BUILTINS) {
                open_builtin_app(hub_builtins[idx].win_id);
                kstd::strcpy(hub_status, "Opened ");
                kstd::strcpy(hub_status + kstd::strlen(hub_status), hub_builtins[idx].name);
                hub_status_kind = 1;
                full_redraw_needed = true;
                return true;
            }
        }

        if (mx >= rx && mx < rx + bw && my >= by && my < by + bh) {
            char msg[128];
            msg[0] = '\0';
            bool ok = apphub::refresh_catalog(msg, sizeof(msg));
            kstd::strcpy(hub_status, ok ? "Catalog: " : "Refresh failed - ");
            kstd::strcpy(hub_status + kstd::strlen(hub_status),
                         msg[0] ? msg : (ok ? "done" : "network error"));
            hub_status_kind = ok ? 1 : 2;
            if (ok) {
                n = apphub::count();
                if (hub_selected >= n) hub_selected = n > 0 ? n - 1 : 0;
            }
            full_redraw_needed = true;
            return true;
        }

        if (mx >= ix && mx < ix + bw && my >= by && my < by + bh) {
            if (n > 0 && hub_selected >= 0 && hub_selected < n) {
                char msg[128];
                msg[0] = '\0';
                bool ok = apphub::install(hub_selected, msg, sizeof(msg));
                kstd::strcpy(hub_status, ok ? "" : "Install failed - ");
                kstd::strcpy(hub_status + kstd::strlen(hub_status),
                             msg[0] ? msg : (ok ? "installed" : "unknown error"));
                hub_status_kind = ok ? 1 : 2;
            } else {
                kstd::strcpy(hub_status, "Select an app first.");
                hub_status_kind = 2;
            }
            full_redraw_needed = true;
            return true;
        }

        if (mx >= ox && mx < ox + bw && my >= by && my < by + bh) {
            const apphub::App* a = (hub_selected >= 0 && hub_selected < n)
                                       ? apphub::get(hub_selected) : nullptr;
            if (a && a->installed) {
                char url[48];
                kstd::strcpy(url, "app://");
                kstd::strncpy(url + 6, a->name, sizeof(url) - 7);
                url[sizeof(url) - 1] = '\0';
                launch_win4();
                browser_navigate(url, true);
                kstd::strcpy(hub_status, "Opened ");
                kstd::strcpy(hub_status + kstd::strlen(hub_status), a->name);
                hub_status_kind = 1;
            } else {
                kstd::strcpy(hub_status, "Install the app before opening it.");
                hub_status_kind = 2;
            }
            full_redraw_needed = true;
            return true;
        }

        for (int i = 0; i < rows && i < n; ++i) {
            int ry = ly + i * rh;
            if (mx >= lx && mx < lx + lw && my >= ry && my < ry + rh - 6) {
                hub_selected = i;
                full_redraw_needed = true;
                return true;
            }
        }
        return false;
    };

    // Content Renderer for Window 1 (System Monitor)
    auto draw_win1_content = [&](const WindowState& win, bool full_frame) {
        char num_buf[16];
        int bar_w = win.w - 40;
        if (bar_w < 60) bar_w = 60;

        if (full_frame) {
            gfx::fill_rect(win.x + 1, win.y + 28, win.w - 2, win.h - 29, gfx::Colors::Slate);

            gfx::draw_string("Kernel:     AetherOS x86_64 Long Mode", win.x + 20, win.y + 36, gfx::Colors::Teal);

            char res_str[96];
            kstd::strcpy(res_str, "Video:      ");
            if (gfx::has_hardware_2d()) {
                kstd::strcpy(res_str + kstd::strlen(res_str), "VMware SVGA-II ");
            } else if (gfx::has_hardware_double_buffering()) {
                kstd::strcpy(res_str + kstd::strlen(res_str), "Bochs VBE ");
            } else {
                kstd::strcpy(res_str + kstd::strlen(res_str), "Bochs/QEMU ");
            }
            kstd::utoa(width, num_buf, 10);
            kstd::strcpy(res_str + kstd::strlen(res_str), num_buf);
            kstd::strcpy(res_str + kstd::strlen(res_str), "x");
            kstd::utoa(height, num_buf, 10);
            kstd::strcpy(res_str + kstd::strlen(res_str), num_buf);
            kstd::strcpy(res_str + kstd::strlen(res_str), " 32bpp");
            gfx::draw_string(res_str, win.x + 20, win.y + 56, gfx::Colors::White);

            if (win.h >= 270) {
                gfx::draw_string("Language:   Freestanding C++20", win.x + 20, win.y + 226, gfx::Colors::White);
            }
            if (win.h >= 310) {
                gfx::draw_string("Controls:   [R] Hz  [B] Bar  [T] Theme  [Q] Exit", win.x + 20, win.y + 250, gfx::Colors::Cyan);
            }
            if (win.h >= 345) {
                gfx::draw_string("Buttons:    [X] Close   [-] Minimize   [+] Maximize", win.x + 20, win.y + 274, gfx::Colors::Yellow);
            }
        }

        // Dynamic Elements area
        if (!full_frame) {
            int dyn_h = (win.h > 80) ? (win.h - 80) : 10;
            gfx::fill_rect(win.x + 18, win.y + 76, bar_w + 4, dyn_h, gfx::Colors::Slate);
        }

        char fps_str[64];
        kstd::strcpy(fps_str, "Display:    ");
        kstd::strcpy(fps_str + kstd::strlen(fps_str), fps_labels[current_fps_idx]);
        kstd::strcpy(fps_str + kstd::strlen(fps_str), " (");
        kstd::utoa(measured_fps, num_buf, 10);
        kstd::strcpy(fps_str + kstd::strlen(fps_str), num_buf);
        kstd::strcpy(fps_str + kstd::strlen(fps_str), " FPS)");
        gfx::draw_string(fps_str, win.x + 20, win.y + 78, gfx::Colors::Green);

        size_t total_mb = pmm::get_total_memory() / (1024 * 1024);
        size_t used_mb = pmm::get_used_memory() / (1024 * 1024);
        uint32_t percent = total_mb ? static_cast<uint32_t>((used_mb * 100) / total_mb) : 0;

        char mem_str[64];
        kstd::strcpy(mem_str, "Memory:     ");
        kstd::utoa(used_mb, num_buf, 10);
        kstd::strcpy(mem_str + kstd::strlen(mem_str), num_buf);
        kstd::strcpy(mem_str + kstd::strlen(mem_str), " MB / ");
        kstd::utoa(total_mb, num_buf, 10);
        kstd::strcpy(mem_str + kstd::strlen(mem_str), num_buf);
        kstd::strcpy(mem_str + kstd::strlen(mem_str), " MB (");
        kstd::utoa(percent, num_buf, 10);
        kstd::strcpy(mem_str + kstd::strlen(mem_str), num_buf);
        kstd::strcpy(mem_str + kstd::strlen(mem_str), "%)");
        gfx::draw_string(mem_str, win.x + 20, win.y + 98, gfx::Colors::White);

        // Memory bar scales dynamically with win.w
        gfx::draw_rect(win.x + 20, win.y + 118, bar_w, 16, gfx::Colors::LightNavy);
        gfx::fill_rect(win.x + 22, win.y + 120, ((bar_w - 4) * percent) / 100, 12, gfx::Colors::Teal);

        char heap_str[64];
        kstd::strcpy(heap_str, "Heap Free:  ");
        kstd::utoa(heap::get_free_bytes() / (1024 * 1024), num_buf, 10);
        kstd::strcpy(heap_str + kstd::strlen(heap_str), num_buf);
        kstd::strcpy(heap_str + kstd::strlen(heap_str), " MB / ");
        kstd::utoa(heap::get_total_bytes() / (1024 * 1024), num_buf, 10);
        kstd::strcpy(heap_str + kstd::strlen(heap_str), num_buf);
        kstd::strcpy(heap_str + kstd::strlen(heap_str), " MB");
        gfx::draw_string(heap_str, win.x + 20, win.y + 140, gfx::Colors::White);

        char uptime_str[64];
        uint64_t sec = pit::uptime_seconds();
        kstd::strcpy(uptime_str, "Uptime:     ");
        kstd::utoa(sec / 60, num_buf, 10);
        kstd::strcpy(uptime_str + kstd::strlen(uptime_str), num_buf);
        kstd::strcpy(uptime_str + kstd::strlen(uptime_str), "m ");
        kstd::utoa(sec % 60, num_buf, 10);
        kstd::strcpy(uptime_str + kstd::strlen(uptime_str), num_buf);
        kstd::strcpy(uptime_str + kstd::strlen(uptime_str), "s");
        gfx::draw_string(uptime_str, win.x + 20, win.y + 160, gfx::Colors::White);

        if (win.h >= 210) {
            char net_str[80];
            kstd::strcpy(net_str, "Network:    ");
            if (wifi::is_ethernet_preferred()) {
                kstd::strcpy(net_str + kstd::strlen(net_str), "eth0 [Auto-ETH Active]");
            } else if (wifi::is_connected()) {
                kstd::strcpy(net_str + kstd::strlen(net_str), "wlan0 (");
                kstd::strcpy(net_str + kstd::strlen(net_str), wifi::get_connected_ssid());
                kstd::strcpy(net_str + kstd::strlen(net_str), ")");
            } else {
                kstd::strcpy(net_str + kstd::strlen(net_str), "Offline (No Link)");
            }
            gfx::draw_string(net_str, win.x + 20, win.y + 180, gfx::Colors::Cyan);
        }

        if (win.h >= 240) {
            char mouse_str[64];
            kstd::strcpy(mouse_str, "Mouse:      X=");
            kstd::utoa(mouse::get_x(), num_buf, 10);
            kstd::strcpy(mouse_str + kstd::strlen(mouse_str), num_buf);
            kstd::strcpy(mouse_str + kstd::strlen(mouse_str), " Y=");
            kstd::utoa(mouse::get_y(), num_buf, 10);
            kstd::strcpy(mouse_str + kstd::strlen(mouse_str), num_buf);
            if (mouse::is_left_down()) kstd::strcpy(mouse_str + kstd::strlen(mouse_str), " [CLICK]");
            gfx::draw_string(mouse_str, win.x + 20, win.y + 202, gfx::Colors::Yellow);
        }
    };

    // ========================================================================
    // Interactive Terminal Logic & Command Execution for Window 2
    // ========================================================================
    auto execute_terminal_command = [&]() {
        // 1. Record line in terminal output with prompt
        char hist[160];
        kstd::strcpy(hist, "aether@x86_64:~$ ");
        kstd::strncpy(hist + 17, term.input_buf, sizeof(hist) - 18);
        hist[sizeof(hist) - 1] = '\0';
        term.add_line(hist, gfx::Colors::Teal);

        // 2. Parse arguments
        char line_copy[128];
        kstd::strncpy(line_copy, term.input_buf, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';

        char* p = line_copy;
        while (*p && kstd::isspace(*p)) p++;

        char* argv[16];
        int argc = 0;
        while (*p && argc < 16) {
            while (kstd::isspace(*p)) *p++ = '\0';
            if (*p == '\0') break;
            argv[argc++] = p;
            while (*p && !kstd::isspace(*p)) p++;
        }

        // Save raw copy for commands that take raw text like calc or echo
        char raw_buf[128];
        kstd::strncpy(raw_buf, term.input_buf, sizeof(raw_buf) - 1);
        raw_buf[sizeof(raw_buf) - 1] = '\0';

        // 3. Clear active input line
        term.input_buf[0] = '\0';
        term.input_len = 0;
        term.cursor_pos = 0;

        if (argc == 0) return;

        sound::play_gui_cue_command();

        const char* cmd = argv[0];

        if (!kstd::strcmp(cmd, "help")) {
            term.add_line("=== Available Commands ===", gfx::Colors::Yellow);
            term.add_line("  help              Show this help menu", gfx::Colors::White);
            term.add_line("  3d / render       Launch 3D Hardware Rendering Engine", gfx::Colors::White);
            term.add_line("  sound             Audio device status, volume, and cues", gfx::Colors::White);
            term.add_line("  volume <0-100>    Set master output volume", gfx::Colors::White);
            term.add_line("  mute / unmute     Mute or unmute audio output", gfx::Colors::White);
            term.add_line("  play <effect>     Play sound effect (beep, chime, laser, coin...)", gfx::Colors::White);
            term.add_line("  wavplay <file>    Play WAV sound file from ramfs", gfx::Colors::White);
            term.add_line("  fetch / fastfetch Display system banner & overview", gfx::Colors::White);
            term.add_line("  ifconfig / ip     Show network interface IP, MAC, & stats", gfx::Colors::White);
            term.add_line("  wifi [scan|..]    Wi-Fi 802.11 scan, connect & auto-ETH", gfx::Colors::White);
            term.add_line("  wlan              Alias for wifi", gfx::Colors::White);
            term.add_line("  route             Display kernel IP routing table", gfx::Colors::White);
            term.add_line("  ping <ip> [count] Send ICMP echo requests and test latency", gfx::Colors::White);
            term.add_line("  arp               Show ARP address resolution cache table", gfx::Colors::White);
            term.add_line("  netstat           Show protocol counters & socket stats", gfx::Colors::White);
            term.add_line("  curl <ip> [port]  HTTP client GET request and response", gfx::Colors::White);
            term.add_line("  http <ip> [port]  Alias for curl", gfx::Colors::White);
            term.add_line("  ls                List files in RAM filesystem", gfx::Colors::White);
            term.add_line("  cat <file>        Display content of file", gfx::Colors::White);
            term.add_line("  mem               Physical RAM & kernel heap statistics", gfx::Colors::White);
            term.add_line("  uptime            Show time elapsed since boot", gfx::Colors::White);
            term.add_line("  date / rtc        Current Real-Time Clock timestamp", gfx::Colors::White);
            term.add_line("  bench             Run 64-bit ALU benchmark", gfx::Colors::White);
            term.add_line("  pat [bench|wc..]  Inspect & control PAT and Write-Combining", gfx::Colors::White);
            term.add_line("  calc <expr>       Integer calculator (+, -, *, /, %)", gfx::Colors::White);
            term.add_line("  echo <text...>    Print text to terminal", gfx::Colors::White);
            term.add_line("  clear             Clear terminal screen", gfx::Colors::White);
            term.add_line("  exit              Close terminal window", gfx::Colors::White);
        } else if (!kstd::strcmp(cmd, "fetch") || !kstd::strcmp(cmd, "fastfetch")) {
            term.add_line("   ___    aether@x86_64", gfx::Colors::Teal);
            term.add_line("  / _ \\   ------------------------------------", gfx::Colors::DarkGrey);
            term.add_line(" / /_\\ \\  OS:     AetherOS 64-bit (Beta)", gfx::Colors::White);
            term.add_line(" |  _  /  Kernel: x86_64 Long Mode C++20", gfx::Colors::White);

            char up_str[64];
            char nbuf[16];
            uint64_t s = pit::uptime_seconds();
            kstd::strcpy(up_str, " | | | |  Uptime: ");
            kstd::utoa(s / 60, nbuf, 10);
            kstd::strcpy(up_str + kstd::strlen(up_str), nbuf);
            kstd::strcpy(up_str + kstd::strlen(up_str), "m ");
            kstd::utoa(s % 60, nbuf, 10);
            kstd::strcpy(up_str + kstd::strlen(up_str), nbuf);
            kstd::strcpy(up_str + kstd::strlen(up_str), "s");
            term.add_line(up_str, gfx::Colors::White);

            char mem_s[64];
            size_t used_mb = pmm::get_used_memory() / (1024 * 1024);
            size_t total_mb = pmm::get_total_memory() / (1024 * 1024);
            kstd::strcpy(mem_s, " \\_| |_/  Memory: ");
            kstd::utoa(used_mb, nbuf, 10);
            kstd::strcpy(mem_s + kstd::strlen(mem_s), nbuf);
            kstd::strcpy(mem_s + kstd::strlen(mem_s), " MB / ");
            kstd::utoa(total_mb, nbuf, 10);
            kstd::strcpy(mem_s + kstd::strlen(mem_s), nbuf);
            kstd::strcpy(mem_s + kstd::strlen(mem_s), " MB (");
            kstd::utoa(total_mb ? (used_mb * 100 / total_mb) : 0, nbuf, 10);
            kstd::strcpy(mem_s + kstd::strlen(mem_s), nbuf);
            kstd::strcpy(mem_s + kstd::strlen(mem_s), "%)");
            term.add_line(mem_s, gfx::Colors::White);

            char heap_s[64];
            kstd::strcpy(heap_s, "          Heap:   ");
            kstd::utoa(heap::get_used_bytes() / 1024, nbuf, 10);
            kstd::strcpy(heap_s + kstd::strlen(heap_s), nbuf);
            kstd::strcpy(heap_s + kstd::strlen(heap_s), " KB Used / ");
            kstd::utoa(heap::get_free_bytes() / 1024, nbuf, 10);
            kstd::strcpy(heap_s + kstd::strlen(heap_s), nbuf);
            kstd::strcpy(heap_s + kstd::strlen(heap_s), " KB Free");
            term.add_line(heap_s, gfx::Colors::White);

            term.add_line("          Storage: RamFS (In-Memory VFS)", gfx::Colors::White);
        } else if (!kstd::strcmp(cmd, "ls") || !kstd::strcmp(cmd, "dir")) {
            term.add_line("=== RAM Filesystem (ramfs) ===", gfx::Colors::Cyan);
            size_t count = 0;
            size_t total_bytes = 0;
            const ramfs::File* all_files = ramfs::get_files();
            for (size_t i = 0; i < ramfs::MAX_FILES; ++i) {
                if (all_files[i].used) {
                    char f_line[64];
                    char num[16];
                    kstd::strcpy(f_line, "  ");
                    kstd::strcpy(f_line + kstd::strlen(f_line), all_files[i].name);
                    size_t len = kstd::strlen(all_files[i].name);
                    for (size_t sp = len; sp < 18; ++sp) {
                        kstd::strcpy(f_line + kstd::strlen(f_line), " ");
                    }
                    kstd::utoa(all_files[i].size, num, 10);
                    kstd::strcpy(f_line + kstd::strlen(f_line), num);
                    kstd::strcpy(f_line + kstd::strlen(f_line), " bytes");
                    term.add_line(f_line, gfx::Colors::White);
                    count++;
                    total_bytes += all_files[i].size;
                }
            }
            if (count == 0) {
                term.add_line("  (No files found)", gfx::Colors::LightGrey);
            } else {
                char tot_s[64];
                char num[16];
                kstd::strcpy(tot_s, "Total: ");
                kstd::utoa(count, num, 10);
                kstd::strcpy(tot_s + kstd::strlen(tot_s), num);
                kstd::strcpy(tot_s + kstd::strlen(tot_s), " file(s), ");
                kstd::utoa(total_bytes, num, 10);
                kstd::strcpy(tot_s + kstd::strlen(tot_s), num);
                kstd::strcpy(tot_s + kstd::strlen(tot_s), " bytes");
                term.add_line(tot_s, gfx::Colors::Teal);
            }
        } else if (!kstd::strcmp(cmd, "cat")) {
            if (argc < 2) {
                term.add_line("Usage: cat <filename>", gfx::Colors::Yellow);
            } else {
                const char* content = ramfs::read(argv[1]);
                if (!content) {
                    char err[64];
                    kstd::strcpy(err, "cat: '");
                    kstd::strncpy(err + kstd::strlen(err), argv[1], 30);
                    kstd::strcpy(err + kstd::strlen(err), "': No such file");
                    term.add_line(err, gfx::Colors::Red);
                } else {
                    term.write(content, gfx::Colors::White);
                }
            }
        } else if (!kstd::strcmp(cmd, "mem")) {
            term.add_line("--- Memory Statistics ---", gfx::Colors::Cyan);
            size_t total_ram = pmm::get_total_memory();
            size_t used_ram = pmm::get_used_memory();
            size_t free_ram = pmm::get_free_memory();
            char mline[80];
            char num[16];

            kstd::strcpy(mline, "  Physical RAM: ");
            kstd::utoa(used_ram / (1024 * 1024), num, 10);
            kstd::strcpy(mline + kstd::strlen(mline), num);
            kstd::strcpy(mline + kstd::strlen(mline), " MB / ");
            kstd::utoa(total_ram / (1024 * 1024), num, 10);
            kstd::strcpy(mline + kstd::strlen(mline), num);
            kstd::strcpy(mline + kstd::strlen(mline), " MB (Free: ");
            kstd::utoa(free_ram / (1024 * 1024), num, 10);
            kstd::strcpy(mline + kstd::strlen(mline), num);
            kstd::strcpy(mline + kstd::strlen(mline), " MB)");
            term.add_line(mline, gfx::Colors::White);

            char fline[80];
            kstd::strcpy(fline, "  Frames: ");
            kstd::utoa(pmm::get_used_frames(), num, 10);
            kstd::strcpy(fline + kstd::strlen(fline), num);
            kstd::strcpy(fline + kstd::strlen(fline), " used / ");
            kstd::utoa(pmm::get_total_frames(), num, 10);
            kstd::strcpy(fline + kstd::strlen(fline), num);
            kstd::strcpy(fline + kstd::strlen(fline), " total");
            term.add_line(fline, gfx::Colors::White);

            size_t heap_used = heap::get_used_bytes();
            size_t heap_free = heap::get_free_bytes();
            char hline[80];
            kstd::strcpy(hline, "  Kernel Heap:  ");
            kstd::utoa(heap_used / 1024, num, 10);
            kstd::strcpy(hline + kstd::strlen(hline), num);
            kstd::strcpy(hline + kstd::strlen(hline), " KB used, ");
            kstd::utoa(heap_free / 1024, num, 10);
            kstd::strcpy(hline + kstd::strlen(hline), num);
            kstd::strcpy(hline + kstd::strlen(hline), " KB free");
            term.add_line(hline, gfx::Colors::White);
        } else if (!kstd::strcmp(cmd, "uptime")) {
            uint64_t total_sec = pit::uptime_seconds();
            uint64_t hours = total_sec / 3600;
            uint64_t mins = (total_sec % 3600) / 60;
            uint64_t secs = total_sec % 60;
            uint64_t ms = pit::uptime_ms() % 1000;
            char uline[80];
            char num[16];
            kstd::strcpy(uline, "Uptime: ");
            kstd::utoa(hours, num, 10);
            kstd::strcpy(uline + kstd::strlen(uline), num);
            kstd::strcpy(uline + kstd::strlen(uline), "h ");
            kstd::utoa(mins, num, 10);
            kstd::strcpy(uline + kstd::strlen(uline), num);
            kstd::strcpy(uline + kstd::strlen(uline), "m ");
            kstd::utoa(secs, num, 10);
            kstd::strcpy(uline + kstd::strlen(uline), num);
            kstd::strcpy(uline + kstd::strlen(uline), ".");
            if (ms < 100) kstd::strcpy(uline + kstd::strlen(uline), "0");
            if (ms < 10)  kstd::strcpy(uline + kstd::strlen(uline), "0");
            kstd::utoa(ms, num, 10);
            kstd::strcpy(uline + kstd::strlen(uline), num);
            kstd::strcpy(uline + kstd::strlen(uline), "s (Ticks: ");
            kstd::utoa(pit::get_ticks(), num, 10);
            kstd::strcpy(uline + kstd::strlen(uline), num);
            kstd::strcpy(uline + kstd::strlen(uline), ")");
            term.add_line(uline, gfx::Colors::Green);
        } else if (!kstd::strcmp(cmd, "date") || !kstd::strcmp(cmd, "rtc") || !kstd::strcmp(cmd, "time")) {
            rtc::DateTime dt = rtc::get_datetime();
            char dline[64];
            char num[16];
            kstd::strcpy(dline, "RTC Time: ");
            kstd::utoa(dt.year, num, 10);
            kstd::strcpy(dline + kstd::strlen(dline), num);
            kstd::strcpy(dline + kstd::strlen(dline), "-");
            if (dt.month < 10) kstd::strcpy(dline + kstd::strlen(dline), "0");
            kstd::utoa(dt.month, num, 10);
            kstd::strcpy(dline + kstd::strlen(dline), num);
            kstd::strcpy(dline + kstd::strlen(dline), "-");
            if (dt.day < 10) kstd::strcpy(dline + kstd::strlen(dline), "0");
            kstd::utoa(dt.day, num, 10);
            kstd::strcpy(dline + kstd::strlen(dline), num);
            kstd::strcpy(dline + kstd::strlen(dline), " ");
            if (dt.hour < 10) kstd::strcpy(dline + kstd::strlen(dline), "0");
            kstd::utoa(dt.hour, num, 10);
            kstd::strcpy(dline + kstd::strlen(dline), num);
            kstd::strcpy(dline + kstd::strlen(dline), ":");
            if (dt.minute < 10) kstd::strcpy(dline + kstd::strlen(dline), "0");
            kstd::utoa(dt.minute, num, 10);
            kstd::strcpy(dline + kstd::strlen(dline), num);
            kstd::strcpy(dline + kstd::strlen(dline), ":");
            if (dt.second < 10) kstd::strcpy(dline + kstd::strlen(dline), "0");
            kstd::utoa(dt.second, num, 10);
            kstd::strcpy(dline + kstd::strlen(dline), num);
            kstd::strcpy(dline + kstd::strlen(dline), " UTC");
            term.add_line(dline, gfx::Colors::Cyan);
        } else if (!kstd::strcmp(cmd, "bench") || !kstd::strcmp(cmd, "benchmark")) {
            term.add_line("Running 64-bit ALU integer benchmark (10M ops)...", gfx::Colors::Yellow);
            uint64_t start_ms = pit::uptime_ms();
            volatile uint64_t acc = 0;
            for (uint64_t i = 1; i <= 10000000; ++i) {
                acc += (i ^ (i >> 3)) + (i * 3);
            }
            (void)acc;
            uint64_t duration_ms = pit::uptime_ms() - start_ms;
            if (duration_ms == 0) duration_ms = 1;
            uint64_t mops = (10000000ULL * 1000) / (duration_ms * 1000000);
            char res_str[80];
            char num[16];
            kstd::strcpy(res_str, "Benchmark Result: ");
            kstd::utoa(duration_ms, num, 10);
            kstd::strcpy(res_str + kstd::strlen(res_str), num);
            kstd::strcpy(res_str + kstd::strlen(res_str), " ms | ");
            kstd::utoa(mops, num, 10);
            kstd::strcpy(res_str + kstd::strlen(res_str), num);
            kstd::strcpy(res_str + kstd::strlen(res_str), " Mops/sec");
            term.add_line(res_str, gfx::Colors::Green);
        } else if (!kstd::strcmp(cmd, "pat")) {
            if (argc >= 2 && (!kstd::strcmp(argv[1], "bench") || !kstd::strcmp(argv[1], "benchmark"))) {
                term.add_line("Running Framebuffer Write Bandwidth Benchmark (64 MB)...", gfx::Colors::Yellow);
                pat::run_benchmark();
                gfx::swap_buffers();
                term.add_line("Benchmark completed. Screen redrawn.", gfx::Colors::Green);
            } else if (argc >= 2 && !kstd::strcmp(argv[1], "wc")) {
                uintptr_t fb = bga::get_framebuffer_addr();
                if (fb) {
                    pat::set_range_wc(fb, 32 * 1024 * 1024);
                    term.add_line("Framebuffer caching set to Write-Combining (WC, PAT4).", gfx::Colors::Green);
                }
            } else if (argc >= 2 && !kstd::strcmp(argv[1], "wb")) {
                uintptr_t fb = bga::get_framebuffer_addr();
                if (fb) {
                    pat::set_range_wb(fb, 32 * 1024 * 1024);
                    term.add_line("Framebuffer caching set to Write-Back (WB, PAT0).", gfx::Colors::Yellow);
                }
            } else if (argc >= 2 && !kstd::strcmp(argv[1], "uc")) {
                uintptr_t fb = bga::get_framebuffer_addr();
                if (fb) {
                    pat::set_range_uc(fb, 32 * 1024 * 1024);
                    term.add_line("Framebuffer caching set to Uncacheable (UC, PAT3).", gfx::Colors::Red);
                }
            } else {
                term.add_line("=== Page Attribute Table (PAT) Status ===", gfx::Colors::Teal);
                char line[96];
                char hex[24];
                uint64_t msr = pat::get_pat_msr();
                kstd::strcpy(line, "IA32_PAT MSR (0x277): 0x");
                kstd::utoa(msr >> 32, hex, 16);
                kstd::strcpy(line + kstd::strlen(line), hex);
                kstd::utoa(static_cast<uint32_t>(msr), hex, 16);
                kstd::strcpy(line + kstd::strlen(line), hex);
                term.add_line(line, gfx::Colors::White);

                uintptr_t fb = bga::get_framebuffer_addr();
                uint8_t type = pat::get_page_mem_type(fb);
                uint8_t idx = pat::get_page_pat_index(fb);
                kstd::strcpy(line, "Framebuffer: PAT");
                char nb[8];
                kstd::utoa(idx, nb, 10);
                kstd::strcpy(line + kstd::strlen(line), nb);
                kstd::strcpy(line + kstd::strlen(line), " -> ");
                kstd::strcpy(line + kstd::strlen(line), pat::get_mem_type_name(type));
                term.add_line(line, (type == pat::MEM_WC) ? gfx::Colors::Green : gfx::Colors::Yellow);
                term.add_line("Commands: pat bench | pat wc | pat wb | pat uc", gfx::Colors::DarkGrey);
            }
        } else if (!kstd::strcmp(cmd, "calc")) {
            int64_t a = 0, b = 0;
            char op = 0;
            if (argc >= 4) {
                a = kstd::parse_int(argv[1]);
                op = argv[2][0];
                b = kstd::parse_int(argv[3]);
            } else {
                const char* cp = raw_buf;
                while (*cp && kstd::isspace(*cp)) cp++;
                if (!kstd::strncmp(cp, "calc", 4)) {
                    cp += 4;
                    while (*cp && kstd::isspace(*cp)) cp++;
                }
                a = kstd::parse_int(cp);
                while (*cp && (kstd::isdigit(*cp) || *cp == '-')) cp++;
                while (*cp && kstd::isspace(*cp)) cp++;
                op = *cp;
                if (op) {
                    cp++;
                    while (*cp && kstd::isspace(*cp)) cp++;
                    b = kstd::parse_int(cp);
                }
            }

            if (!op) {
                term.add_line("Usage: calc <n1> <op> <n2>  (e.g. calc 100 * 4)", gfx::Colors::Yellow);
                term.add_line("Operators: +, -, *, /, %", gfx::Colors::LightGrey);
            } else {
                int64_t res = 0;
                bool ok = true;
                if (op == '+') res = a + b;
                else if (op == '-') res = a - b;
                else if (op == '*') res = a * b;
                else if (op == '/') {
                    if (b == 0) {
                        term.add_line("Error: Division by zero!", gfx::Colors::Red);
                        ok = false;
                    } else {
                        res = a / b;
                    }
                } else if (op == '%') {
                    if (b == 0) {
                        term.add_line("Error: Modulo by zero!", gfx::Colors::Red);
                        ok = false;
                    } else {
                        res = a % b;
                    }
                } else {
                    term.add_line("Error: Unsupported operator", gfx::Colors::Red);
                    ok = false;
                }

                if (ok) {
                    char cres[80];
                    char n1[24], n2[24], nr[24];
                    kstd::itoa(a, n1, 10);
                    kstd::itoa(b, n2, 10);
                    kstd::itoa(res, nr, 10);
                    kstd::strcpy(cres, n1);
                    kstd::strcpy(cres + kstd::strlen(cres), " ");
                    size_t cl = kstd::strlen(cres);
                    cres[cl] = op;
                    cres[cl + 1] = ' ';
                    cres[cl + 2] = '\0';
                    kstd::strcpy(cres + kstd::strlen(cres), n2);
                    kstd::strcpy(cres + kstd::strlen(cres), " = ");
                    kstd::strcpy(cres + kstd::strlen(cres), nr);
                    term.add_line(cres, gfx::Colors::Teal);
                }
            }
        } else if (!kstd::strcmp(cmd, "echo")) {
            if (argc <= 1) {
                term.add_line("", gfx::Colors::White);
            } else {
                const char* text = raw_buf;
                while (*text && kstd::isspace(*text)) text++;
                if (!kstd::strncmp(text, "echo", 4)) {
                    text += 4;
                    while (*text == ' ') text++;
                }
                term.add_line(text, gfx::Colors::White);
            }
        } else if (!kstd::strcmp(cmd, "ifconfig") || !kstd::strcmp(cmd, "ip")) {
            net::cmd_ifconfig(argc, argv, gui_print_callback);
        } else if (!kstd::strcmp(cmd, "wifi") || !kstd::strcmp(cmd, "wlan")) {
            wifi::cmd_wifi(argc, argv, gui_print_callback);
        } else if (!kstd::strcmp(cmd, "route")) {
            wifi::cmd_route(argc, argv, gui_print_callback);
        } else if (!kstd::strcmp(cmd, "ping")) {
            net::cmd_ping(argc, argv, gui_print_callback);
        } else if (!kstd::strcmp(cmd, "arp")) {
            net::cmd_arp(argc, argv, gui_print_callback);
        } else if (!kstd::strcmp(cmd, "netstat")) {
            net::cmd_netstat(argc, argv, gui_print_callback);
        } else if (!kstd::strcmp(cmd, "curl") || !kstd::strcmp(cmd, "http")) {
            net::cmd_curl(argc, argv, gui_print_callback);
        } else if (!kstd::strcmp(cmd, "sound")) {
            term.add_line("=== Audio Subsystem Status ===", gfx::Colors::Yellow);
            char dstr[96];
            kstd::strcpy(dstr, "Driver: ");
            kstd::strcpy(dstr + 8, sound::get_driver_name());
            term.add_line(dstr, gfx::Colors::White);

            char vstr[64];
            char nbuf[16];
            kstd::strcpy(vstr, "Volume: ");
            kstd::utoa(sound::get_volume(), nbuf, 10);
            kstd::strcpy(vstr + kstd::strlen(vstr), nbuf);
            kstd::strcpy(vstr + kstd::strlen(vstr), "% [");
            kstd::strcpy(vstr + kstd::strlen(vstr), sound::is_muted() ? "MUTED" : "UNMUTED");
            kstd::strcpy(vstr + kstd::strlen(vstr), "]");
            term.add_line(vstr, gfx::Colors::Cyan);

            char cstr[64];
            kstd::strcpy(cstr, "GUI Cues: ");
            kstd::strcpy(cstr + 10, sound::are_gui_cues_enabled() ? "Enabled" : "Disabled");
            term.add_line(cstr, gfx::Colors::Teal);
        } else if (!kstd::strcmp(cmd, "volume") || !kstd::strcmp(cmd, "vol")) {
            if (argc >= 2) {
                int vol = kstd::parse_int(argv[1]);
                if (vol < 0) vol = 0;
                if (vol > 100) vol = 100;
                sound::set_volume(static_cast<uint8_t>(vol));
                if (sound::is_muted()) sound::set_mute(false);
                char vmsg[48];
                kstd::strcpy(vmsg, "Master volume set to ");
                char nbuf[16];
                kstd::utoa(vol, nbuf, 10);
                kstd::strcpy(vmsg + kstd::strlen(vmsg), nbuf);
                kstd::strcpy(vmsg + kstd::strlen(vmsg), "%.");
                term.add_line(vmsg, gfx::Colors::Green);
            } else {
                char vmsg[48];
                kstd::strcpy(vmsg, "Current volume: ");
                char nbuf[16];
                kstd::utoa(sound::get_volume(), nbuf, 10);
                kstd::strcpy(vmsg + kstd::strlen(vmsg), nbuf);
                kstd::strcpy(vmsg + kstd::strlen(vmsg), "%");
                term.add_line(vmsg, gfx::Colors::White);
            }
        } else if (!kstd::strcmp(cmd, "mute")) {
            sound::set_mute(true);
            term.add_line("Audio output muted.", gfx::Colors::Yellow);
        } else if (!kstd::strcmp(cmd, "unmute")) {
            sound::set_mute(false);
            term.add_line("Audio output unmuted.", gfx::Colors::Green);
        } else if (!kstd::strcmp(cmd, "play")) {
            if (argc < 2) {
                term.add_line("Usage: play <beep|chime|laser|coin|fanfare|chord>", gfx::Colors::Yellow);
            } else {
                const char* eff = argv[1];
                if (!kstd::strcmp(eff, "beep")) {
                    term.add_line("Playing effect 'beep'...", gfx::Colors::White);
                    sound::play_effect(sound::Effect::Beep);
                } else if (!kstd::strcmp(eff, "chime") || !kstd::strcmp(eff, "bell")) {
                    term.add_line("Playing effect 'chime' (bell chord)...", gfx::Colors::White);
                    sound::play_effect(sound::Effect::Chime);
                } else if (!kstd::strcmp(eff, "laser")) {
                    term.add_line("Playing effect 'laser' (pitch sweep)...", gfx::Colors::White);
                    sound::play_effect(sound::Effect::Laser);
                } else if (!kstd::strcmp(eff, "coin")) {
                    term.add_line("Playing effect 'coin' (8-bit pickup)...", gfx::Colors::White);
                    sound::play_effect(sound::Effect::Coin);
                } else if (!kstd::strcmp(eff, "fanfare")) {
                    term.add_line("Playing effect 'fanfare' (victory jingle)...", gfx::Colors::White);
                    sound::play_effect(sound::Effect::Fanfare);
                } else if (!kstd::strcmp(eff, "chord")) {
                    term.add_line("Playing C Major chord...", gfx::Colors::White);
                    uint32_t c_maj[] = { 523, 659, 784, 1046 };
                    sound::play_chord(c_maj, 4, 500, true);
                } else {
                    term.add_line("Unknown effect. Available: beep, chime, laser, coin, fanfare, chord", gfx::Colors::Red);
                }
            }
        } else if (!kstd::strcmp(cmd, "wavplay")) {
            if (argc < 2) {
                term.add_line("Usage: wavplay <filename.wav>", gfx::Colors::Yellow);
            } else {
                size_t sz = 0;
                const void* data = ramfs::read_bytes(argv[1], &sz);
                if (!data) {
                    char err[64];
                    kstd::strcpy(err, "File not found: ");
                    kstd::strncpy(err + 16, argv[1], 32);
                    term.add_line(err, gfx::Colors::Red);
                } else {
                    char msg[64];
                    kstd::strcpy(msg, "Playing '");
                    kstd::strncpy(msg + 9, argv[1], 32);
                    kstd::strcpy(msg + kstd::strlen(msg), "'...");
                    term.add_line(msg, gfx::Colors::Cyan);
                    if (sound::play_wav(data, sz, true)) {
                        term.add_line("Playback complete.", gfx::Colors::Green);
                    } else {
                        term.add_line("Failed to decode WAV file.", gfx::Colors::Red);
                    }
                }
            }
        } else if (!kstd::strcmp(cmd, "clear")) {
            term.clear();
        } else if (!kstd::strcmp(cmd, "exit")) {
            close_window(&win2);
        } else if (!kstd::strcmp(cmd, "3d") || !kstd::strcmp(cmd, "render") || !kstd::strcmp(cmd, "pipeline") || !kstd::strcmp(cmd, "win3")) {
            launch_win3();
            term.add_line("3D Hardware Rendering Engine launched.", gfx::Colors::Green);
        } else if (!kstd::strcmp(cmd, "browser") || !kstd::strcmp(cmd, "web") || !kstd::strcmp(cmd, "win4")) {
            launch_win4();
            if (argc > 1) {
                browser_navigate(argv[1], true);
            }
            term.add_line("Aether Web Browser launched.", gfx::Colors::Green);
        } else if (!kstd::strcmp(cmd, "reboot")) {
            term.add_line("Rebooting system...", gfx::Colors::Yellow);
            arch::outb(0x64, 0xFE);
            asm volatile("lidt 0; int $3");
        } else if (!kstd::strcmp(cmd, "shutdown") || !kstd::strcmp(cmd, "poweroff") || !kstd::strcmp(cmd, "exit")) {
            term.add_line("Shutting down system...", gfx::Colors::Yellow);
            arch::outw(0x604, 0x2000);
            arch::outw(0xB004, 0x2000);
            arch::outw(0x4004, 0x3400);
            term.add_line("System halted. It is now safe to turn off your machine.", gfx::Colors::White);
            arch::cli();
            while (true) arch::hlt();
        } else {
            char unk[80];
            kstd::strcpy(unk, "Unknown command '");
            kstd::strncpy(unk + kstd::strlen(unk), cmd, 32);
            kstd::strcpy(unk + kstd::strlen(unk), "'. Type 'help' for commands.");
            term.add_line(unk, gfx::Colors::Red);
        }
    };

    auto handle_terminal_key = [&](char c) {
        // Handle arrow key sequences (ESC [ A/B/C/D)
        static char seq_buf[4] = {0};
        static int seq_len = 0;

        if (seq_len > 0) {
            // Collect escape sequence
            seq_buf[seq_len++] = c;
            if (seq_len >= 3) {
                // Full escape sequence received: ESC [ X
                if (seq_buf[0] == 27 && seq_buf[1] == '[' && seq_buf[2] >= 65 && seq_buf[2] <= 70) {
                    switch (seq_buf[2]) {
                        case 65: { // Up arrow: previous command in history
                            // Navigate through history
                            static bool hist_loaded = false;
                            if (!hist_loaded) {
                                kstd::strncpy(term.input_buf, term.input_buf, sizeof(term.input_buf) - 1);
                                term.input_len = kstd::strlen(term.input_buf);
                                hist_loaded = true;
                            }

                            // Find previous command
                            for (size_t i = term.line_count - 2; i >= 0; i--) {
                                const char* ltext = term.lines[i].text;
                                size_t len = kstd::strlen(ltext);
                                if (len > 17 && kstd::strncmp(ltext, "aether@x86_64:~$ ", 17) == 0) {
                                    char cmd_history[128];
                                    kstd::strncpy(cmd_history, ltext + 17, sizeof(cmd_history) - 1);
                                    if (kstd::strcmp(cmd_history, term.input_buf) != 0) {
                                        kstd::strcpy(term.input_buf, cmd_history);
                                        term.input_len = kstd::strlen(term.input_buf);
                                        term.cursor_pos = term.input_len;
                                        break;
                                    }
                                }
                            }
                            break;
                        }
                        case 66: { // Down arrow: next command in history
                                                    // Move forward in history
                            char cmd_history[128];
                            kstd::strncpy(cmd_history, term.input_buf, sizeof(cmd_history) - 1);
                            cmd_history[sizeof(cmd_history) - 1] = '\0';

                            // Find the next occurrence after the current one
                            for (size_t i = 0; i < term.line_count; ++i) {
                                const char* ltext = term.lines[i].text;
                                size_t len = kstd::strlen(ltext);
                                if (len > 17 && kstd::strncmp(ltext, "aether@x86_64:~$ ", 17) == 0) {
                                    kstd::strncpy(cmd_history, ltext + 17, sizeof(cmd_history) - 1);
                                    cmd_history[sizeof(cmd_history) - 1] = '\0';
                                    if (kstd::strcmp(cmd_history, term.input_buf) == 0) {
                                        // This is the current command, move to the next one
                                        if (i < term.line_count - 1) {
                                            // Found the next one
                                            kstd::strcpy(term.input_buf, cmd_history);
                                            term.input_len = kstd::strlen(term.input_buf);
                                            term.cursor_pos = term.input_len;
                                        }
                                        break;
                                    }
                                }
                            }
                            break;
                        }
                        case 67: { // Right arrow: move cursor right
                            if (term.cursor_pos < term.input_len) {
                                term.cursor_pos++;
                            }
                            break;
                        }
                        case 68: { // Left arrow: move cursor left
                            if (term.cursor_pos > 0) {
                                term.cursor_pos--;
                            }
                        }
                    }
                }
                seq_len = 0;
                seq_buf[0] = '\0';
            }
            return;
        }

        if (c == '\n' || c == '\r') {
            execute_terminal_command();
        } else if (c == '\b' || c == 8 || c == 127) {
            if (term.input_len > 0) {
                term.input_len--;
                term.input_buf[term.input_len] = '\0';
                term.cursor_pos = term.input_len;
            }
        } else if (c == 27) { // ESC - start of escape sequence
            seq_len = 0;
            seq_buf[0] = '\0';
        } else if (c == '\t') {
            // Tab key: cycle focus (handled by parent)
        } else if (c == 28) { // Ctrl+\
        } else if (c == 29) { // Ctrl+)
        } else if (c == 30) { // Ctrl+(
        } else if (c == 31) { // Ctrl+?
            // Ctrl+? = backspace (already handled above)
        } else if (c == 127) {
            // DEL (already handled above)
        } else if (c == 10) { // LF (line feed)
            execute_terminal_command();
        } else if (c == 12) { // Ctrl+L: clear screen
            term.clear();
            term.add_line("AetherOS Interactive Desktop Terminal [v1.0]", gfx::Colors::Teal);
            term.add_line("Type 'help' for commands, 'fetch' for system overview.", gfx::Colors::LightGrey);
            term.add_line("", gfx::Colors::White);
        } else if (c == 3) { // Ctrl+C: clear input
            term.input_buf[0] = '\0';
            term.input_len = 0;
            term.cursor_pos = 0;
        } else if (c >= 32 && c <= 126) {
            if (term.input_len < sizeof(term.input_buf) - 1) {
                term.input_buf[term.input_len++] = c;
                term.input_buf[term.input_len] = '\0';
                term.cursor_pos = term.input_len;
            }
        }
    };

    // Content Renderer for Window 2 (Interactive Terminal)
    auto draw_win2_content = [&](const WindowState& win) {
        int term_x = win.x + 10;
        int term_y = win.y + 35;
        int term_w = win.w - 20;
        int term_h = win.h - 48;

        if (term_w <= 20 || term_h <= 20) return;

        gfx::fill_rect(term_x, term_y, term_w, term_h, 0xFF08101E);

        int text_x = term_x + 6;
        int text_y = term_y + 6;
        int avail_w = term_w - 12;
        int avail_h = term_h - 12;

        if (avail_w < 16 || avail_h < 16) return;

        size_t max_cols = static_cast<size_t>(avail_w / 8);
        size_t max_rows = static_cast<size_t>(avail_h / 16);
        if (max_rows == 0) return;

        size_t prompt_row = (term.line_count < max_rows - 1) ? term.line_count : (max_rows - 1);
        size_t history_visible = prompt_row;
        size_t history_start = (term.line_count <= history_visible) ? 0 : (term.line_count - history_visible);

        // Draw history lines
        for (size_t r = 0; r < history_visible; ++r) {
            size_t idx = history_start + r;
            if (idx >= term.line_count) break;
            int ly = text_y + static_cast<int>(r) * 16;
            const char* ltext = term.lines[idx].text;
            uint32_t lcol = term.lines[idx].color;
            size_t len = kstd::strlen(ltext);
            if (len <= max_cols) {
                gfx::draw_string(ltext, text_x, ly, lcol);
            } else {
                char trunc[256];
                size_t c_len = max_cols < 255 ? max_cols : 255;
                kstd::strncpy(trunc, ltext, c_len);
                trunc[c_len] = '\0';
                gfx::draw_string(trunc, text_x, ly, lcol);
            }
        }

        // Draw active prompt line at prompt_row
        int py = text_y + static_cast<int>(prompt_row) * 16;
        const char* prompt_str = (max_cols >= 20) ? "aether@x86_64:~$ " : "$ ";
        size_t prompt_len = (max_cols >= 20) ? 17 : 2;

        if (prompt_len <= max_cols) {
            gfx::draw_string(prompt_str, text_x, py, gfx::Colors::Green);
        }

        int input_screen_x = text_x + static_cast<int>(prompt_len) * 8;
        if (max_cols > prompt_len) {
            size_t max_input_cols = max_cols - prompt_len;
            if (term.input_len <= max_input_cols) {
                gfx::draw_string(term.input_buf, input_screen_x, py, gfx::Colors::White);
            } else {
                char trunc_in[256];
                size_t in_len = max_input_cols < 255 ? max_input_cols : 255;
                kstd::strncpy(trunc_in, term.input_buf, in_len);
                trunc_in[in_len] = '\0';
                gfx::draw_string(trunc_in, input_screen_x, py, gfx::Colors::White);
            }
        }

        // Blinking cursor when Window 2 is active/focused
        bool is_focused = (win.active && &win == win_stack[TOP]);
        if (is_focused) {
            bool blink_on = ((pit::uptime_ms() / 500) % 2 == 0);
            if (blink_on) {
                int cursor_screen_x = input_screen_x + static_cast<int>(term.cursor_pos) * 8;
                if (cursor_screen_x + 8 <= term_x + term_w - 4) {
                    gfx::fill_rect(cursor_screen_x, py + 1, 8, 14, gfx::Colors::Teal);
                    if (term.cursor_pos < term.input_len) {
                        char ch = term.input_buf[term.cursor_pos];
                        gfx::draw_char(ch, cursor_screen_x, py, 0xFF08101E);
                    }
                }
            }
        }
    };

    auto draw_win3_hud = [&](const WindowState& win) {
        int hud_x = win.x + 20;
        int hud_y = win.y + win.h - 58;
        int hud_w = win.w - 40;
        int hud_h = 50;
        if (hud_w <= 0 || hud_h <= 0) return;

        // Clear only the HUD region inside Window 3
        gfx::fill_rect(hud_x, hud_y, hud_w, hud_h, 0xFF0A071A);

        char num_buf[16];
        char hud_fps[80];
        kstd::strcpy(hud_fps, "Pipeline: ");
        kstd::strcpy(hud_fps + kstd::strlen(hud_fps), fps_labels[current_fps_idx]);
        kstd::strcpy(hud_fps + kstd::strlen(hud_fps), "  |  ");
        kstd::utoa(measured_fps, num_buf, 10);
        kstd::strcpy(hud_fps + kstd::strlen(hud_fps), num_buf);
        kstd::strcpy(hud_fps + kstd::strlen(hud_fps), " FPS  |  ~");
        kstd::utoa(frame_time_ms, num_buf, 10);
        kstd::strcpy(hud_fps + kstd::strlen(hud_fps), num_buf);
        kstd::strcpy(hud_fps + kstd::strlen(hud_fps), " ms");
        gfx::draw_string(hud_fps, hud_x, hud_y + 4, gfx::Colors::Teal, 0xFF0A071A, false);

        if (hud_w >= 440) {
            gfx::draw_string("Buttons: [X] Close  [-] Minimize  [+] Maximize  |  [R] Cycle Hz", hud_x, hud_y + 26, gfx::Colors::Yellow, 0xFF0A071A, false);
        } else if (hud_w >= 280) {
            gfx::draw_string("[X] Close  [-] Min  [+] Max  [R] Hz", hud_x, hud_y + 26, gfx::Colors::Yellow, 0xFF0A071A, false);
        } else {
            gfx::draw_string("[R] Cycle Hz", hud_x, hud_y + 26, gfx::Colors::Yellow, 0xFF0A071A, false);
        }
    };

    // Content Renderer for Window 3 (3D Engine with Bounding-Box Dirty Tracking)
    auto draw_win3_content = [&](const WindowState& win, bool full_frame) {
        if (!win.visible || win.minimized) return;

        int cx = win.x + win.w / 2;
        int cy = win.y + 35 + (win.h - 95) / 2;
        int min_dim = (win.w < (win.h - 60)) ? win.w : (win.h - 60);
        int cube_scale = (min_dim > 700) ? 130 : (min_dim > 450 ? 90 : (min_dim > 300 ? 60 : 38));
        int dist = (min_dim > 450) ? 500 : 350;
        int z_offset = (min_dim > 450) ? 380 : 260;

        if (full_frame) {
            gfx::fill_rect(win.x + 10, win.y + 35, win.w - 20, win.h - 45, 0xFF0A071A);
            draw_win3_hud(win);
            win3_last_hud_fps = measured_fps;
            win3_last_hud_mode = current_fps_idx;
            win3_hud_dirty = false;
            win3_cube_prev.valid = false;
        }

        Point3D local_verts[8] = {
            { -cube_scale, -cube_scale, -cube_scale }, {  cube_scale, -cube_scale, -cube_scale },
            {  cube_scale,  cube_scale, -cube_scale }, { -cube_scale,  cube_scale, -cube_scale },
            { -cube_scale, -cube_scale,  cube_scale }, {  cube_scale, -cube_scale,  cube_scale },
            {  cube_scale,  cube_scale,  cube_scale }, { -cube_scale,  cube_scale,  cube_scale }
        };

        Point2D projected[8];
        for (int i = 0; i < 8; ++i) {
            int px = local_verts[i].x;
            int py = local_verts[i].y;
            int pz = local_verts[i].z;

            int y1 = (py * cos_fp(angle_x) - pz * sin_fp(angle_x)) / 256;
            int z1 = (py * sin_fp(angle_x) + pz * cos_fp(angle_x)) / 256;

            int x2 = (px * cos_fp(angle_y) + z1 * sin_fp(angle_y)) / 256;
            int z2 = (-px * sin_fp(angle_y) + z1 * cos_fp(angle_y)) / 256;

            int x3 = (x2 * cos_fp(angle_z) - y1 * sin_fp(angle_z)) / 256;
            int y3 = (x2 * sin_fp(angle_z) + y1 * cos_fp(angle_z)) / 256;

            int z_dist = z2 + z_offset;
            if (z_dist < 10) z_dist = 10;
            projected[i].x = cx + (x3 * dist) / z_dist;
            projected[i].y = cy + (y3 * dist) / z_dist;
        }

        // 1. Calculate current frame bounding box of projected vertices
        int min_x = projected[0].x;
        int max_x = projected[0].x;
        int min_y = projected[0].y;
        int max_y = projected[0].y;
        for (int i = 1; i < 8; ++i) {
            if (projected[i].x < min_x) min_x = projected[i].x;
            if (projected[i].x > max_x) max_x = projected[i].x;
            if (projected[i].y < min_y) min_y = projected[i].y;
            if (projected[i].y > max_y) max_y = projected[i].y;
        }

        // 2. Expand by margin (16px) for vertex dots (radius 6px) and lines
        const int margin = 16;
        min_x -= margin;
        max_x += margin;
        min_y -= margin;
        max_y += margin;

        // Clip to window canvas bounds (stay strictly above HUD area)
        int clip_x1 = win.x + 10;
        int clip_y1 = win.y + 35;
        int clip_x2 = win.x + win.w - 10;
        int clip_y2 = win.y + win.h - 60;

        if (min_x < clip_x1) min_x = clip_x1;
        if (max_x > clip_x2) max_x = clip_x2;
        if (min_y < clip_y1) min_y = clip_y1;
        if (max_y > clip_y2) max_y = clip_y2;

        win3_cube_curr.min_x = min_x;
        win3_cube_curr.min_y = min_y;
        win3_cube_curr.max_x = max_x;
        win3_cube_curr.max_y = max_y;
        win3_cube_curr.valid = true;

        // 3. Clear ONLY the union bounding box between previous and current frames
        if (!full_frame) {
            BoundingBox u_box = win3_cube_curr;
            if (win3_cube_prev.valid) {
                if (win3_cube_prev.min_x < u_box.min_x) u_box.min_x = win3_cube_prev.min_x;
                if (win3_cube_prev.min_y < u_box.min_y) u_box.min_y = win3_cube_prev.min_y;
                if (win3_cube_prev.max_x > u_box.max_x) u_box.max_x = win3_cube_prev.max_x;
                if (win3_cube_prev.max_y > u_box.max_y) u_box.max_y = win3_cube_prev.max_y;
            }
            int u_w = u_box.max_x - u_box.min_x + 1;
            int u_h = u_box.max_y - u_box.min_y + 1;
            if (u_w > 0 && u_h > 0) {
                gfx::fill_rect(u_box.min_x, u_box.min_y, u_w, u_h, 0xFF0A071A);
            }
        }

        // 4. Draw wireframe cube edges
        uint32_t edge_colors[12] = {
            gfx::Colors::Cyan, gfx::Colors::Teal, gfx::Colors::Green, gfx::Colors::Yellow,
            gfx::Colors::Magenta, gfx::Colors::Purple, gfx::Colors::Blue, gfx::Colors::Orange,
            gfx::Colors::White, gfx::Colors::Cyan, gfx::Colors::Magenta, gfx::Colors::Green
        };

        for (int e = 0; e < 12; ++e) {
            int v0 = cube_edges[e][0];
            int v1 = cube_edges[e][1];
            gfx::draw_line(projected[v0].x, projected[v0].y, projected[v1].x, projected[v1].y, edge_colors[e]);
        }

        // 5. Draw vertex dots
        for (int i = 0; i < 8; ++i) {
            gfx::fill_circle(projected[i].x, projected[i].y, (win.w > 700) ? 6 : 4, gfx::Colors::White);
        }

        // 6. Update HUD if dirty or values changed
        if (!full_frame) {
            if (win3_hud_dirty || measured_fps != win3_last_hud_fps || current_fps_idx != win3_last_hud_mode) {
                draw_win3_hud(win);
                win3_last_hud_fps = measured_fps;
                win3_last_hud_mode = current_fps_idx;
                win3_hud_dirty = true; // Signal swap_rect
            }
        }

        angle_x = (angle_x + 2) & 0xFF;
        angle_y = (angle_y + 3) & 0xFF;
        angle_z = (angle_z + 1) & 0xFF;
    };

    // Content Renderer for Window 4 (Aether Web Browser)
    auto draw_win4_content = [&](const WindowState& win, bool full_frame) {
        if (win.w < 100 || win.h < 100) return;

        int win_in_x = win.x + 2;
        int win_in_y = win.y + 28;
        int win_in_w = win.w - 4;

        int tb_y = win_in_y;
        int tb_h = 28;

        int bm_y = win_in_y + 28;
        int bm_h = 24;

        int sb_stat_y = win.y + win.h - 22;
        int sb_stat_h = 20;

        int vp_x = win_in_x;
        int vp_y = bm_y + bm_h;
        int vp_w = win_in_w - 16;
        int vp_h = sb_stat_y - vp_y;
        if (vp_h < 10) vp_h = 10;
        if (vp_w < 10) vp_w = 10;

        if (full_frame || win4_dirty) {
            // 1. Toolbar
            gfx::draw_gradient_v(win_in_x, tb_y, win_in_w, tb_h, 0xFF18283E, 0xFF0E1826);
            gfx::draw_line(win_in_x, tb_y + tb_h - 1, win_in_x + win_in_w - 1, tb_y + tb_h - 1, 0xFF0A121E);

            // Back button [ < ]
            bool can_back = (browser_history_pos > 0);
            uint32_t nav_bg = can_back ? 0xFF19385C : 0xFF101B29;
            uint32_t nav_border = can_back ? 0xFF2C5C94 : 0xFF182A3E;
            uint32_t nav_fg = can_back ? gfx::Colors::White : 0xFF506880;
            gfx::fill_rect(win.x + 6, tb_y + 3, 24, 22, nav_bg);
            gfx::draw_rect(win.x + 6, tb_y + 3, 24, 22, nav_border);
            gfx::draw_string("<", win.x + 14, tb_y + 6, nav_fg);

            // Forward button [ > ]
            bool can_fwd = (browser_history_pos + 1 < browser_history_count);
            nav_bg = can_fwd ? 0xFF19385C : 0xFF101B29;
            nav_border = can_fwd ? 0xFF2C5C94 : 0xFF182A3E;
            nav_fg = can_fwd ? gfx::Colors::White : 0xFF506880;
            gfx::fill_rect(win.x + 34, tb_y + 3, 24, 22, nav_bg);
            gfx::draw_rect(win.x + 34, tb_y + 3, 24, 22, nav_border);
            gfx::draw_string(">", win.x + 42, tb_y + 6, nav_fg);

            // Reload button [ R ]
            gfx::fill_rect(win.x + 62, tb_y + 3, 24, 22, 0xFF19385C);
            gfx::draw_rect(win.x + 62, tb_y + 3, 24, 22, 0xFF2C5C94);
            gfx::draw_string("R", win.x + 70, tb_y + 6, gfx::Colors::Cyan);

            // Home button [ H ]
            gfx::fill_rect(win.x + 90, tb_y + 3, 24, 22, 0xFF19385C);
            gfx::draw_rect(win.x + 90, tb_y + 3, 24, 22, 0xFF2C5C94);
            gfx::draw_string("H", win.x + 98, tb_y + 6, gfx::Colors::Yellow);

            // Address Bar
            int ab_x = win.x + 120;
            int ab_w = win.w - 176;
            if (ab_w < 50) ab_w = 50;
            gfx::fill_rect(ab_x, tb_y + 3, ab_w, 22, 0xFF08101C);
            gfx::draw_rect(ab_x, tb_y + 3, ab_w, 22, browser_url_active ? gfx::Colors::Teal : 0xFF1D3B5C);

            char display_url[96];
            kstd::strncpy(display_url, browser_url_input, sizeof(display_url) - 1);
            display_url[sizeof(display_url) - 1] = '\0';
            int max_chars = (ab_w - 14) / 8;
            if (max_chars > 0 && static_cast<int>(kstd::strlen(display_url)) > max_chars) {
                display_url[max_chars] = '\0';
            }
            gfx::draw_string(display_url, ab_x + 6, tb_y + 6, gfx::Colors::White);

            if (browser_url_active) {
                int cur_x = ab_x + 6 + static_cast<int>(kstd::strlen(display_url)) * 8;
                if (cur_x < ab_x + ab_w - 6) {
                    gfx::draw_line(cur_x, tb_y + 5, cur_x, tb_y + 19, gfx::Colors::Cyan);
                }
            }

            // Go button [ Go ]
            int go_x = win.x + win.w - 50;
            gfx::fill_rect(go_x, tb_y + 3, 44, 22, 0xFF10482B);
            gfx::draw_rect(go_x, tb_y + 3, 44, 22, gfx::Colors::Green);
            gfx::draw_string("Go", go_x + 14, tb_y + 6, gfx::Colors::White);

            // 2. Bookmarks Bar
            gfx::fill_rect(win_in_x, bm_y, win_in_w, bm_h, 0xFF0C1420);
            gfx::draw_line(win_in_x, bm_y + bm_h - 1, win_in_x + win_in_w - 1, bm_y + bm_h - 1, 0xFF18283E);

            auto draw_bm_chip = [&](int bx, int bw, const char* label) {
                if (bx + bw > win.x + win.w - 10) return;
                gfx::fill_rect(bx, bm_y + 2, bw, 20, 0xFF142438);
                gfx::draw_rect(bx, bm_y + 2, bw, 20, 0xFF223E5E);
                gfx::draw_string(label, bx + 6, bm_y + 4, gfx::Colors::Cyan);
            };

            draw_bm_chip(win.x + 8,   70, "Portal");
            draw_bm_chip(win.x + 82,  68, "RamFS");
            draw_bm_chip(win.x + 154, 58, "Docs");
            draw_bm_chip(win.x + 216, 58, "Acid");
            draw_bm_chip(win.x + 278, 58, "News");
            draw_bm_chip(win.x + 340, 68, "About");
            draw_bm_chip(win.x + 408, 58, "Apps");

            // 4. Status Bar
            gfx::fill_rect(win_in_x, sb_stat_y, win_in_w, sb_stat_h, 0xFF0A121E);
            gfx::draw_line(win_in_x, sb_stat_y, win_in_x + win_in_w - 1, sb_stat_y, 0xFF18283E);

            if (browser_hover_url[0] != '\0') {
                gfx::draw_string("Link: ", win.x + 8, sb_stat_y + 3, gfx::Colors::Yellow);
                char short_hover[48];
                kstd::strncpy(short_hover, browser_hover_url, 47);
                short_hover[47] = '\0';
                gfx::draw_string(short_hover, win.x + 52, sb_stat_y + 3, gfx::Colors::Cyan);
            } else {
                char stat_msg[80];
                char nbuf[16];
                kstd::strcpy(stat_msg, "Ready | Load: ");
                kstd::utoa(browser_doc->load_time_ms, nbuf, 10);
                kstd::strcpy(stat_msg + kstd::strlen(stat_msg), nbuf);
                kstd::strcpy(stat_msg + kstd::strlen(stat_msg), "ms | Boxes: ");
                kstd::utoa(static_cast<uint32_t>(browser_doc->box_count), nbuf, 10);
                kstd::strcpy(stat_msg + kstd::strlen(stat_msg), nbuf);
                gfx::draw_string(stat_msg, win.x + 8, sb_stat_y + 3, gfx::Colors::LightGrey);
            }

            if (win.w >= 540) {
                gfx::draw_string("[ 144Hz Accelerated ]", win.x + win.w - 180, sb_stat_y + 3, gfx::Colors::Green);
            }
        }

        // 3. Web Viewport & Scrollbar
        if (vp_w > 20 && vp_h > 20) {
            browser::render(*browser_doc, vp_x, vp_y, vp_w, vp_h, browser_doc->scroll_y);

            // Vertical Scrollbar
            int sb_x = win_in_x + win_in_w - 14;
            int sb_y = vp_y;
            int sb_w = 14;
            int sb_h = vp_h;

            gfx::fill_rect(sb_x, sb_y, sb_w, sb_h, 0xFF0A121E);
            gfx::draw_rect(sb_x, sb_y, sb_w, sb_h, 0xFF162538);

            int doc_h = (browser_doc->page_height > vp_h) ? browser_doc->page_height : vp_h;
            int max_scroll = doc_h - vp_h;
            int thumb_h = (vp_h * vp_h) / doc_h;
            if (thumb_h < 18) thumb_h = 18;
            if (thumb_h > sb_h) thumb_h = sb_h;

            int thumb_y = sb_y + (max_scroll > 0 ? (browser_doc->scroll_y * (sb_h - thumb_h)) / max_scroll : 0);
            gfx::fill_rect(sb_x + 2, thumb_y, sb_w - 4, thumb_h, 0xFF204870);
            gfx::draw_rect(sb_x + 2, thumb_y, sb_w - 4, thumb_h, gfx::Colors::Teal);
        }

        win4_dirty = false;
    };

    // ---------------------------------------------------------------------
    // Desktop launcher icons (left column). Icons map onto the existing
    // application windows; clicking a tile focuses / launches its window.
    // ---------------------------------------------------------------------
    struct DesktopIcon { const char* label; int kind; };
    const DesktopIcon desktop_icons[7] = {
        { "Terminal", 0 }, { "Browser", 1 }, { "3D Engine", 2 },
        { "Monitor", 3 }, { "Tasks", 4 }, { "Settings", 5 },
        { "App Hub", 6 }
    };
    constexpr int NICON = 7;
    int hovered_icon = -1;

    auto icon_center = [&](int idx, int& cx, int& cy, int& r) {
        int top = (current_bar_pos == BAR_TOP) ? 36 : 0;
        r = 28;
        cx = 22 + r;
        cy = top + 26 + r + idx * 88;
    };

    // Draw one complete window (frame + content + resize grip).
    auto draw_one_window = [&](WindowState& win, bool full) {
        draw_window_frame(win, current_button_style);
        if (win.id == 1) {
            draw_win1_content(win, full);
        } else if (win.id == 2) {
            draw_win2_content(win);
        } else if (win.id == 3) {
            draw_win3_content(win, full);
        } else if (win.id == 4) {
            draw_win4_content(win, full);
        } else if (win.id == 5) {
            draw_win5_content(win);
        } else if (win.id == 6) {
            draw_win6_content(win);
        } else if (win.id == 7) {
            draw_win7_content(win);
        }
        if (!win.maximized) {
            draw_resize_grip(win);
        }
    };

    auto draw_desktop_icons = [&]() {
        for (int i = 0; i < NICON; ++i) {
            int cx, cy, r;
            icon_center(i, cx, cy, r);
            draw_app_icon(cx, cy, r, desktop_icons[i].kind, hovered_icon == i);
            int lw = static_cast<int>(kstd::strlen(desktop_icons[i].label)) * 8;
            gfx::draw_string(desktop_icons[i].label, cx - lw / 2, cy + r + 6,
                             hovered_icon == i ? gfx::Colors::White : gfx::Colors::LightGrey,
                             0, true);
        }
    };

    // Render Full Desktop
    auto render_full_desktop = [&]() {
        cursor_has_saved_bg = false;

        // 1. Wallpaper: deep layered gradient with a soft top-left glow band.
        // The static wallpaper is cached in RAM: rebuilding the per-pixel
        // gradient for 1-2M pixels every full redraw was a major drag-time cost.
        if (wp_cache == nullptr) {
            wp_cache = static_cast<uint32_t*>(heap::malloc(static_cast<size_t>(width) * height * sizeof(uint32_t)));
        }
        if (wp_cache && wp_cache_valid) {
            uint32_t* buf = gfx::get_backbuffer();
            kstd::memcpy(buf, wp_cache, static_cast<size_t>(width) * height * sizeof(uint32_t));
        } else {
            gfx::draw_gradient_v(0, 0, width, height, 0xFF0E2140, 0xFF03060C);
            gfx::draw_gradient_v(0, 0, width, 220, 0xFF16365F, 0xFF0E2140);
            uint32_t grid_start_y = (current_bar_pos == BAR_TOP) ? 46 : 12;
            uint32_t grid_end_y = (current_bar_pos == BAR_TOP) ? (height - 12) : (height - 46);
            // 2. Fine 1px grid + brighter intersection nodes (subtle technical look)
            for (uint32_t gy = grid_start_y; gy < grid_end_y; gy += 48) {
                gfx::draw_line(0, static_cast<int>(gy), static_cast<int>(width) - 1,
                               static_cast<int>(gy), 0xFF0F2036);
            }
            for (uint32_t gx = 48; gx < width; gx += 48) {
                gfx::draw_line(static_cast<int>(gx), static_cast<int>(grid_start_y),
                               static_cast<int>(gx), static_cast<int>(grid_end_y), 0xFF0F2036);
            }
            if (wp_cache) {
                kstd::memcpy(wp_cache, gfx::get_backbuffer(),
                             static_cast<size_t>(width) * height * sizeof(uint32_t));
                wp_cache_valid = true;
            }
        }

        // 3. Desktop launcher icons (behind all windows)
        draw_desktop_icons();

        // 3. Render Windows from bottom to top of Z-stack
        for (int i = 0; i < NWIN; ++i) {
            WindowState* win = win_stack[i];
            if (!win->visible || win->minimized) continue;
            draw_one_window(*win, true);
        }

        // 4. Menu Bar / Taskbar (y = 0 when BAR_TOP, y = height - 36 when BAR_BOTTOM)
        uint32_t bar_y = (current_bar_pos == BAR_TOP) ? 0 : (height - 36);
        gfx::draw_gradient_v(0, bar_y, width, 36, gfx::Colors::Slate, gfx::Colors::Navy);
        if (current_bar_pos == BAR_TOP) {
            gfx::draw_line(0, 35, width - 1, 35, gfx::Colors::LightNavy);
        } else {
            gfx::draw_line(0, bar_y, width - 1, bar_y, gfx::Colors::LightNavy);
        }

        // Menu Bar Window Buttons
        auto draw_tb_button = [&](int bx, const char* label, const WindowState& w) {
            uint32_t bg = !w.visible ? 0xFF0D1C2E : (w.minimized ? 0xFF122842 : (w.active ? 0xFF1A4670 : 0xFF102236));
            uint32_t border = w.active ? gfx::Colors::Teal : (!w.visible ? 0xFF2A5078 : (w.minimized ? 0xFF2A5078 : gfx::Colors::LightNavy));
            uint32_t fg = !w.visible ? gfx::Colors::Cyan : (w.minimized ? gfx::Colors::Yellow : (w.active ? gfx::Colors::White : gfx::Colors::LightGrey));
            gfx::fill_rect(bx, bar_y + 4, 116, 28, bg);
            gfx::draw_rect(bx, bar_y + 4, 116, 28, border);
            int text_x = bx + (116 - static_cast<int>(kstd::strlen(label)) * 8) / 2;
            if (text_x < bx + 2) text_x = bx + 2;
            gfx::draw_string(label, text_x, bar_y + 10, fg);
        };

        draw_tb_button(114, !win1.visible ? "x SysMonitor" : (win1.minimized ? "_ SysMonitor" : (win1.active ? "* SysMonitor" : "  SysMonitor")), win1);
        draw_tb_button(238, !win2.visible ? "x Terminal"   : (win2.minimized ? "_ Terminal"   : (win2.active ? "* Terminal"   : "  Terminal")), win2);
        draw_tb_button(362, !win3.visible ? "[+ 3D Engine]" : (win3.minimized ? "[_ 3D Engine]" : (win3.active ? "[* 3D Engine]" : "[ 3D Engine ]")), win3);
        draw_tb_button(486, !win4.visible ? "[+ Browser]" : (win4.minimized ? "[_ Browser]" : (win4.active ? "[* Browser]" : "[ Browser ]")), win4);

        // Bar Position Toggle Button
        gfx::fill_rect(610, bar_y + 4, 116, 28, 0xFF102236);
        gfx::draw_rect(610, bar_y + 4, 116, 28, gfx::Colors::LightNavy);
        gfx::draw_string(current_bar_pos == BAR_TOP ? "[ Bar: Top ]" : "[ Bar: Btm ]", 618, bar_y + 10, gfx::Colors::LightGrey);

        // Button Style Toggle Button
        gfx::fill_rect(734, bar_y + 4, 110, 28, 0xFF102236);
        gfx::draw_rect(734, bar_y + 4, 110, 28, gfx::Colors::LightNavy);
        gfx::draw_string(current_button_style == BUTTONS_CIRCLES ? "[ Style: O ]" : "[ Style: # ]", 742, bar_y + 10, gfx::Colors::LightGrey);

        // Resolution Badge
        char res_badge[32];
        char num_buf[16];
        kstd::utoa(width, num_buf, 10);
        kstd::strcpy(res_badge, num_buf);
        kstd::strcpy(res_badge + kstd::strlen(res_badge), "x");
        kstd::utoa(height, num_buf, 10);
        kstd::strcpy(res_badge + kstd::strlen(res_badge), num_buf);
        if (width >= 1200) {
            gfx::fill_rect(width - 480, bar_y + 4, 115, 28, 0xFF14243B);
            gfx::draw_rect(width - 480, bar_y + 4, 115, 28, gfx::Colors::LightNavy);
            gfx::draw_string(res_badge, width - 470, bar_y + 10, gfx::Colors::LightGrey);
        }

        // Network Status Badge (Auto-Ethernet / Wi-Fi)
        if (width >= 1400) {
            uint32_t net_fg = gfx::Colors::LightGrey;
            char net_badge[32];
            if (wifi::is_ethernet_preferred()) {
                kstd::strcpy(net_badge, "[ ETH: 1Gbps ]");
                net_fg = gfx::Colors::Green;
            } else if (wifi::is_connected()) {
                kstd::strcpy(net_badge, "[ WiFi: ");
                const char* s = wifi::get_connected_ssid();
                char short_s[9];
                kstd::strncpy(short_s, s, 8);
                short_s[8] = '\0';
                kstd::strcpy(net_badge + kstd::strlen(net_badge), short_s);
                kstd::strcpy(net_badge + kstd::strlen(net_badge), " ]");
                net_fg = gfx::Colors::Cyan;
            } else {
                kstd::strcpy(net_badge, "[ Net: Off ]");
                net_fg = gfx::Colors::Grey;
            }
            gfx::fill_rect(width - 640, bar_y + 4, 150, 28, 0xFF14243B);
            gfx::draw_rect(width - 640, bar_y + 4, 150, 28, gfx::Colors::LightNavy);
            gfx::draw_string(net_badge, width - 630, bar_y + 10, net_fg);
        }

        // Refresh Rate Badge
        gfx::fill_rect(width - 355, bar_y + 4, 135, 28, 0xFF0D324D);
        gfx::draw_rect(width - 355, bar_y + 4, 135, 28, gfx::Colors::Teal);
        char hz_btn[32];
        kstd::strcpy(hz_btn, "[ ");
        kstd::strcpy(hz_btn + kstd::strlen(hz_btn), fps_labels[current_fps_idx]);
        kstd::strcpy(hz_btn + kstd::strlen(hz_btn), " (R) ]");
        gfx::draw_string(hz_btn, width - 347, bar_y + 10, gfx::Colors::Cyan);

        // Measured FPS Badge
        char live_fps[32];
        kstd::strcpy(live_fps, "FPS: ");
        kstd::utoa(measured_fps, num_buf, 10);
        kstd::strcpy(live_fps + kstd::strlen(live_fps), num_buf);
        gfx::fill_rect(width - 210, bar_y + 4, 85, 28, 0xFF14243B);
        gfx::draw_rect(width - 210, bar_y + 4, 85, 28, gfx::Colors::LightNavy);
        gfx::draw_string(live_fps, width - 204, bar_y + 10, gfx::Colors::Green, 0xFF14243B, false);

        // Real-Time Clock Badge
        int clock_x = width - 120;
        int clock_y = bar_y + 4;
        int clock_w = 114;
        int clock_h = 28;
        gfx::fill_rect(clock_x, clock_y, clock_w, clock_h, 0xFF14243B);
        gfx::draw_rect(clock_x, clock_y, clock_w, clock_h, gfx::Colors::LightNavy);
        rtc::DateTime dt = rtc::get_datetime();
        char time_str[32];
        time_str[0] = '\0';
        if (dt.hour < 10) kstd::strcpy(time_str, "0");
        kstd::utoa(dt.hour, num_buf, 10);
        kstd::strcpy(time_str + kstd::strlen(time_str), num_buf);
        kstd::strcpy(time_str + kstd::strlen(time_str), ":");
        if (dt.minute < 10) kstd::strcpy(time_str + kstd::strlen(time_str), "0");
        kstd::utoa(dt.minute, num_buf, 10);
        kstd::strcpy(time_str + kstd::strlen(time_str), num_buf);
        kstd::strcpy(time_str + kstd::strlen(time_str), ":");
        if (dt.second < 10) kstd::strcpy(time_str + kstd::strlen(time_str), "0");
        kstd::utoa(dt.second, num_buf, 10);
        kstd::strcpy(time_str + kstd::strlen(time_str), num_buf);
        kstd::strcpy(time_str + kstd::strlen(time_str), " UTC");
        gfx::draw_string(time_str, width - 112, bar_y + 10, gfx::Colors::Teal, 0xFF14243B, false);
        last_clock_second = dt.second;
        menu_last_fps = measured_fps;
    };

    // Fast path used while dragging a window: repaint only the rectangle that
    // actually changed (the union of the window's old and new footprint) rather
    // than re-rendering the whole desktop and blitting the whole framebuffer.
    // Returns false to request a full redraw fallback.
    auto render_drag_region = [&](WindowState& dw) -> bool {
        if (!dw.visible || dw.minimized || dw.maximized) return false;
        if (!wp_cache || !wp_cache_valid) return false;

        const int M = 10; // shadow margin
        int ox = dw.x - M, oy = dw.y - M, ow = dw.w + 2 * M, oh = dw.h + 2 * M;
        int x0 = ox, y0 = oy, x1 = ox + ow, y1 = oy + oh;
        if (drag_prev_valid) {
            if (drag_prev_x < x0) x0 = drag_prev_x;
            if (drag_prev_y < y0) y0 = drag_prev_y;
            if (drag_prev_x + drag_prev_w > x1) x1 = drag_prev_x + drag_prev_w;
            if (drag_prev_y + drag_prev_h > y1) y1 = drag_prev_y + drag_prev_h;
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > static_cast<int>(width)) x1 = width;
        if (y1 > static_cast<int>(height)) y1 = height;
        if (x1 <= x0 || y1 <= y0) return false;

        // Bail out to a full redraw when the region touches the menu bar (which
        // is drawn on top of windows) or grows too large to be worth it.
        int bar_y = (current_bar_pos == BAR_TOP) ? 0 : (static_cast<int>(height) - 36);
        if (y0 < bar_y + 36 && y1 > bar_y) return false;
        unsigned long long area = static_cast<unsigned long long>(x1 - x0) * (y1 - y0);
        if (area * 2ULL > static_cast<unsigned long long>(width) * height) return false;

        uint32_t* buf = gfx::get_backbuffer();
        int dmg_w = x1 - x0;

        // 1. Restore wallpaper for the damaged rectangle from the RAM cache.
        for (int y = y0; y < y1; ++y) {
            kstd::memcpy(buf + static_cast<size_t>(y) * width + x0,
                         wp_cache + static_cast<size_t>(y) * width + x0,
                         static_cast<size_t>(dmg_w) * sizeof(uint32_t));
        }

        // 2. Launcher icons that intersect the region.
        for (int i = 0; i < NICON; ++i) {
            int cx, cy, r;
            icon_center(i, cx, cy, r);
            if (cx + r >= x0 && cx - r <= x1 && cy + r >= y0 && cy - r <= y1) {
                draw_app_icon(cx, cy, r, desktop_icons[i].kind, hovered_icon == i);
                int lw = static_cast<int>(kstd::strlen(desktop_icons[i].label)) * 8;
                gfx::draw_string(desktop_icons[i].label, cx - lw / 2, cy + r + 6,
                                 hovered_icon == i ? gfx::Colors::White : gfx::Colors::LightGrey, 0, true);
            }
        }

        // 3. Other windows overlapping the region (bottom -> top). Drawn fully
        //    because the wallpaper restore above may have erased parts of them.
        for (int i = 0; i < NWIN; ++i) {
            WindowState* w = win_stack[i];
            if (w == &dw || !w->visible || w->minimized) continue;
            if (w->x < x1 && w->x + w->w > x0 && w->y < y1 && w->y + w->h > y0) {
                draw_one_window(*w, true);
            }
        }

        // 4. Dragged window: redraw its frame + content over the region.
        draw_one_window(dw, true);

        drag_dmg_x = x0; drag_dmg_y = y0; drag_dmg_w = x1 - x0; drag_dmg_h = y1 - y0;
        drag_dmg_valid = true;
        drag_prev_x = ox; drag_prev_y = oy; drag_prev_w = ow; drag_prev_h = oh;
        drag_prev_valid = true;
        return true;
    };

    render_full_desktop();

    while (keyboard::has_char()) keyboard::get_char();

    while (true) {
        uint64_t frame_start = pit::uptime_ms();
        uint32_t bar_y = (current_bar_pos == BAR_TOP) ? 0 : (height - 36);

        // Process keyboard input
        if (keyboard::has_char()) {
            char c = keyboard::get_char();
            if (win_stack[TOP] == &win2 && win2.visible && !win2.minimized) {
                win2_dirty = true;
                if (c == '\t' && term.input_len == 0) {
                    cycle_focus();
                    full_redraw_needed = true;
                } else if (c == '3' && term.input_len == 0 && (!win3.visible || win3.minimized)) {
                    launch_win3();
                } else if (c == '4' && term.input_len == 0 && (!win4.visible || win4.minimized)) {
                    launch_win4();
                } else if (c == '5' && term.input_len == 0 && (!win5.visible || win5.minimized)) {
                    launch_win5();
                } else if (c == '6' && term.input_len == 0 && (!win6.visible || win6.minimized)) {
                    launch_win6();
                } else {
                    handle_terminal_key(c);
                }
            } else if (win_stack[TOP] == &win4 && win4.visible && !win4.minimized) {
                if (browser_url_active) {
                    if (c == '\n' || c == '\r') {
                        browser_navigate(browser_url_input, true);
                    } else if (c == '\b' || c == 8 || c == 127) {
                        if (browser_url_len > 0) {
                            browser_url_input[--browser_url_len] = '\0';
                            win4_dirty = true;
                        }
                    } else if (c == 27) { // ESC cancels editing
                        browser_url_active = false;
                        kstd::strncpy(browser_url_input, browser_doc->current_url, sizeof(browser_url_input) - 1);
                        browser_url_len = static_cast<int>(kstd::strlen(browser_url_input));
                        win4_dirty = true;
                    } else if (c >= 32 && c <= 126) {
                        if (browser_url_len < static_cast<int>(sizeof(browser_url_input)) - 2) {
                            browser_url_input[browser_url_len++] = c;
                            browser_url_input[browser_url_len] = '\0';
                            win4_dirty = true;
                        }
                    }
                } else {
                    if (c == '\t') {
                        cycle_focus();
                        full_redraw_needed = true;
                    } else if (c == 'j' || c == 's' || c == ' ') {
                        int vh = (win4.y + win4.h - 22) - (win4.y + 80);
                        int max_scroll = browser_doc->page_height - vh;
                        if (max_scroll < 0) max_scroll = 0;
                        browser_doc->scroll_y += 40;
                        if (browser_doc->scroll_y > max_scroll) browser_doc->scroll_y = max_scroll;
                        win4_dirty = true;
                    } else if (c == 'k' || c == 'w') {
                        browser_doc->scroll_y -= 40;
                        if (browser_doc->scroll_y < 0) browser_doc->scroll_y = 0;
                        win4_dirty = true;
                    } else if (c == 'd') {
                        int vh = (win4.y + win4.h - 22) - (win4.y + 80);
                        int max_scroll = browser_doc->page_height - vh;
                        if (max_scroll < 0) max_scroll = 0;
                        browser_doc->scroll_y += vh / 2;
                        if (browser_doc->scroll_y > max_scroll) browser_doc->scroll_y = max_scroll;
                        win4_dirty = true;
                    } else if (c == 'u') {
                        int vh = (win4.y + win4.h - 22) - (win4.y + 80);
                        browser_doc->scroll_y -= vh / 2;
                        if (browser_doc->scroll_y < 0) browser_doc->scroll_y = 0;
                        win4_dirty = true;
                    } else if (c == 'l' || c == 'L') {
                        browser_url_active = true;
                        win4_dirty = true;
                    } else if (c == 'h' || c == 'H') {
                        browser_navigate("about:home", true);
                    } else if (c == 'r' || c == 'R') {
                        browser_navigate(browser_doc->current_url, false);
                    } else if (c == 27 || c == 'q' || c == 'Q') {
                        close_window(&win4);
                    } else if (c == '1') {
                        current_fps_idx = 0; full_redraw_needed = true;
                    } else if (c == '2') {
                        current_fps_idx = 1; full_redraw_needed = true;
                    } else if (c == '3') {
                        launch_win3();
                    } else if (c == 'b' || c == 'B') {
                        current_bar_pos = (current_bar_pos == BAR_TOP) ? BAR_BOTTOM : BAR_TOP;
                        reset_window_layout();
                        wp_cache_valid = false;
                        full_redraw_needed = true;
                    } else if (c == 't' || c == 'T') {
                        current_button_style = (current_button_style == BUTTONS_CIRCLES) ? BUTTONS_SQUARES : BUTTONS_CIRCLES;
                        full_redraw_needed = true;
                    }
                }
            } else {
                if (c == 27 || c == 'q' || c == 'Q') {
                    break;
                } else if (c == '\t') {
                    cycle_focus();
                    full_redraw_needed = true;
                } else if (c == 'r' || c == 'R') {
                    cycle_fps();
                    full_redraw_needed = true;
                } else if (c == 'b' || c == 'B') {
                    current_bar_pos = (current_bar_pos == BAR_TOP) ? BAR_BOTTOM : BAR_TOP;
                    reset_window_layout();
                    wp_cache_valid = false;
                    full_redraw_needed = true;
                } else if (c == 't' || c == 'T') {
                    current_button_style = (current_button_style == BUTTONS_CIRCLES) ? BUTTONS_SQUARES : BUTTONS_CIRCLES;
                    full_redraw_needed = true;
                } else if (c == '1') {
                    current_fps_idx = 0; // 60 Hz
                    full_redraw_needed = true;
                } else if (c == '2') {
                    current_fps_idx = 1; // 120 Hz
                    full_redraw_needed = true;
                } else if (c == '3') {
                    if (!win3.visible || win3.minimized) {
                        launch_win3();
                    } else {
                        bring_to_front(&win3);
                    }
                    full_redraw_needed = true;
                } else if (c == '4') {
                    if (!win4.visible || win4.minimized) {
                        launch_win4();
                    } else {
                        bring_to_front(&win4);
                    }
                    full_redraw_needed = true;
                } else if (c == '7') {
                    if (!win7.visible || win7.minimized) {
                        launch_win7();
                    } else {
                        bring_to_front(&win7);
                    }
                    full_redraw_needed = true;
                } else if (c == '0' || c == 'm' || c == 'M') {
                    current_fps_idx = 4; // Unlocked / MAX
                    full_redraw_needed = true;
                }
            }
        }

        int mx = mouse::get_x();
        int my = mouse::get_y();
        bool left_down = mouse::is_left_down();

        // Desktop icon hover highlight (only changes when a different tile is
        // under the pointer, so it costs at most one full redraw per crossing).
        {
            int new_hover = -1;
            for (int i = 0; i < NICON; ++i) {
                int cx, cy, r;
                icon_center(i, cx, cy, r);
                int dx = mx - cx, dy = my - cy;
                if (dx * dx + dy * dy <= r * r) { new_hover = i; break; }
            }
            if (new_hover != hovered_icon) {
                hovered_icon = new_hover;
                full_redraw_needed = true;
            }
        }

        // Window 1 shows live mouse coords / FPS / uptime: refresh it lazily
        {
            uint64_t now_ms = pit::uptime_ms();
            if (mx != prev_win1_mx || my != prev_win1_my ||
                measured_fps != last_win1_fps || now_ms - last_win1_ms >= 250) {
                win1_dirty = true;
                prev_win1_mx = mx;
                prev_win1_my = my;
                last_win1_fps = measured_fps;
                last_win1_ms = now_ms;
            }
        }

        // Terminal cursor blink only needs a redraw when the phase flips
        {
            int blink = static_cast<int>((pit::uptime_ms() / 500) % 2);
            if (blink != last_term_blink) {
                last_term_blink = blink;
                if (win_stack[TOP] == &win2 && win2.visible && !win2.minimized) {
                    win2_dirty = true;
                }
            }
        }

        // Handle Mouse Clicks
        if (left_down && !prev_left_click) {
            bool click_handled = false;

            // 1. Check Window Controls & Bodies from Top to Bottom of Z-stack
            for (int i = TOP; i >= 0; --i) {
                WindowState* win = win_stack[i];
                if (!win->visible || win->minimized) continue;

                // Check Titlebar Controls (y between win->y and win->y + 28)
                if (my >= win->y && my < win->y + 28) {
                    if (current_button_style == BUTTONS_CIRCLES) {
                        // Close [X] (Red circle on left)
                        if (mx >= win->x && mx < win->x + 24) {
                            close_window(win);
                            click_handled = true;
                            break;
                        }
                        // Minimize [-] (Yellow circle on left)
                        if (mx >= win->x + 24 && mx < win->x + 44) {
                            minimize_window(win);
                            click_handled = true;
                            break;
                        }
                        // Maximize [+] (Green circle on left)
                        if (mx >= win->x + 44 && mx < win->x + 68) {
                            maximize_toggle_window(win);
                            click_handled = true;
                            break;
                        }
                        // Titlebar drag / double-click
                        if (mx >= win->x + 68 && mx <= win->x + win->w) {
                            uint64_t now = pit::uptime_ms();
                            if (now - last_title_click < 400 && last_clicked_win == win->id) {
                                maximize_toggle_window(win);
                                last_title_click = 0;
                            } else {
                                last_title_click = now;
                                last_clicked_win = win->id;
                                bring_to_front(win);
                                if (!win->maximized) {
                                    begin_drag(win, mx, my);
                                }
                            }
                            click_handled = true;
                            break;
                        }
                    } else { // BUTTONS_SQUARES
                        // Close [✕] (Square on right)
                        if (mx >= win->x + win->w - 26 && mx <= win->x + win->w) {
                            close_window(win);
                            click_handled = true;
                            break;
                        }
                        // Maximize [□] (Square on right)
                        if (mx >= win->x + win->w - 47 && mx < win->x + win->w - 26) {
                            maximize_toggle_window(win);
                            click_handled = true;
                            break;
                        }
                        // Minimize [_] (Square on right)
                        if (mx >= win->x + win->w - 68 && mx < win->x + win->w - 47) {
                            minimize_window(win);
                            click_handled = true;
                            break;
                        }
                        // Titlebar drag / double-click
                        if (mx >= win->x && mx < win->x + win->w - 68) {
                            uint64_t now = pit::uptime_ms();
                            if (now - last_title_click < 400 && last_clicked_win == win->id) {
                                maximize_toggle_window(win);
                                last_title_click = 0;
                            } else {
                                last_title_click = now;
                                last_clicked_win = win->id;
                                bring_to_front(win);
                                if (!win->maximized) {
                                    begin_drag(win, mx, my);
                                }
                            }
                            click_handled = true;
                            break;
                        }
                    }
                }

                // Check Window Resize Hit-Testing (only when not maximized)
                if (!win->maximized) {
                    ResizeZone rz = get_window_resize_zone(*win, mx, my);
                    if (rz != RESIZE_NONE) {
                        resizing_win = win->id;
                        resizing_zone = rz;
                        resize_start_mx = mx;
                        resize_start_my = my;
                        resize_start_x = win->x;
                        resize_start_w = win->w;
                        resize_start_h = win->h;
                        bring_to_front(win);
                        click_handled = true;
                        break;
                    }
                }

                // Browser content interaction (toolbar, bookmarks, links)
                if (win->id == 4 && my >= win->y + 28 && my < win->y + win->h &&
                    mx >= win->x && mx < win->x + win->w) {
                    if (handle_browser_click(mx, my)) {
                        bring_to_front(win);
                        click_handled = true;
                        break;
                    }
                }

                // Task Manager kill controls
                if (win->id == 5 && my >= win->y + 28 && my < win->y + win->h &&
                    mx >= win->x && mx < win->x + win->w) {
                    if (handle_tm_click(mx, my)) {
                        click_handled = true;
                        break;
                    }
                }

                // Settings / updater controls
                if (win->id == 6 && my >= win->y + 28 && my < win->y + win->h &&
                    mx >= win->x && mx < win->x + win->w) {
                    if (handle_settings_click(mx, my)) {
                        click_handled = true;
                        break;
                    }
                }

                // App Hub controls
                if (win->id == 7 && my >= win->y + 28 && my < win->y + win->h &&
                    mx >= win->x && mx < win->x + win->w) {
                    if (handle_hub_click(mx, my)) {
                        click_handled = true;
                        break;
                    }
                }

                // Check Window Body Click (focus window to front)
                if (mx >= win->x && mx < win->x + win->w && my >= win->y + 28 && my < win->y + win->h) {
                    bring_to_front(win);
                    click_handled = true;
                    break;
                }
            }

            // 2. Check Menu Bar / Taskbar Buttons
            if (!click_handled && my >= static_cast<int>(bar_y) && my <= static_cast<int>(bar_y) + 36) {
                if (mx >= 6 && mx <= 106) {
                    start_menu_open = !start_menu_open;
                    full_redraw_needed = true;
                    click_handled = true;
                } else if (mx >= 114 && mx <= 230) {
                    handle_taskbar_click(&win1);
                    click_handled = true;
                } else if (mx >= 238 && mx <= 354) {
                    handle_taskbar_click(&win2);
                    click_handled = true;
                } else if (mx >= 362 && mx <= 478) {
                    handle_taskbar_click(&win3);
                    click_handled = true;
                } else if (mx >= 486 && mx <= 606) {
                    current_bar_pos = (current_bar_pos == BAR_TOP) ? BAR_BOTTOM : BAR_TOP;
                    reset_window_layout();
                    wp_cache_valid = false;
                    full_redraw_needed = true;
                    click_handled = true;
                } else if (mx >= 614 && mx <= 724) {
                    current_button_style = (current_button_style == BUTTONS_CIRCLES) ? BUTTONS_SQUARES : BUTTONS_CIRCLES;
                    full_redraw_needed = true;
                    click_handled = true;
                } else if (mx >= static_cast<int>(width) - 355 && mx <= static_cast<int>(width) - 220) {
                    cycle_fps();
                    full_redraw_needed = true;
                    click_handled = true;
                }
            }

            // 3. Check Start Menu Items
            if (!click_handled && start_menu_open) {
                uint32_t sm_y = (current_bar_pos == BAR_TOP) ? 38 : (height - 38 - 344);
                int rel = my - static_cast<int>(sm_y);
                if (mx >= 6 && mx <= 266) {
                    if (rel >= 36 && rel < 62) {
                        win1.visible = true; win1.minimized = false;
                        bring_to_front(&win1);
                        start_menu_open = false;
                        click_handled = true;
                    } else if (rel >= 62 && rel < 88) {
                        win2.visible = true; win2.minimized = false;
                        bring_to_front(&win2);
                        start_menu_open = false;
                        click_handled = true;
                    } else if (rel >= 88 && rel < 114) {
                        win4.visible = true; win4.minimized = false;
                        bring_to_front(&win4);
                        start_menu_open = false;
                        click_handled = true;
                    } else if (rel >= 114 && rel < 140) {
                        launch_win3();
                        start_menu_open = false;
                        click_handled = true;
                    } else if (rel >= 140 && rel < 166) {
                        launch_win5();
                        start_menu_open = false;
                        click_handled = true;
                    } else if (rel >= 166 && rel < 192) {
                        launch_win6();
                        start_menu_open = false;
                        click_handled = true;
                    } else if (rel >= 192 && rel < 218) {
                        launch_win7();
                        start_menu_open = false;
                        click_handled = true;
                    } else if (rel >= 218 && rel < 244) {
                        break; // Exit to Console clicked!
                    }
                } else {
                    start_menu_open = false;
                    full_redraw_needed = true;
                }
            }

            // 4. Desktop Launcher Icons (reachable only if no window, the
            //    taskbar or the start menu already consumed this click)
            if (!click_handled) {
                for (int i = 0; i < NICON; ++i) {
                    int cx, cy, r;
                    icon_center(i, cx, cy, r);
                    int dx = mx - cx, dy = my - cy;
                    if (dx * dx + dy * dy <= r * r) {
                        if (i == 0) {
                            win2.visible = true; win2.minimized = false; bring_to_front(&win2);
                        } else if (i == 1) {
                            win4.visible = true; win4.minimized = false; bring_to_front(&win4);
                        } else if (i == 2) {
                            launch_win3();
                        } else if (i == 3) {
                            win1.visible = true; win1.minimized = false; bring_to_front(&win1);
                        } else if (i == 4) {
                            launch_win5();
                        } else if (i == 5) {
                            launch_win6();
                        } else {
                            launch_win7();
                        }
                        click_handled = true;
                        break;
                    }
                }
            }
        } else if (!left_down) {
            if (dragging_win != 0) {
                // One clean full repaint after a drag so nothing is left behind.
                drag_prev_valid = false;
                drag_dmg_valid = false;
                full_redraw_needed = true;
            }
            dragging_win = 0;
            resizing_win = 0;
            resizing_zone = RESIZE_NONE;
        }

        // Apply Window Dragging
        if (dragging_win != 0) {
            WindowState* dw = (dragging_win == 1) ? &win1
                             : (dragging_win == 2) ? &win2
                             : (dragging_win == 3) ? &win3
                             : (dragging_win == 4) ? &win4
                             : (dragging_win == 5) ? &win5
                             : (dragging_win == 6) ? &win6 : &win7;
            int new_x = mx - drag_offset_x;
            int new_y = my - drag_offset_y;
            if (new_x != dw->x || new_y != dw->y) {
                // Erase last frame's cursor so it isn't baked into the region.
                restore_cursor_background();
                dw->x = new_x; dw->y = new_y;
                dw->saved_x = new_x; dw->saved_y = new_y;
                drag_dmg_valid = false;
                full_redraw_needed = true;
            }
        }

        // Apply Window Resizing
        if (resizing_win != 0) {
            WindowState* rw = (resizing_win == 1) ? &win1
                            : (resizing_win == 2) ? &win2
                            : (resizing_win == 3) ? &win3
                            : (resizing_win == 4) ? &win4
                            : (resizing_win == 5) ? &win5
                            : (resizing_win == 6) ? &win6 : &win7;
            int dx = mx - resize_start_mx;
            int dy = my - resize_start_my;

            int min_w = 200, min_h = 150;
            if (rw->id == 1) { min_w = 300; min_h = 220; }
            else if (rw->id == 2) { min_w = 240; min_h = 160; }
            else if (rw->id == 3) { min_w = 280; min_h = 200; }
            else if (rw->id == 4) { min_w = 360; min_h = 240; }
            else if (rw->id == 6) { min_w = 460; min_h = 300; }
            else if (rw->id == 7) { min_w = 480; min_h = 360; }

            int max_w = static_cast<int>(width);
            int max_h = static_cast<int>(height);

            int new_x = rw->x;
            int new_y = rw->y;
            int new_w = rw->w;
            int new_h = rw->h;

            switch (resizing_zone) {
                case RESIZE_SE: // Bottom-Right: changes width & height
                    new_w = resize_start_w + dx;
                    new_h = resize_start_h + dy;
                    break;
                case RESIZE_E: // Right Edge: changes width
                    new_w = resize_start_w + dx;
                    break;
                case RESIZE_S: // Bottom Edge: changes height
                    new_h = resize_start_h + dy;
                    break;
                case RESIZE_W: { // Left Edge: changes width and x-position
                    int right_edge = resize_start_x + resize_start_w;
                    new_w = resize_start_w - dx;
                    if (new_w < min_w) new_w = min_w;
                    if (new_w > max_w) new_w = max_w;
                    new_x = right_edge - new_w;
                    if (new_x < 0) {
                        new_x = 0;
                        new_w = right_edge;
                    }
                    break;
                }
                case RESIZE_SW: { // Bottom-Left: changes width, height, and x-position
                    int right_edge = resize_start_x + resize_start_w;
                    new_w = resize_start_w - dx;
                    if (new_w < min_w) new_w = min_w;
                    if (new_w > max_w) new_w = max_w;
                    new_x = right_edge - new_w;
                    if (new_x < 0) {
                        new_x = 0;
                        new_w = right_edge;
                    }
                    new_h = resize_start_h + dy;
                    break;
                }
                default:
                    break;
            }

            // Clamp constraints
            if (resizing_zone != RESIZE_W && resizing_zone != RESIZE_SW) {
                if (new_w < min_w) new_w = min_w;
                if (new_w > max_w) new_w = max_w;
                if (new_x + new_w > max_w) new_w = max_w - new_x;
            }
            if (new_h < min_h) new_h = min_h;
            if (new_h > max_h) new_h = max_h;
            if (new_y + new_h > max_h) new_h = max_h - new_y;

            if (new_x != rw->x || new_y != rw->y || new_w != rw->w || new_h != rw->h) {
                rw->x = new_x;
                rw->y = new_y;
                rw->w = new_w;
                rw->h = new_h;
                // Update saved dimensions so restore from maximize restores user's resized dimensions
                rw->saved_x = new_x;
                rw->saved_y = new_y;
                rw->saved_w = new_w;
                rw->saved_h = new_h;
                if (rw->id == 3) {
                    win3_hud_dirty = true;
                    win3_cube_prev.valid = false;
                }
                full_redraw_needed = true;
            }
        }

        prev_left_click = left_down;

        // Update browser link/button hover state (status bar + highlight)
        if (win4.visible && !win4.minimized) {
            int bg_vp_x, bg_vp_y, bg_vp_w, bg_vp_h, bg_tb_y, bg_bm_y;
            browser_geom(win4, bg_vp_x, bg_vp_y, bg_vp_w, bg_vp_h, bg_tb_y, bg_bm_y);
            int hover_box = -1;
            if (dragging_win == 0 && !start_menu_open &&
                bg_vp_w > 20 && bg_vp_h > 20 &&
                mx >= bg_vp_x && mx < bg_vp_x + bg_vp_w &&
                my >= bg_vp_y && my < bg_vp_y + bg_vp_h) {
                hover_box = browser::update_hover(*browser_doc, mx - bg_vp_x, my - bg_vp_y, browser_doc->scroll_y);
            } else {
                browser::update_hover(*browser_doc, -1, -1, browser_doc->scroll_y);
            }
            if (hover_box != prev_browser_hover_box) {
                prev_browser_hover_box = hover_box;
                if (hover_box >= 0) {
                    char link_abs[256];
                    browser::resolve_url(browser_doc->current_url,
                                         browser_doc->boxes[hover_box].link_url,
                                         link_abs, sizeof(link_abs));
                    kstd::strncpy(browser_hover_url, link_abs, sizeof(browser_hover_url) - 1);
                    browser_hover_url[sizeof(browser_hover_url) - 1] = '\0';
                } else {
                    browser_hover_url[0] = '\0';
                }
                win4_dirty = true;
            }
        }

        // Scrollbar thumb dragging (browser)
        if (browser_scroll_drag) {
            if (left_down && win4.visible && !win4.minimized) {
                int bg_vp_x, bg_vp_y, bg_vp_w, bg_vp_h, bg_tb_y, bg_bm_y;
                browser_geom(win4, bg_vp_x, bg_vp_y, bg_vp_w, bg_vp_h, bg_tb_y, bg_bm_y);
                int sb_x, sb_y, sb_w, sb_h, thumb_h, max_scroll;
                browser_scroll_metrics(win4, bg_vp_y, bg_vp_h, sb_x, sb_y, sb_w, sb_h, thumb_h, max_scroll);
                browser_set_scroll_from_thumb(my - browser_scroll_grab, sb_y, sb_h, thumb_h, max_scroll);
            } else {
                browser_scroll_drag = false;
            }
        }

        // Mouse-wheel scrolling (browser viewport under the cursor)
        int wheel_delta = mouse::take_wheel();
        if (wheel_delta != 0 && win4.visible && !win4.minimized) {
            int bg_vp_x, bg_vp_y, bg_vp_w, bg_vp_h, bg_tb_y, bg_bm_y;
            browser_geom(win4, bg_vp_x, bg_vp_y, bg_vp_w, bg_vp_h, bg_tb_y, bg_bm_y);
            if (mx >= bg_vp_x && mx < bg_vp_x + bg_vp_w &&
                my >= bg_vp_y && my < bg_vp_y + bg_vp_h) {
                int doc_h = (browser_doc->page_height > bg_vp_h) ? browser_doc->page_height : bg_vp_h;
                int max_scroll = doc_h - bg_vp_h;
                if (max_scroll < 0) max_scroll = 0;
                browser_doc->scroll_y -= wheel_delta * 48;
                if (browser_doc->scroll_y < 0) browser_doc->scroll_y = 0;
                if (browser_doc->scroll_y > max_scroll) browser_doc->scroll_y = max_scroll;
                win4_dirty = true;
            }
        }

        // Perform full redraw, fast drag-region repaint, or restore cursor bg
        bool used_full_render = false;
        bool used_drag_region = false;
        drag_dmg_valid = false;
        if (dragging_win != 0) {
            WindowState* dw = (dragging_win == 1) ? &win1
                             : (dragging_win == 2) ? &win2
                             : (dragging_win == 3) ? &win3
                             : (dragging_win == 4) ? &win4
                             : (dragging_win == 5) ? &win5
                             : (dragging_win == 6) ? &win6 : &win7;
            used_drag_region = render_drag_region(*dw);
            if (!used_drag_region) {
                render_full_desktop();
                used_full_render = true;
            }
            full_redraw_needed = false;
        } else if (full_redraw_needed) {
            render_full_desktop();
            used_full_render = true;
        } else {
            restore_cursor_background();
        }

        bool top_is_max = (win_stack[TOP]->visible && !win_stack[TOP]->minimized && win_stack[TOP]->maximized);

        // Update Dynamic Window Elements
        bool drew_win4 = false;
        bool drew_win1 = false;
        bool drew_win2 = false;
        if (top_is_max) {
            if (win_stack[TOP]->id == 1 && win1_dirty) {
                draw_win1_content(*win_stack[TOP], false);
                drew_win1 = true;
                win1_dirty = false;
            } else if (win_stack[TOP]->id == 2 && win2_dirty) {
                draw_win2_content(*win_stack[TOP]);
                drew_win2 = true;
                win2_dirty = false;
            } else if (win_stack[TOP]->id == 3 && win3.visible && !win3.minimized) {
                draw_win3_content(*win_stack[TOP], false);
            } else if (win_stack[TOP]->id == 4 && win4.visible && !win4.minimized && win4_dirty) {
                draw_win4_content(*win_stack[TOP], false);
                drew_win4 = true;
            } else if (win_stack[TOP]->id == 5 && win5.visible && !win5.minimized) {
                draw_win5_content(*win_stack[TOP]);
            } else if (win_stack[TOP]->id == 6 && win6.visible && !win6.minimized) {
                draw_win6_content(*win_stack[TOP]);
            }
        } else {
            if (win1.visible && !win1.minimized && win1_dirty && !win_obscured(win1)) {
                draw_win1_content(win1, false);
                drew_win1 = true;
                win1_dirty = false;
            }
            if (win2.visible && !win2.minimized && win2_dirty && !win_obscured(win2)) {
                draw_win2_content(win2);
                drew_win2 = true;
                win2_dirty = false;
            }
            if (win3.visible && !win3.minimized) {
                draw_win3_content(win3, false);
            }
            if (win4.visible && !win4.minimized && win4_dirty && !win_obscured(win4)) {
                draw_win4_content(win4, false);
                drew_win4 = true;
            }
            if (win5.visible && !win5.minimized && !win_obscured(win5)) {
                draw_win5_content(win5);
            }
            if (win6.visible && !win6.minimized && !win_obscured(win6)) {
                draw_win6_content(win6);
            }
        }

        // Update Menu Bar Dynamic Badges
        bar_y = (current_bar_pos == BAR_TOP) ? 0 : (height - 36);
        char num_buf[16];

        // Start Button
        uint32_t start_col = start_menu_open ? gfx::Colors::Teal : gfx::Colors::LightNavy;
        gfx::fill_rect(6, bar_y + 4, 100, 28, start_col);
        gfx::draw_rect(6, bar_y + 4, 100, 28, gfx::Colors::Teal);
        gfx::draw_string(" AETHER", 16, bar_y + 10, gfx::Colors::White);

        if (start_menu_open) {
            uint32_t sm_w = 260;
            uint32_t sm_h = 344;
            uint32_t sm_y = (current_bar_pos == BAR_TOP) ? 38 : (height - 38 - sm_h);
            gfx::fill_rect(6, sm_y, sm_w, sm_h, gfx::Colors::Slate);
            gfx::draw_rect(6, sm_y, sm_w, sm_h, gfx::Colors::Teal);
            gfx::draw_gradient_v(6, sm_y, sm_w, 28, gfx::Colors::LightNavy, gfx::Colors::Navy);
            gfx::draw_string("AetherOS Menu", 16, sm_y + 6, gfx::Colors::White);

            // Row i occupies [sm_y + 36 + i*26, +26).
            gfx::draw_string(win1.visible && !win1.minimized ? "> [Focus] System Monitor" : "> [Show] System Monitor", 16, sm_y + 42, gfx::Colors::Teal);
            gfx::draw_string(win2.visible && !win2.minimized ? "> [Focus] Terminal"       : "> [Show] Terminal",       16, sm_y + 68, gfx::Colors::White);
            gfx::draw_string(win4.visible && !win4.minimized ? "> [Focus] Web Browser"    : "> [Show] Web Browser",    16, sm_y + 94, gfx::Colors::White);
            gfx::draw_string(win3.visible && !win3.minimized ? "> [Focus] 3D Engine"      : (!win3.visible ? "> [+ Launch] 3D Engine" : "> [Restore] 3D Engine"), 16, sm_y + 120, !win3.visible ? gfx::Colors::Cyan : gfx::Colors::White);
            gfx::draw_string(win5.visible && !win5.minimized ? "> [Focus] Task Manager"   : "> [Show] Task Manager",   16, sm_y + 146, win5.visible ? gfx::Colors::White : gfx::Colors::Cyan);
            gfx::draw_string(win6.visible && !win6.minimized ? "> [Focus] Settings"       : "> [Show] Settings",       16, sm_y + 172, win6.visible ? gfx::Colors::White : gfx::Colors::Cyan);
            gfx::draw_string(win7.visible && !win7.minimized ? "> [Focus] App Hub"        : "> [Show] App Hub",        16, sm_y + 198, win7.visible ? gfx::Colors::White : gfx::Colors::Cyan);

            // Exit to Console Shell (final menu item, immediately after the
            // launchers; the settings toggles below were removed as redundant
            // with the Settings window).
            gfx::draw_string("> Exit to Console Shell", 16, sm_y + 224, gfx::Colors::Red);
        }

        // Real-Time Clock
        rtc::DateTime dt = rtc::get_datetime();
        bool clock_dirty = false;
        int clock_x = width - 120;
        int clock_y = bar_y + 4;
        int clock_w = 114;
        int clock_h = 28;

        if (dt.second != last_clock_second) {
            last_clock_second = dt.second;
            char time_str[32];
            time_str[0] = '\0';
            if (dt.hour < 10) kstd::strcpy(time_str, "0");
            kstd::utoa(dt.hour, num_buf, 10);
            kstd::strcpy(time_str + kstd::strlen(time_str), num_buf);
            kstd::strcpy(time_str + kstd::strlen(time_str), ":");

            if (dt.minute < 10) kstd::strcpy(time_str + kstd::strlen(time_str), "0");
            kstd::utoa(dt.minute, num_buf, 10);
            kstd::strcpy(time_str + kstd::strlen(time_str), num_buf);
            kstd::strcpy(time_str + kstd::strlen(time_str), ":");

            if (dt.second < 10) kstd::strcpy(time_str + kstd::strlen(time_str), "0");
            kstd::utoa(dt.second, num_buf, 10);
            kstd::strcpy(time_str + kstd::strlen(time_str), num_buf);
            kstd::strcpy(time_str + kstd::strlen(time_str), " UTC");

            // Redraw clock badge with background fill to prevent character smudging
            gfx::fill_rect(clock_x, clock_y, clock_w, clock_h, 0xFF14243B);
            gfx::draw_rect(clock_x, clock_y, clock_w, clock_h, gfx::Colors::LightNavy);
            gfx::draw_string(time_str, width - 112, bar_y + 10, gfx::Colors::Teal, 0xFF14243B, false);
            clock_dirty = true;
        }

        // Live FPS Badge on Menu Bar
        bool fps_badge_dirty = false;
        if (measured_fps != menu_last_fps) {
            menu_last_fps = measured_fps;
            char live_fps[32];
            kstd::strcpy(live_fps, "FPS: ");
            kstd::utoa(measured_fps, num_buf, 10);
            kstd::strcpy(live_fps + kstd::strlen(live_fps), num_buf);
            gfx::fill_rect(width - 210, bar_y + 4, 85, 28, 0xFF14243B);
            gfx::draw_rect(width - 210, bar_y + 4, 85, 28, gfx::Colors::LightNavy);
            gfx::draw_string(live_fps, width - 204, bar_y + 10, gfx::Colors::Green, 0xFF14243B, false);
            fps_badge_dirty = true;
        }

        // Determine Mouse Cursor Shape based on resize hover/dragging
        CursorType active_cursor = CURSOR_POINTER;
        if (resizing_win != 0) {
            switch (resizing_zone) {
                case RESIZE_SE: active_cursor = CURSOR_RESIZE_NWSE; break;
                case RESIZE_SW: active_cursor = CURSOR_RESIZE_NESW; break;
                case RESIZE_E:
                case RESIZE_W:  active_cursor = CURSOR_RESIZE_EW;   break;
                case RESIZE_S:  active_cursor = CURSOR_RESIZE_NS;   break;
                default:        active_cursor = CURSOR_POINTER;     break;
            }
        } else if (dragging_win == 0 && !start_menu_open) {
            bool over_bar = (my >= static_cast<int>(bar_y) && my <= static_cast<int>(bar_y) + 36);
            if (!over_bar) {
                for (int i = TOP; i >= 0; --i) {
                    WindowState* win = win_stack[i];
                    if (!win->visible || win->minimized) continue;
                    if (win->maximized) {
                        break;
                    }
                    ResizeZone rz = get_window_resize_zone(*win, mx, my);
                    if (rz != RESIZE_NONE) {
                        switch (rz) {
                            case RESIZE_SE: active_cursor = CURSOR_RESIZE_NWSE; break;
                            case RESIZE_SW: active_cursor = CURSOR_RESIZE_NESW; break;
                            case RESIZE_E:
                            case RESIZE_W:  active_cursor = CURSOR_RESIZE_EW;   break;
                            case RESIZE_S:  active_cursor = CURSOR_RESIZE_NS;   break;
                            default:        active_cursor = CURSOR_POINTER;     break;
                        }
                        break;
                    }
                    if (mx >= win->x && mx < win->x + win->w && my >= win->y && my < win->y + win->h) {
                        break;
                    }
                }
            }
        }
        current_cursor_type = active_cursor;

        int cur_hx, cur_hy, cur_cw, cur_ch;
        get_cursor_info(current_cursor_type, cur_hx, cur_hy, cur_cw, cur_ch);
        int cur_draw_x = mx - cur_hx;
        int cur_draw_y = my - cur_hy;
        int cur_draw_w = cur_cw + 2;
        int cur_draw_h = cur_ch + 2;

        // Draw Mouse Cursor with active shape
        save_and_draw_cursor(mx, my, current_cursor_type);

        // High-Speed Dirty-Rectangle Flushing Engine
        if (used_full_render || (!used_drag_region && (full_redraw_needed || resizing_win != 0 || start_menu_open))) {
            gfx::swap_buffers();
            full_redraw_needed = false;
            win3_cube_prev = win3_cube_curr;
            win3_hud_dirty = false;
            prev_draw_x = cur_draw_x;
            prev_draw_y = cur_draw_y;
            prev_draw_w = cur_draw_w;
            prev_draw_h = cur_draw_h;
            prev_cursor_type = current_cursor_type;
        } else if (used_drag_region && drag_dmg_valid) {
            // Only flush the rectangle that changed plus the moving cursor.
            gfx::swap_rect(drag_dmg_x, drag_dmg_y, drag_dmg_w, drag_dmg_h);
            if (clock_dirty) {
                gfx::swap_rect(clock_x, clock_y, clock_w, clock_h);
            }
            if (fps_badge_dirty) {
                gfx::swap_rect(width - 210, bar_y + 4, 85, 28);
            }
            if (prev_draw_x != cur_draw_x || prev_draw_y != cur_draw_y || prev_cursor_type != current_cursor_type) {
                if (prev_draw_x >= 0 && prev_draw_y >= 0) {
                    gfx::swap_rect(prev_draw_x, prev_draw_y, prev_draw_w, prev_draw_h);
                }
                gfx::swap_rect(cur_draw_x, cur_draw_y, cur_draw_w, cur_draw_h);
            }
            win3_cube_prev = win3_cube_curr;
            win3_hud_dirty = false;
            prev_draw_x = cur_draw_x;
            prev_draw_y = cur_draw_y;
            prev_draw_w = cur_draw_w;
            prev_draw_h = cur_draw_h;
            prev_cursor_type = current_cursor_type;
        } else {
            const WindowState* active_w3 = top_is_max ? ((win_stack[TOP]->id == 3 && win3.visible && !win3.minimized) ? win_stack[TOP] : nullptr) : ((win3.visible && !win3.minimized) ? &win3 : nullptr);
            const WindowState* active_w1 = top_is_max ? ((win_stack[TOP]->id == 1) ? win_stack[TOP] : nullptr) : ((win1.visible && !win1.minimized) ? &win1 : nullptr);
            const WindowState* active_w2 = top_is_max ? ((win_stack[TOP]->id == 2) ? win_stack[TOP] : nullptr) : ((win2.visible && !win2.minimized) ? &win2 : nullptr);

            if (active_w3) {
                // Compute union bounding box between previous and current frames
                BoundingBox u_box = win3_cube_curr;
                if (win3_cube_prev.valid) {
                    if (win3_cube_prev.min_x < u_box.min_x) u_box.min_x = win3_cube_prev.min_x;
                    if (win3_cube_prev.min_y < u_box.min_y) u_box.min_y = win3_cube_prev.min_y;
                    if (win3_cube_prev.max_x > u_box.max_x) u_box.max_x = win3_cube_prev.max_x;
                    if (win3_cube_prev.max_y > u_box.max_y) u_box.max_y = win3_cube_prev.max_y;
                }
                int uw = u_box.max_x - u_box.min_x + 1;
                int uh = u_box.max_y - u_box.min_y + 1;
                if (uw > 0 && uh > 0) {
                    gfx::swap_rect(u_box.min_x, u_box.min_y, uw, uh);
                }
                win3_cube_prev = win3_cube_curr;

                // Swap HUD only when changed
                if (win3_hud_dirty) {
                    gfx::swap_rect(active_w3->x + 20, active_w3->y + active_w3->h - 58, active_w3->w - 40, 50);
                    win3_hud_dirty = false;
                }
            }

            if (active_w1 && drew_win1) {
                int dyn_h = (active_w1->h > 80) ? (active_w1->h - 82) : 20;
                gfx::swap_rect(active_w1->x + 18, active_w1->y + 76, active_w1->w - 36, dyn_h);
            }

            if (active_w2 && drew_win2) {
                gfx::swap_rect(active_w2->x + 10, active_w2->y + 35, active_w2->w - 20, active_w2->h - 48);
            }

            // Flush browser window interior only when its content changed
            if (drew_win4 && win4.visible && !win4.minimized) {
                gfx::swap_rect(win4.x + 2, win4.y + 28, win4.w - 4, win4.h - 28);
            }

            // Flush clock badge only when second changed
            if (clock_dirty) {
                gfx::swap_rect(clock_x, clock_y, clock_w, clock_h);
            }

            // Flush FPS badge only when updated
            if (fps_badge_dirty) {
                gfx::swap_rect(width - 210, bar_y + 4, 85, 28);
            }

            // Flush cursor movement or shape change
            if (prev_draw_x != cur_draw_x || prev_draw_y != cur_draw_y || prev_cursor_type != current_cursor_type) {
                if (prev_draw_x >= 0 && prev_draw_y >= 0) {
                    gfx::swap_rect(prev_draw_x, prev_draw_y, prev_draw_w, prev_draw_h);
                }
                gfx::swap_rect(cur_draw_x, cur_draw_y, cur_draw_w, cur_draw_h);
            }
            prev_draw_x = cur_draw_x;
            prev_draw_y = cur_draw_y;
            prev_draw_w = cur_draw_w;
            prev_draw_h = cur_draw_h;
            prev_cursor_type = current_cursor_type;
        }

        // Frame Pacing & FPS Measurement
        uint64_t frame_end = pit::uptime_ms();
        frame_time_ms = static_cast<uint32_t>(frame_end - frame_start);
        if (frame_time_ms == 0) frame_time_ms = 1;

        frame_count++;
        if (frame_end - last_fps_time >= 500) {
            measured_fps = static_cast<uint32_t>((frame_count * 1000) / (frame_end - last_fps_time));
            frame_count = 0;
            last_fps_time = frame_end;
        }

        uint32_t cur_target_fps = fps_modes[current_fps_idx];
        if (cur_target_fps > 0) {
            uint32_t target_interval_ms = 1000 / cur_target_fps;
            uint64_t elapsed = pit::uptime_ms() - frame_start;
            if (elapsed < target_interval_ms) {
                pit::sleep(target_interval_ms - elapsed);
            }
        }
    }

    gfx::exit();
}

void run_demo(uint32_t width, uint32_t height) {
    if (!gfx::init(width, height)) {
        if (!gfx::init(1280, 720)) {
            if (!gfx::init(1024, 768)) {
                printk("[GFX] Failed to initialize graphics mode!\n");
                return;
            }
        }
    }

    width = gfx::get_width();
    height = gfx::get_height();
    mouse::set_bounds(width, height);
    mouse::set_position(width / 2, height / 2);

    while (keyboard::has_char()) keyboard::get_char();

    while (true) {
        if (keyboard::has_char()) {
            keyboard::get_char();
            break;
        }

        int mx = mouse::get_x();
        int my = mouse::get_y();

        gfx::draw_gradient_v(0, 0, width, height, 0xFF0A1828, 0xFF1E3A5F);

        gfx::draw_string("=== AetherOS 2D Graphics Engine High-Resolution Test Pattern ===", width / 2 - 280, 30, gfx::Colors::White);

        uint32_t test_colors[8] = {
            gfx::Colors::Red, gfx::Colors::Green, gfx::Colors::Blue, gfx::Colors::Yellow,
            gfx::Colors::Cyan, gfx::Colors::Magenta, gfx::Colors::Orange, gfx::Colors::Teal
        };
        int swatch_start_x = (width > 1200) ? (width / 2 - 340) : 100;
        for (int i = 0; i < 8; ++i) {
            gfx::fill_rect(swatch_start_x + i * 85, 80, 75, 45, test_colors[i]);
            gfx::draw_rect(swatch_start_x + i * 85, 80, 75, 45, gfx::Colors::White);
        }

        int circle_cx = width / 4;
        int circle_cy = height / 3 + 40;
        for (int r = 10; r < 120; r += 14) {
            gfx::draw_circle(circle_cx, circle_cy, r, gfx::Colors::Cyan);
        }
        gfx::fill_circle(circle_cx, circle_cy, 10, gfx::Colors::Yellow);

        int grad_x = width / 2 - 100;
        int grad_y = height / 3;
        gfx::draw_gradient_v(grad_x, grad_y, 200, 180, gfx::Colors::Magenta, gfx::Colors::Blue);
        gfx::draw_rect(grad_x, grad_y, 200, 180, gfx::Colors::White);
        gfx::draw_string("Vertical Gradient", grad_x + 30, grad_y + 80, gfx::Colors::White);

        int star_cx = (width * 3) / 4;
        int star_cy = height / 3 + 40;
        for (int deg = 0; deg < 256; deg += 16) {
            int lx = star_cx + (cos_fp(deg) * 90) / 256;
            int ly = star_cy + (sin_fp(deg) * 90) / 256;
            gfx::draw_line(star_cx, star_cy, lx, ly, gfx::Colors::Green);
        }

        int info_w = (width > 1200) ? 800 : (width - 100);
        int info_x = (width - info_w) / 2;
        int info_y = height - 260;
        gfx::fill_rect(info_x, info_y, info_w, 180, 0xFF081220);
        gfx::draw_rect(info_x, info_y, info_w, 180, gfx::Colors::LightNavy);
        gfx::draw_string("Embedded 8x16 VGA Bitmap Font Showcase:", info_x + 30, info_y + 25, gfx::Colors::Teal);
        gfx::draw_string("ABCDEFGHIJKLMNOPQRSTUVWXYZ  abcdefghijklmnopqrstuvwxyz", info_x + 30, info_y + 55, gfx::Colors::White);
        gfx::draw_string("0123456789  !@#$%^&*()_+-=[]{}|;':\",./<>?", info_x + 30, info_y + 80, gfx::Colors::Yellow);

        char res_info[80];
        char nbuf[16];
        kstd::strcpy(res_info, "Linear Framebuffer: ");
        kstd::utoa(width, nbuf, 10);
        kstd::strcpy(res_info + kstd::strlen(res_info), nbuf);
        kstd::strcpy(res_info + kstd::strlen(res_info), "x");
        kstd::utoa(height, nbuf, 10);
        kstd::strcpy(res_info + kstd::strlen(res_info), nbuf);
        kstd::strcpy(res_info + kstd::strlen(res_info), " 32-bit ARGB8888 Double-Buffered (144Hz+)");
        gfx::draw_string(res_info, info_x + 30, info_y + 110, gfx::Colors::Green);
        gfx::draw_string("Press any key to return to text shell...", info_x + 30, info_y + 145, gfx::Colors::White);

        save_and_draw_cursor(mx, my);

        gfx::swap_buffers();
        pit::sleep(7); // ~144 Hz
    }

    gfx::exit();
}

} // namespace gui
