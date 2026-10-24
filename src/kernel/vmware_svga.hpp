#ifndef KERNEL_VMWARE_SVGA_HPP
#define KERNEL_VMWARE_SVGA_HPP

#include "types.hpp"
#include "pci.hpp"

namespace vmware_svga {

// PCI Identification
constexpr uint16_t PCI_VENDOR_VMWARE       = 0x15AD;
constexpr uint16_t PCI_DEVICE_SVGA2        = 0x0405;

// Magic & Version IDs
constexpr uint32_t SVGA_MAGIC              = 0x900000UL;
constexpr uint32_t SVGA_MAKE_ID(uint32_t ver) { return (SVGA_MAGIC << 8) | (ver); }
constexpr uint32_t SVGA_ID_0               = SVGA_MAKE_ID(0);
constexpr uint32_t SVGA_ID_1               = SVGA_MAKE_ID(1);
constexpr uint32_t SVGA_ID_2               = SVGA_MAKE_ID(2);

// Standard Device Register Indices
constexpr uint32_t SVGA_REG_ID             = 0;
constexpr uint32_t SVGA_REG_ENABLE         = 1;
constexpr uint32_t SVGA_REG_WIDTH          = 2;
constexpr uint32_t SVGA_REG_HEIGHT         = 3;
constexpr uint32_t SVGA_REG_MAX_WIDTH      = 4;
constexpr uint32_t SVGA_REG_MAX_HEIGHT     = 5;
constexpr uint32_t SVGA_REG_DEPTH          = 6;
constexpr uint32_t SVGA_REG_BITS_PER_PIXEL = 7;
constexpr uint32_t SVGA_REG_PSEUDOCOLOR    = 8;
constexpr uint32_t SVGA_REG_RED_MASK       = 9;
constexpr uint32_t SVGA_REG_GREEN_MASK     = 10;
constexpr uint32_t SVGA_REG_BLUE_MASK      = 11;
constexpr uint32_t SVGA_REG_BYTES_PER_LINE = 12;
constexpr uint32_t SVGA_REG_FB_START       = 13;
constexpr uint32_t SVGA_REG_FB_OFFSET      = 14;
constexpr uint32_t SVGA_REG_VRAM_SIZE      = 15;
constexpr uint32_t SVGA_REG_FB_SIZE        = 16;
constexpr uint32_t SVGA_REG_CAPABILITIES   = 17;
constexpr uint32_t SVGA_REG_MEM_START      = 18; // FIFO Base Address
constexpr uint32_t SVGA_REG_MEM_SIZE       = 19; // FIFO Size in Bytes
constexpr uint32_t SVGA_REG_CONFIG_DONE    = 20;
constexpr uint32_t SVGA_REG_SYNC           = 21;
constexpr uint32_t SVGA_REG_BUSY           = 22;
constexpr uint32_t SVGA_REG_GUEST_ID       = 23;
constexpr uint32_t SVGA_REG_CURSOR_ID      = 24;
constexpr uint32_t SVGA_REG_CURSOR_X       = 25;
constexpr uint32_t SVGA_REG_CURSOR_Y       = 26;
constexpr uint32_t SVGA_REG_CURSOR_ON      = 27;

// Device Capabilities Bitmask
constexpr uint32_t SVGA_CAP_RECT_FILL       = (1 << 0);
constexpr uint32_t SVGA_CAP_RECT_COPY       = (1 << 1);
constexpr uint32_t SVGA_CAP_RECT_PAT_FILL   = (1 << 2);
constexpr uint32_t SVGA_CAP_LEGACY_OFFSCREEN = (1 << 3);
constexpr uint32_t SVGA_CAP_RASTER_OP       = (1 << 4);
constexpr uint32_t SVGA_CAP_CURSOR          = (1 << 5);
constexpr uint32_t SVGA_CAP_CURSOR_BYPASS   = (1 << 6);
constexpr uint32_t SVGA_CAP_CURSOR_BYPASS_2 = (1 << 7);
constexpr uint32_t SVGA_CAP_8BIT_EMULATION  = (1 << 8);
constexpr uint32_t SVGA_CAP_ALPHA_CURSOR    = (1 << 9);
constexpr uint32_t SVGA_CAP_GLYPH           = (1 << 10);
constexpr uint32_t SVGA_CAP_PITCHLOCK       = (1 << 12);
constexpr uint32_t SVGA_CAP_EXTENDED_FIFO   = (1 << 15);

// FIFO Control Register Indices (within FIFO memory region)
constexpr uint32_t SVGA_FIFO_MIN            = 0;
constexpr uint32_t SVGA_FIFO_MAX            = 1;
constexpr uint32_t SVGA_FIFO_NEXT_CMD       = 2;
constexpr uint32_t SVGA_FIFO_STOP           = 3;
constexpr uint32_t SVGA_FIFO_CAPABILITIES   = 4;
constexpr uint32_t SVGA_FIFO_FLAGS          = 5;
constexpr uint32_t SVGA_FIFO_FENCE          = 6;
constexpr uint32_t SVGA_FIFO_NUM_REGS       = 293;

// 2D Acceleration FIFO Commands
constexpr uint32_t SVGA_CMD_INVALID_CMD     = 0;
constexpr uint32_t SVGA_CMD_UPDATE          = 1;
constexpr uint32_t SVGA_CMD_RECT_FILL       = 2;
constexpr uint32_t SVGA_CMD_RECT_COPY       = 3;
constexpr uint32_t SVGA_CMD_DEFINE_BITMAP   = 4;
constexpr uint32_t SVGA_CMD_DEFINE_PIXMAP   = 6;
constexpr uint32_t SVGA_CMD_DEFINE_CURSOR   = 19;
constexpr uint32_t SVGA_CMD_DISPLAY_CURSOR  = 20;
constexpr uint32_t SVGA_CMD_MOVE_CURSOR     = 21;
constexpr uint32_t SVGA_CMD_DEFINE_ALPHA_CURSOR = 22;

// Detection & Lifecycle
bool is_available();
bool init(uint32_t width = 1920, uint32_t height = 1080, uint32_t bpp = 32);
void disable();

// Configuration & Metrics
uint32_t get_width();
uint32_t get_height();
uint32_t get_bpp();
uint32_t get_pitch();
uint32_t get_capabilities();
uintptr_t get_framebuffer_addr();
uint32_t* get_framebuffer();
size_t get_framebuffer_size();
uintptr_t get_fifo_addr();
size_t get_fifo_size();

// Hardware Accelerated 2D Commands via FIFO
void update(int x, int y, int w, int h);
void rect_fill(uint32_t color, int x, int y, int w, int h);
void rect_copy(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
void fifo_rect_fill(uint32_t color, int x, int y, int w, int h);
void fifo_rect_copy(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
void set_fifo_fill_enabled(bool enabled);
bool is_fifo_fill_enabled();
void sync();

// Hardware Cursor Support
bool has_hardware_cursor();
void set_cursor_position(int x, int y);
void set_cursor_visible(bool visible);
void define_alpha_cursor(int width, int height, int hotspot_x, int hotspot_y, const uint32_t* rgba_data);

// Register I/O
uint32_t read_reg(uint32_t index);
void write_reg(uint32_t index, uint32_t val);

} // namespace vmware_svga

#endif // KERNEL_VMWARE_SVGA_HPP
