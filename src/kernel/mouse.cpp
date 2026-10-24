#include "mouse.hpp"
#include "idt.hpp"
#include "pic.hpp"
#include "../arch/x86_64/io.hpp"

#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define KBD_COMMAND_PORT 0x64

namespace mouse {

static int mouse_x = 512;
static int mouse_y = 384;
static int max_width = 1024;
static int max_height = 768;

static bool btn_left = false;
static bool btn_right = false;
static bool btn_middle = false;

static uint8_t packet_cycle = 0;
static uint8_t packet_size = 3;
static uint8_t packet[4] = {0, 0, 0, 0};
static int wheel_accum = 0;

static inline void wait_input_empty() {
    uint32_t timeout = 100000;
    while ((arch::inb(KBD_STATUS_PORT) & 0x02) && --timeout) {}
}

static inline void wait_output_full() {
    uint32_t timeout = 100000;
    while (!(arch::inb(KBD_STATUS_PORT) & 0x01) && --timeout) {}
}

static void mouse_write(uint8_t value) {
    wait_input_empty();
    arch::outb(KBD_COMMAND_PORT, 0xD4); // Tell 8042 next byte goes to mouse
    wait_input_empty();
    arch::outb(KBD_DATA_PORT, value);
}

static uint8_t mouse_read() {
    wait_output_full();
    return arch::inb(KBD_DATA_PORT);
}

static void mouse_irq_handler(idt::Registers*) {
    uint8_t status = arch::inb(KBD_STATUS_PORT);
    // Check that output buffer has data and it's from aux port (bit 5)
    if (!(status & 0x20)) {
        return;
    }

    uint8_t b = arch::inb(KBD_DATA_PORT);

    if (packet_cycle == 0) {
        // First byte must have bit 3 set to 1
        if (b & 0x08) {
            packet[0] = b;
            packet_cycle = 1;
        }
        return;
    }

    packet[packet_cycle++] = b;
    if (packet_cycle < packet_size) {
        return;
    }
    packet_cycle = 0;

    uint8_t flags = packet[0];
    btn_left   = (flags & 0x01) != 0;
    btn_right  = (flags & 0x02) != 0;
    btn_middle = (flags & 0x04) != 0;

    int dx = static_cast<int8_t>(packet[1]);
    int dy = static_cast<int8_t>(packet[2]);

    // Handle overflow packets by clamping to max delta instead of freezing
    if (flags & 0x80) { // X overflow
        dx = (flags & 0x10) ? -127 : 127;
    }
    if (flags & 0x40) { // Y overflow
        dy = (flags & 0x20) ? -127 : 127;
    }

    mouse_x += dx;
    mouse_y -= dy; // Inverted in screen coordinates

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x >= max_width) mouse_x = max_width - 1;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y >= max_height) mouse_y = max_height - 1;

    // 4th byte carries wheel Z-axis movement (IntelliMouse)
    if (packet_size == 4) {
        int8_t z = static_cast<int8_t>(packet[3]);
        wheel_accum += z;
    }
}

void init() {
    packet_cycle = 0;
    mouse_x = max_width / 2;
    mouse_y = max_height / 2;

    // Enable auxiliary mouse device on 8042
    wait_input_empty();
    arch::outb(KBD_COMMAND_PORT, 0xA8);

    // Read controller configuration byte
    wait_input_empty();
    arch::outb(KBD_COMMAND_PORT, 0x20);
    wait_output_full();
    uint8_t status = arch::inb(KBD_DATA_PORT);

    // Enable IRQ12 (bit 1) and enable mouse clock (clear bit 5)
    status |= 0x02;
    status &= ~0x20;

    // Write back configuration byte
    wait_input_empty();
    arch::outb(KBD_COMMAND_PORT, 0x60);
    wait_input_empty();
    arch::outb(KBD_DATA_PORT, status);

    // Set default mouse settings (command 0xF6)
    mouse_write(0xF6);
    mouse_read(); // ACK (0xFA)

    // Attempt IntelliMouse negotiation to enable the scroll wheel.
    // Magic knock: sample rates 200 -> 100 -> 80, then query device ID.
    packet_size = 3;
    wheel_accum = 0;
    const uint8_t knock_rates[3] = {200, 100, 80};
    for (uint8_t rate : knock_rates) {
        mouse_write(0xF3);
        mouse_read(); // ACK
        mouse_write(rate);
        mouse_read(); // ACK
    }
    mouse_write(0xF2);        // Get device ID
    mouse_read();             // ACK
    uint8_t device_id = mouse_read();
    if (device_id == 0x03 || device_id == 0x04) {
        packet_size = 4;      // IntelliMouse: 4-byte packets with Z-axis
    }

    // Enable mouse packet streaming (command 0xF4)
    mouse_write(0xF4);
    mouse_read(); // ACK (0xFA)

    // Register IRQ12 (vector 44 = 32 + 12)
    idt::register_handler(44, mouse_irq_handler);

    // Unmask IRQ2 (cascade) and IRQ12 on PIC
    pic::unmask(2);
    pic::unmask(12);
}

void set_bounds(int max_x, int max_y) {
    max_width = max_x;
    max_height = max_y;
    if (mouse_x >= max_width) mouse_x = max_width - 1;
    if (mouse_y >= max_height) mouse_y = max_height - 1;
}

void set_position(int x, int y) {
    mouse_x = x < 0 ? 0 : (x >= max_width ? max_width - 1 : x);
    mouse_y = y < 0 ? 0 : (y >= max_height ? max_height - 1 : y);
}

int get_x() { return mouse_x; }
int get_y() { return mouse_y; }
bool is_left_down() { return btn_left; }
bool is_right_down() { return btn_right; }
bool is_middle_down() { return btn_middle; }

int take_wheel() {
    int w = wheel_accum;
    wheel_accum = 0;
    return w;
}

} // namespace mouse
