#ifndef KERNEL_PIC_HPP
#define KERNEL_PIC_HPP

#include "types.hpp"

namespace pic {

void init();
void send_eoi(uint8_t irq);
void mask(uint8_t irq);
void unmask(uint8_t irq);
void disable();

} // namespace pic

#endif // KERNEL_PIC_HPP
