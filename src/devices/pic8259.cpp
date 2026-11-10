#include "pic8259.h"

#include "../common/log.h"

namespace Aetheron {

uint32_t Pic8259::PortRead(uint16_t port, uint8_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (port == 0x4D0 || port == 0x4D1) return elcr_[port - 0x4D0];
    Chip& c = chip_[(port & 0x80) ? 1 : 0];
    if (port & 1) return c.imr;
    return c.read_isr ? c.isr : c.irr;
}

void Pic8259::PortWrite(uint16_t port, uint8_t size, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t v = uint8_t(value);
    if (port == 0x4D0 || port == 0x4D1) {
        elcr_[port - 0x4D0] = v;
        return;
    }
    int idx = (port & 0x80) ? 1 : 0;
    Chip& c = chip_[idx];
    if (!(port & 1)) {
        if (v & 0x10) {  // ICW1
            c.icw_step = 1;
            c.icw4 = v & 1;
            c.imr = 0;
            c.isr = c.irr = 0;
        } else if ((v & 0x18) == 0x08) {  // OCW3
            if (v & 0x02) c.read_isr = v & 0x01;
        }
        // OCW2 (EOI): no in-service bits are ever set by this model.
        return;
    }
    switch (c.icw_step) {
        case 1:
            c.vector_base = v & 0xF8;
            c.icw_step = 2;
            return;
        case 2:
            c.icw_step = c.icw4 ? 3 : 0;
            return;
        case 3:
            c.icw_step = 0;
            return;
        default: {
            uint8_t unmasked = uint8_t(~v);
            if (idx == 0) unmasked &= ~0x04;  // cascade line is not a device
            c.imr = v;
            if (unmasked)
                LogOnce("pic-unmask-" + std::to_string(idx) + "-" + std::to_string(unmasked),
                        "PIC%d: guest unmasked lines 0x%02x. Legacy 8259 delivery is not implemented on this "
                        "platform (IOAPIC only).", idx, unmasked);
        }
    }
}

}  // namespace Aetheron
