#ifndef KERNEL_PCI_HPP
#define KERNEL_PCI_HPP

#include "types.hpp"

namespace pci {

struct Device {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_id;
    uint8_t subclass_id;
    uint8_t prog_if;
    uint8_t header_type;
};

uint32_t read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t read_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t read_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
void write_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void write_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);

bool find_device(uint16_t vendor_id, uint16_t device_id, Device* out_dev);
bool find_device_by_class(uint8_t class_id, uint8_t subclass_id, Device* out_dev);
void enable_bus_mastering(const Device& dev);
uint32_t get_bar(const Device& dev, uint8_t bar_index);
uint8_t get_interrupt_line(const Device& dev);

const char* get_class_name(uint8_t class_id, uint8_t subclass_id);
void list_devices();

} // namespace pci

#endif // KERNEL_PCI_HPP
