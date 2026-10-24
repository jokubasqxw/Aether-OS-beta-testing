#ifndef KERNEL_E1000_HPP
#define KERNEL_E1000_HPP

#include "types.hpp"
#include "pci.hpp"

namespace e1000 {

// Intel e1000 PCI Vendor and Device IDs
constexpr uint16_t INTEL_VENDOR_ID     = 0x8086;
constexpr uint16_t E1000_DEV_82540EM   = 0x100E; // QEMU standard e1000
constexpr uint16_t E1000_DEV_82543GC   = 0x1004;
constexpr uint16_t E1000_DEV_82545EM   = 0x100F;
constexpr uint16_t E1000_DEV_82574L    = 0x10D3; // e1000e
constexpr uint16_t E1000_DEV_82540EP   = 0x1017;
constexpr uint16_t E1000_DEV_82541EI   = 0x1013;

// Ring buffer sizing
constexpr size_t RX_DESC_COUNT = 32;
constexpr size_t TX_DESC_COUNT = 32;
constexpr size_t PKT_BUF_SIZE  = 2048;

// Legacy Receive Descriptor (16 bytes)
struct RxDesc {
    volatile uint64_t buffer_addr;
    volatile uint16_t length;
    volatile uint16_t checksum;
    volatile uint8_t  status;
    volatile uint8_t  errors;
    volatile uint16_t special;
} __attribute__((packed));

// Legacy Transmit Descriptor (16 bytes)
struct TxDesc {
    volatile uint64_t buffer_addr;
    volatile uint16_t length;
    volatile uint8_t  cso;
    volatile uint8_t  cmd;
    volatile uint8_t  status;
    volatile uint8_t  css;
    volatile uint16_t special;
} __attribute__((packed));

// Driver Public API
bool init();
bool is_available();
bool has_link();

const uint8_t* get_mac();
void get_mac(uint8_t out_mac[6]);

bool send_packet(const void* data, size_t len);
size_t receive_packet(void* buf, size_t max_len);

// Statistics
uint64_t get_rx_packets();
uint64_t get_tx_packets();
uint64_t get_rx_bytes();
uint64_t get_tx_bytes();
uint64_t get_rx_errors();
uint64_t get_tx_errors();
uint64_t get_rx_dropped();

uint8_t   get_irq();
uintptr_t get_mmio_base();
uint16_t  get_device_id();
const char* get_device_name();

} // namespace e1000

#endif // KERNEL_E1000_HPP
