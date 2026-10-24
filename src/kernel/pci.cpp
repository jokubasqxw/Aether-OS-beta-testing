#include "pci.hpp"
#include "printk.hpp"
#include "../arch/x86_64/io.hpp"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

namespace pci {

uint32_t read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (static_cast<uint32_t>(bus) << 16) |
                       (static_cast<uint32_t>(slot) << 11) |
                       (static_cast<uint32_t>(func) << 8) |
                       (offset & 0xFC) |
                       0x80000000;
    arch::outl(PCI_CONFIG_ADDRESS, address);
    return arch::inl(PCI_CONFIG_DATA);
}

uint16_t read_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = read_config_dword(bus, slot, func, offset);
    return static_cast<uint16_t>((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t read_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = read_config_dword(bus, slot, func, offset);
    return static_cast<uint8_t>((dword >> ((offset & 3) * 8)) & 0xFF);
}

void write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (static_cast<uint32_t>(bus) << 16) |
                       (static_cast<uint32_t>(slot) << 11) |
                       (static_cast<uint32_t>(func) << 8) |
                       (offset & 0xFC) |
                       0x80000000;
    arch::outl(PCI_CONFIG_ADDRESS, address);
    arch::outl(PCI_CONFIG_DATA, value);
}

void write_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t address = (static_cast<uint32_t>(bus) << 16) |
                       (static_cast<uint32_t>(slot) << 11) |
                       (static_cast<uint32_t>(func) << 8) |
                       (offset & 0xFC) |
                       0x80000000;
    arch::outl(PCI_CONFIG_ADDRESS, address);
    arch::outw(static_cast<uint16_t>(PCI_CONFIG_DATA + (offset & 2)), value);
}

void write_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value) {
    uint32_t address = (static_cast<uint32_t>(bus) << 16) |
                       (static_cast<uint32_t>(slot) << 11) |
                       (static_cast<uint32_t>(func) << 8) |
                       (offset & 0xFC) |
                       0x80000000;
    arch::outl(PCI_CONFIG_ADDRESS, address);
    arch::outb(static_cast<uint16_t>(PCI_CONFIG_DATA + (offset & 3)), value);
}

bool find_device(uint16_t vendor_id, uint16_t device_id, Device* out_dev) {
    for (uint16_t bus = 0; bus < 8; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            uint32_t val0 = read_config_dword(static_cast<uint8_t>(bus), slot, 0, 0);
            uint16_t vid0 = static_cast<uint16_t>(val0 & 0xFFFF);
            if (vid0 == 0xFFFF || vid0 == 0x0000) continue;

            uint8_t header_type = static_cast<uint8_t>((read_config_dword(static_cast<uint8_t>(bus), slot, 0, 0x0C) >> 16) & 0xFF);
            uint8_t num_funcs = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < num_funcs; ++func) {
                uint32_t val = read_config_dword(static_cast<uint8_t>(bus), slot, func, 0);
                uint16_t vid = static_cast<uint16_t>(val & 0xFFFF);
                if (vid == 0xFFFF || vid == 0x0000) continue;
                uint16_t did = static_cast<uint16_t>((val >> 16) & 0xFFFF);

                if (vid == vendor_id && (device_id == 0xFFFF || did == device_id)) {
                    if (out_dev) {
                        out_dev->bus = static_cast<uint8_t>(bus);
                        out_dev->slot = slot;
                        out_dev->func = func;
                        out_dev->vendor_id = vid;
                        out_dev->device_id = did;
                        uint32_t rev_class = read_config_dword(static_cast<uint8_t>(bus), slot, func, 8);
                        out_dev->class_id = static_cast<uint8_t>((rev_class >> 24) & 0xFF);
                        out_dev->subclass_id = static_cast<uint8_t>((rev_class >> 16) & 0xFF);
                        out_dev->prog_if = static_cast<uint8_t>((rev_class >> 8) & 0xFF);
                        out_dev->header_type = header_type;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

bool find_device_by_class(uint8_t class_id, uint8_t subclass_id, Device* out_dev) {
    for (uint16_t bus = 0; bus < 8; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            uint32_t val0 = read_config_dword(static_cast<uint8_t>(bus), slot, 0, 0);
            uint16_t vid0 = static_cast<uint16_t>(val0 & 0xFFFF);
            if (vid0 == 0xFFFF || vid0 == 0x0000) continue;

            uint8_t header_type = static_cast<uint8_t>((read_config_dword(static_cast<uint8_t>(bus), slot, 0, 0x0C) >> 16) & 0xFF);
            uint8_t num_funcs = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < num_funcs; ++func) {
                uint32_t val = read_config_dword(static_cast<uint8_t>(bus), slot, func, 0);
                uint16_t vid = static_cast<uint16_t>(val & 0xFFFF);
                if (vid == 0xFFFF || vid == 0x0000) continue;
                uint16_t did = static_cast<uint16_t>((val >> 16) & 0xFFFF);

                uint32_t rev_class = read_config_dword(static_cast<uint8_t>(bus), slot, func, 8);
                uint8_t cid = static_cast<uint8_t>((rev_class >> 24) & 0xFF);
                uint8_t scid = static_cast<uint8_t>((rev_class >> 16) & 0xFF);

                if (cid == class_id && scid == subclass_id) {
                    if (out_dev) {
                        out_dev->bus = static_cast<uint8_t>(bus);
                        out_dev->slot = slot;
                        out_dev->func = func;
                        out_dev->vendor_id = vid;
                        out_dev->device_id = did;
                        out_dev->class_id = cid;
                        out_dev->subclass_id = scid;
                        out_dev->prog_if = static_cast<uint8_t>((rev_class >> 8) & 0xFF);
                        out_dev->header_type = header_type;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

void enable_bus_mastering(const Device& dev) {
    uint16_t cmd = read_config_word(dev.bus, dev.slot, dev.func, 0x04);
    cmd |= (1 << 0) | (1 << 1) | (1 << 2); // IO, Memory, Bus Master
    write_config_word(dev.bus, dev.slot, dev.func, 0x04, cmd);
}

uint32_t get_bar(const Device& dev, uint8_t bar_index) {
    if (bar_index > 5) return 0;
    return read_config_dword(dev.bus, dev.slot, dev.func, 0x10 + bar_index * 4);
}

uint8_t get_interrupt_line(const Device& dev) {
    return read_config_byte(dev.bus, dev.slot, dev.func, 0x3C);
}

const char* get_class_name(uint8_t class_id, uint8_t subclass_id) {
    switch (class_id) {
        case 0x00: return "Unclassified Device";
        case 0x01: {
            switch (subclass_id) {
                case 0x01: return "IDE Interface";
                case 0x06: return "SATA Controller";
                case 0x08: return "NVMe Controller";
                default:   return "Mass Storage Controller";
            }
        }
        case 0x02: {
            switch (subclass_id) {
                case 0x00: return "Ethernet Controller";
                case 0x80: return "Network Controller";
                default:   return "Network Device";
            }
        }
        case 0x03: {
            switch (subclass_id) {
                case 0x00: return "VGA Compatible Controller";
                case 0x02: return "3D Controller";
                default:   return "Display Controller";
            }
        }
        case 0x04: return "Multimedia Controller";
        case 0x05: return "Memory Controller";
        case 0x06: {
            switch (subclass_id) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-to-PCI Bridge";
                default:   return "Bridge Device";
            }
        }
        case 0x07: return "Simple Communication Controller";
        case 0x08: return "Base System Peripheral";
        case 0x09: return "Input Device Controller";
        case 0x0C: {
            switch (subclass_id) {
                case 0x03: return "USB Controller";
                case 0x05: return "SMBus Controller";
                default:   return "Serial Bus Controller";
            }
        }
        default: return "Unknown Device";
    }
}

void list_devices() {
    printk("=== PCI Bus Device Scan ===\n");
    int count = 0;

    for (uint16_t bus = 0; bus < 8; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            uint32_t val0 = read_config_dword(static_cast<uint8_t>(bus), slot, 0, 0);
            uint16_t vendor_id = static_cast<uint16_t>(val0 & 0xFFFF);
            if (vendor_id == 0xFFFF || vendor_id == 0x0000) continue;

            uint8_t header_type = static_cast<uint8_t>((read_config_dword(static_cast<uint8_t>(bus), slot, 0, 0x0C) >> 16) & 0xFF);
            uint8_t num_funcs = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < num_funcs; ++func) {
                uint32_t val = read_config_dword(static_cast<uint8_t>(bus), slot, func, 0);
                uint16_t vid = static_cast<uint16_t>(val & 0xFFFF);
                if (vid == 0xFFFF || vid == 0x0000) continue;

                uint16_t did = static_cast<uint16_t>((val >> 16) & 0xFFFF);
                uint32_t rev_class = read_config_dword(static_cast<uint8_t>(bus), slot, func, 8);
                uint8_t class_id = static_cast<uint8_t>((rev_class >> 24) & 0xFF);
                uint8_t subclass_id = static_cast<uint8_t>((rev_class >> 16) & 0xFF);

                printk(" %s%u:%s%u.%u [%x:%x] %s (Class %x:%x)\n",
                       bus < 10 ? "0" : "", bus,
                       slot < 10 ? "0" : "", slot,
                       func,
                       vid, did,
                       get_class_name(class_id, subclass_id),
                       class_id, subclass_id);
                count++;
            }
        }
    }
    printk("Total PCI devices detected: %d\n", count);
}

} // namespace pci
