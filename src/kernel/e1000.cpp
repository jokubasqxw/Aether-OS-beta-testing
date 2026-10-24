#include "e1000.hpp"
#include "printk.hpp"
#include "kstring.hpp"
#include "pmm.hpp"
#include "heap.hpp"
#include "pic.hpp"
#include "pit.hpp"
#include "../arch/x86_64/io.hpp"

namespace e1000 {

// Hardware Registers
constexpr uint32_t REG_CTRL     = 0x0000; // Device Control
constexpr uint32_t REG_STATUS   = 0x0008; // Device Status
constexpr uint32_t REG_EECD     = 0x0010; // EEPROM/Flash Control
constexpr uint32_t REG_EERD     = 0x0014; // EEPROM Read
constexpr uint32_t REG_CTRL_EXT = 0x0018; // Extended Device Control
constexpr uint32_t REG_ICR      = 0x00C0; // Interrupt Cause Read (clears on read)
constexpr uint32_t REG_ITR      = 0x00C4; // Interrupt Throttling Rate
constexpr uint32_t REG_ICS      = 0x00C8; // Interrupt Cause Set
constexpr uint32_t REG_IMS      = 0x00D0; // Interrupt Mask Set/Read
constexpr uint32_t REG_IMC      = 0x00D8; // Interrupt Mask Clear
constexpr uint32_t REG_RCTL     = 0x0100; // Receive Control
constexpr uint32_t REG_TCTL     = 0x0400; // Transmit Control
constexpr uint32_t REG_TIPG     = 0x0410; // Transmit Inter-Packet Gap
constexpr uint32_t REG_RDBAL    = 0x2800; // Receive Descriptor Base Low
constexpr uint32_t REG_RDBAH    = 0x2804; // Receive Descriptor Base High
constexpr uint32_t REG_RDLEN    = 0x2808; // Receive Descriptor Length
constexpr uint32_t REG_RDH      = 0x2810; // Receive Descriptor Head
constexpr uint32_t REG_RDT      = 0x2818; // Receive Descriptor Tail
constexpr uint32_t REG_RDTR     = 0x2820; // Receive Delay Timer
constexpr uint32_t REG_RXDCTL   = 0x2828; // Receive Descriptor Control
constexpr uint32_t REG_TDBAL    = 0x3800; // Transmit Descriptor Base Low
constexpr uint32_t REG_TDBAH    = 0x3804; // Transmit Descriptor Base High
constexpr uint32_t REG_TDLEN    = 0x3808; // Transmit Descriptor Length
constexpr uint32_t REG_TDH      = 0x3810; // Transmit Descriptor Head
constexpr uint32_t REG_TDT      = 0x3818; // Transmit Descriptor Tail
constexpr uint32_t REG_TIDV     = 0x3820; // Transmit Interrupt Delay Value
constexpr uint32_t REG_TXDCTL   = 0x3828; // Transmit Descriptor Control
constexpr uint32_t REG_MTA      = 0x5200; // Multicast Table Array (128 dwords)
constexpr uint32_t REG_RAL      = 0x5400; // Receive Address Low
constexpr uint32_t REG_RAH      = 0x5404; // Receive Address High

// Register Bit Flags
constexpr uint32_t CTRL_SLU       = (1 << 6);  // Set Link Up
constexpr uint32_t CTRL_ASDE      = (1 << 5);  // Auto-Speed Detection Enable
constexpr uint32_t CTRL_RST       = (1 << 26); // Device Reset

constexpr uint32_t STATUS_LU      = (1 << 1);  // Link Up

constexpr uint32_t RCTL_EN        = (1 << 1);  // Receiver Enable
constexpr uint32_t RCTL_SBP       = (1 << 2);  // Store Bad Packets
constexpr uint32_t RCTL_UPE       = (1 << 3);  // Unicast Promiscuous Enable
constexpr uint32_t RCTL_MPE       = (1 << 4);  // Multicast Promiscuous Enable
constexpr uint32_t RCTL_LPE       = (1 << 5);  // Long Packet Enable
constexpr uint32_t RCTL_BAM       = (1 << 15); // Broadcast Accept Mode
constexpr uint32_t RCTL_BSIZE_2048= (0 << 16); // Buffer size 2048 bytes
constexpr uint32_t RCTL_SECRC     = (1 << 26); // Strip Ethernet CRC

constexpr uint32_t TCTL_EN        = (1 << 1);  // Transmit Enable
constexpr uint32_t TCTL_PSP       = (1 << 3);  // Pad Short Packets
constexpr uint32_t TCTL_CT_SHIFT  = 4;         // Collision Threshold
constexpr uint32_t TCTL_COLD_SHIFT= 12;        // Collision Distance

constexpr uint8_t TXD_CMD_EOP     = (1 << 0);  // End of Packet
constexpr uint8_t TXD_CMD_IFCS    = (1 << 1);  // Insert FCS/CRC
constexpr uint8_t TXD_CMD_RS      = (1 << 3);  // Report Status
constexpr uint8_t TXD_STAT_DD     = (1 << 0);  // Descriptor Done

constexpr uint8_t RXD_STAT_DD     = (1 << 0);  // Descriptor Done
constexpr uint8_t RXD_STAT_EOP    = (1 << 1);  // End of Packet

// Driver State
static bool is_initialized = false;
static pci::Device pci_dev;
static uintptr_t mmio_base = 0;
static uint8_t irq_line = 0;
static uint8_t mac_addr[6] = {0, 0, 0, 0, 0, 0};

// Descriptors & Buffers
static RxDesc* rx_descs = nullptr;
static TxDesc* tx_descs = nullptr;
static uint8_t* rx_buffers[RX_DESC_COUNT];
static uint8_t* tx_buffers[TX_DESC_COUNT];

static uint32_t rx_cur = 0;
static uint32_t tx_cur = 0;

// Statistics
static uint64_t stat_rx_packets = 0;
static uint64_t stat_tx_packets = 0;
static uint64_t stat_rx_bytes   = 0;
static uint64_t stat_tx_bytes   = 0;
static uint64_t stat_rx_errors  = 0;
static uint64_t stat_tx_errors  = 0;
static uint64_t stat_rx_dropped = 0;

// MMIO Access Helpers
static inline void write_reg(uint32_t reg, uint32_t val) {
    *reinterpret_cast<volatile uint32_t*>(mmio_base + reg) = val;
}

static inline uint32_t read_reg(uint32_t reg) {
    return *reinterpret_cast<volatile uint32_t*>(mmio_base + reg);
}

// EEPROM Read Helper
static uint16_t read_eeprom(uint8_t addr) {
    uint32_t val;
    // Standard e1000 (82540EM): write 1 | (addr << 8)
    write_reg(REG_EERD, 1 | (static_cast<uint32_t>(addr) << 8));
    uint32_t timeout = 50000;
    while (!((val = read_reg(REG_EERD)) & (1 << 4)) && --timeout) {
        arch::io_wait();
    }
    if (val & (1 << 4)) {
        return static_cast<uint16_t>(val >> 16);
    }

    // Older 82542/82543 fallback: write 1 | (addr << 2)
    write_reg(REG_EERD, 1 | (static_cast<uint32_t>(addr) << 2));
    timeout = 50000;
    while (!((val = read_reg(REG_EERD)) & (1 << 1)) && --timeout) {
        arch::io_wait();
    }
    return static_cast<uint16_t>(val >> 16);
}

// Read MAC Address from Hardware Registers / EEPROM
static void read_mac_address() {
    // 1. Try reading from RAL0 (0x5400) and RAH0 (0x5404)
    uint32_t ral = read_reg(REG_RAL);
    uint32_t rah = read_reg(REG_RAH);

    if ((rah & (1U << 31)) && (ral != 0 || (rah & 0xFFFF) != 0)) {
        mac_addr[0] = static_cast<uint8_t>(ral & 0xFF);
        mac_addr[1] = static_cast<uint8_t>((ral >> 8) & 0xFF);
        mac_addr[2] = static_cast<uint8_t>((ral >> 16) & 0xFF);
        mac_addr[3] = static_cast<uint8_t>((ral >> 24) & 0xFF);
        mac_addr[4] = static_cast<uint8_t>(rah & 0xFF);
        mac_addr[5] = static_cast<uint8_t>((rah >> 8) & 0xFF);
    } else {
        // 2. Try EEPROM
        for (uint8_t i = 0; i < 3; ++i) {
            uint16_t word = read_eeprom(i);
            mac_addr[i * 2]     = static_cast<uint8_t>(word & 0xFF);
            mac_addr[i * 2 + 1] = static_cast<uint8_t>((word >> 8) & 0xFF);
        }
    }

    // 3. Fallback to QEMU standard default MAC if unprogrammed or all zeros/FFs
    if ((mac_addr[0] == 0 && mac_addr[1] == 0 && mac_addr[2] == 0 &&
         mac_addr[3] == 0 && mac_addr[4] == 0 && mac_addr[5] == 0) ||
        (mac_addr[0] == 0xFF && mac_addr[1] == 0xFF && mac_addr[2] == 0xFF &&
         mac_addr[3] == 0xFF && mac_addr[4] == 0xFF && mac_addr[5] == 0xFF)) {
        mac_addr[0] = 0x52; mac_addr[1] = 0x54; mac_addr[2] = 0x00;
        mac_addr[3] = 0x12; mac_addr[4] = 0x34; mac_addr[5] = 0x56;
    }

    // 4. Ensure RAL0 and RAH0 are explicitly programmed with this MAC and Address Valid
    ral = static_cast<uint32_t>(mac_addr[0]) |
          (static_cast<uint32_t>(mac_addr[1]) << 8) |
          (static_cast<uint32_t>(mac_addr[2]) << 16) |
          (static_cast<uint32_t>(mac_addr[3]) << 24);
    rah = static_cast<uint32_t>(mac_addr[4]) |
          (static_cast<uint32_t>(mac_addr[5]) << 8) |
          (1U << 31); // Address Valid

    write_reg(REG_RAL, ral);
    write_reg(REG_RAH, rah);
}

bool init() {
    printk("[e1000] Probing PCI bus for Intel Gigabit Ethernet Controller...\n");

    // Scan for supported Intel e1000 PCI devices
    bool found = false;
    if (pci::find_device(INTEL_VENDOR_ID, E1000_DEV_82540EM, &pci_dev) ||
        pci::find_device(INTEL_VENDOR_ID, E1000_DEV_82543GC, &pci_dev) ||
        pci::find_device(INTEL_VENDOR_ID, E1000_DEV_82545EM, &pci_dev) ||
        pci::find_device(INTEL_VENDOR_ID, E1000_DEV_82574L,  &pci_dev) ||
        pci::find_device(INTEL_VENDOR_ID, E1000_DEV_82540EP, &pci_dev) ||
        pci::find_device(INTEL_VENDOR_ID, E1000_DEV_82541EI, &pci_dev)) {
        found = true;
    } else if (pci::find_device_by_class(0x02, 0x00, &pci_dev) && pci_dev.vendor_id == INTEL_VENDOR_ID) {
        found = true;
    }

    if (!found) {
        printk("[e1000] No supported Intel e1000 network controller detected.\n");
        return false;
    }

    printk("[e1000] Detected %s [%x:%x] at PCI %u:%u.%u\n",
           get_device_name(), pci_dev.vendor_id, pci_dev.device_id,
           pci_dev.bus, pci_dev.slot, pci_dev.func);

    // Read BAR0 (Memory Mapped IO Base)
    uint32_t bar0 = pci::get_bar(pci_dev, 0);
    if (bar0 & 1) {
        printk("[e1000] Error: BAR0 is IO space, MMIO expected!\n");
        return false;
    }
    mmio_base = static_cast<uintptr_t>(bar0 & ~0x0F);

    // Check for 64-bit BAR
    if ((bar0 & 0x06) == 0x04) {
        uint32_t bar1 = pci::get_bar(pci_dev, 1);
        mmio_base |= (static_cast<uintptr_t>(bar1) << 32);
    }

    printk("[e1000] BAR0 MMIO Physical Base: %p\n", mmio_base);

    // Enable PCI Bus Mastering and Memory/IO Access
    pci::enable_bus_mastering(pci_dev);

    // Read PCI Interrupt Line (IRQ)
    irq_line = pci::get_interrupt_line(pci_dev);
    printk("[e1000] PCI Interrupt IRQ: %u\n", irq_line);

    // 1. Device Software Reset
    write_reg(REG_IMC, 0xFFFFFFFF); // Mask all interrupts
    write_reg(REG_CTRL, read_reg(REG_CTRL) | CTRL_RST); // Assert Reset
    pit::sleep(10);

    // Wait for reset bit to self-clear
    uint32_t rst_timeout = 1000;
    while ((read_reg(REG_CTRL) & CTRL_RST) && --rst_timeout) {
        pit::sleep(1);
    }
    if (read_reg(REG_CTRL) & CTRL_RST) {
        printk("[e1000] Warning: Hardware reset timed out, continuing...\n");
    }

    // Re-enable bus mastering in case reset cleared command register
    pci::enable_bus_mastering(pci_dev);
    write_reg(REG_IMC, 0xFFFFFFFF); // Mask interrupts again
    read_reg(REG_ICR);              // Clear pending interrupts

    // 2. Set Link Up and Auto-Speed Detection
    uint32_t ctrl = read_reg(REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(1 << 3); // Clear LRST (Link Reset)
    write_reg(REG_CTRL, ctrl);

    // 3. Read Hardware MAC Address
    read_mac_address();
    printk("[e1000] Hardware MAC Address: %x:%x:%x:%x:%x:%x\n",
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5]);

    // 4. Initialize Multicast Table Array (MTA) to 0
    for (int i = 0; i < 128; ++i) {
        write_reg(REG_MTA + i * 4, 0);
    }

    // 5. Setup Receive (RX) Descriptor Ring in DMA memory
    // Allocate 4KB aligned physical frame for descriptors
    rx_descs = reinterpret_cast<RxDesc*>(pmm::alloc_frame());
    if (!rx_descs) {
        printk("[e1000] Error: Failed to allocate RX descriptor ring memory!\n");
        return false;
    }
    memset(rx_descs, 0, 4096);

    for (size_t i = 0; i < RX_DESC_COUNT; ++i) {
        rx_buffers[i] = reinterpret_cast<uint8_t*>(kmalloc(PKT_BUF_SIZE));
        if (!rx_buffers[i]) {
            printk("[e1000] Error: Failed to allocate RX buffer #%u!\n", i);
            return false;
        }
        rx_descs[i].buffer_addr = reinterpret_cast<uint64_t>(rx_buffers[i]);
        rx_descs[i].status = 0;
    }

    uint64_t rx_phys = reinterpret_cast<uint64_t>(rx_descs);
    write_reg(REG_RDBAL, static_cast<uint32_t>(rx_phys & 0xFFFFFFFF));
    write_reg(REG_RDBAH, static_cast<uint32_t>(rx_phys >> 32));
    write_reg(REG_RDLEN, RX_DESC_COUNT * sizeof(RxDesc));
    write_reg(REG_RDH, 0);
    write_reg(REG_RDT, RX_DESC_COUNT - 1);
    rx_cur = 0;

    // Enable Receive Control (RCTL)
    uint32_t rctl = RCTL_EN | RCTL_SBP | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC;
    write_reg(REG_RCTL, rctl);

    // 6. Setup Transmit (TX) Descriptor Ring in DMA memory
    tx_descs = reinterpret_cast<TxDesc*>(pmm::alloc_frame());
    if (!tx_descs) {
        printk("[e1000] Error: Failed to allocate TX descriptor ring memory!\n");
        return false;
    }
    memset(tx_descs, 0, 4096);

    for (size_t i = 0; i < TX_DESC_COUNT; ++i) {
        tx_buffers[i] = reinterpret_cast<uint8_t*>(kmalloc(PKT_BUF_SIZE));
        if (!tx_buffers[i]) {
            printk("[e1000] Error: Failed to allocate TX buffer #%u!\n", i);
            return false;
        }
        tx_descs[i].buffer_addr = reinterpret_cast<uint64_t>(tx_buffers[i]);
        tx_descs[i].cmd = 0;
        tx_descs[i].status = TXD_STAT_DD; // Initially free
    }

    uint64_t tx_phys = reinterpret_cast<uint64_t>(tx_descs);
    write_reg(REG_TDBAL, static_cast<uint32_t>(tx_phys & 0xFFFFFFFF));
    write_reg(REG_TDBAH, static_cast<uint32_t>(tx_phys >> 32));
    write_reg(REG_TDLEN, TX_DESC_COUNT * sizeof(TxDesc));
    write_reg(REG_TDH, 0);
    write_reg(REG_TDT, 0);
    tx_cur = 0;

    // Configure Transmit Inter-Packet Gap (TIPG)
    // IPGT=10, IPGR1=8, IPGR2=6 -> Standard IEEE 802.3
    write_reg(REG_TIPG, 0x0060200A);

    // Configure Transmit Control (TCTL)
    uint32_t tctl = TCTL_EN | TCTL_PSP | (15 << TCTL_CT_SHIFT) | (0x40 << TCTL_COLD_SHIFT);
    write_reg(REG_TCTL, tctl);

    // 7. Use polled RX/TX. IRQ-driven net::poll() can race with the main
    //    thread's receive buffer, corrupting long TLS streams.
    write_reg(REG_IMC, 0xFFFFFFFF);
    read_reg(REG_ICR);
    printk("[e1000] Polling mode (IRQ%u masked).\n", irq_line);

    is_initialized = true;

    // Check link status
    uint32_t status = read_reg(REG_STATUS);
    bool link_up = (status & STATUS_LU) != 0;
    printk("[e1000] Initialization complete! Link Status: %s (%s duplex, %u Mbps)\n",
           link_up ? "UP" : "DOWN",
           (status & (1 << 0)) ? "Full" : "Half",
           (status & (1 << 7)) ? 1000 : ((status & (1 << 6)) ? 100 : 10));

    return true;
}

bool is_available() {
    return is_initialized;
}

bool has_link() {
    if (!is_initialized) return false;
    return (read_reg(REG_STATUS) & STATUS_LU) != 0;
}

const uint8_t* get_mac() {
    return mac_addr;
}

void get_mac(uint8_t out_mac[6]) {
    memcpy(out_mac, mac_addr, 6);
}

bool send_packet(const void* data, size_t len) {
    if (!is_initialized || !data || len == 0 || len > 1518) {
        stat_tx_errors++;
        return false;
    }

    // Copy packet into current TX buffer
    memcpy(tx_buffers[tx_cur], data, len);

    // Configure descriptor
    tx_descs[tx_cur].length = static_cast<uint16_t>(len);
    tx_descs[tx_cur].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    tx_descs[tx_cur].status = 0;

    uint32_t sent_idx = tx_cur;
    tx_cur = (tx_cur + 1) % TX_DESC_COUNT;

    // Advance Tail to tell controller packet is ready
    write_reg(REG_TDT, tx_cur);

    // Wait for transmit to complete (DD bit set in status) with timeout
    uint64_t start = pit::uptime_ms();
    while (!(tx_descs[sent_idx].status & TXD_STAT_DD)) {
        if (pit::uptime_ms() - start > 100) {
            stat_tx_errors++;
            return false;
        }
        arch::io_wait();
    }

    stat_tx_packets++;
    stat_tx_bytes += len;
    return true;
}

size_t receive_packet(void* buf, size_t max_len) {
    if (!is_initialized || !buf || max_len == 0) return 0;

    // Check if current descriptor has received a packet
    if (!(rx_descs[rx_cur].status & RXD_STAT_DD)) {
        return 0; // No packet ready
    }

    uint16_t len = rx_descs[rx_cur].length;
    size_t copy_len = (len < max_len) ? len : max_len;

    memcpy(buf, rx_buffers[rx_cur], copy_len);

    // Clear descriptor status
    rx_descs[rx_cur].status = 0;

    // Advance tail register to recycle descriptor back to controller
    uint32_t old_cur = rx_cur;
    rx_cur = (rx_cur + 1) % RX_DESC_COUNT;
    write_reg(REG_RDT, old_cur);

    stat_rx_packets++;
    stat_rx_bytes += copy_len;
    return copy_len;
}

uint64_t get_rx_packets() { return stat_rx_packets; }
uint64_t get_tx_packets() { return stat_tx_packets; }
uint64_t get_rx_bytes()   { return stat_rx_bytes; }
uint64_t get_tx_bytes()   { return stat_tx_bytes; }
uint64_t get_rx_errors()  { return stat_rx_errors; }
uint64_t get_tx_errors()  { return stat_tx_errors; }
uint64_t get_rx_dropped() { return stat_rx_dropped; }

uint8_t   get_irq()       { return irq_line; }
uintptr_t get_mmio_base() { return mmio_base; }
uint16_t  get_device_id() { return pci_dev.device_id; }

const char* get_device_name() {
    switch (pci_dev.device_id) {
        case E1000_DEV_82540EM: return "Intel 82540EM Gigabit Ethernet";
        case E1000_DEV_82543GC: return "Intel 82543GC Gigabit Ethernet";
        case E1000_DEV_82545EM: return "Intel 82545EM Gigabit Ethernet";
        case E1000_DEV_82574L:  return "Intel 82574L Gigabit Ethernet (e1000e)";
        case E1000_DEV_82540EP: return "Intel 82540EP Gigabit Ethernet";
        case E1000_DEV_82541EI: return "Intel 82541EI Gigabit Ethernet";
        default:                return "Intel e1000 Gigabit Controller";
    }
}

} // namespace e1000
