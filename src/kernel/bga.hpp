#ifndef KERNEL_BGA_HPP
#define KERNEL_BGA_HPP

#include "types.hpp"

namespace bga {

constexpr uint16_t VBE_DISPI_IOPORT_INDEX = 0x01CE;
constexpr uint16_t VBE_DISPI_IOPORT_DATA  = 0x01CF;

constexpr uint16_t VBE_DISPI_INDEX_ID          = 0;
constexpr uint16_t VBE_DISPI_INDEX_XRES        = 1;
constexpr uint16_t VBE_DISPI_INDEX_YRES        = 2;
constexpr uint16_t VBE_DISPI_INDEX_BPP         = 3;
constexpr uint16_t VBE_DISPI_INDEX_ENABLE      = 4;
constexpr uint16_t VBE_DISPI_INDEX_BANK        = 5;
constexpr uint16_t VBE_DISPI_INDEX_VIRT_WIDTH  = 6;
constexpr uint16_t VBE_DISPI_INDEX_VIRT_HEIGHT = 7;
constexpr uint16_t VBE_DISPI_INDEX_X_OFFSET    = 8;
constexpr uint16_t VBE_DISPI_INDEX_Y_OFFSET    = 9;

constexpr uint16_t VBE_DISPI_DISABLED          = 0x00;
constexpr uint16_t VBE_DISPI_ENABLED           = 0x01;
constexpr uint16_t VBE_DISPI_LFB_ENABLED       = 0x40;
constexpr uint16_t VBE_DISPI_NOCLEARMEM        = 0x80;

bool is_available();
uint32_t get_version();
uintptr_t get_framebuffer_addr();
size_t get_vram_size();

void set_video_mode(uint32_t width, uint32_t height, uint32_t bpp = 32, bool clear = true, bool enable_double_buffering = true);
void disable();

uint32_t get_width();
uint32_t get_height();
uint32_t get_bpp();
uint32_t get_pitch();

// Hardware Double Buffering via Y-Offset Panning
bool has_hardware_double_buffering();
int get_current_page();
void flip_page(int page);
void swap_buffers();

uint32_t* get_page_buffer(int page);
uint32_t* get_front_buffer();
uint32_t* get_back_buffer();

// Register caching & panning control
void set_y_offset(uint16_t y_offset);
uint16_t get_y_offset();
void set_x_offset(uint16_t x_offset);
uint16_t get_x_offset();
void flush_register_cache();

} // namespace bga

#endif // KERNEL_BGA_HPP
