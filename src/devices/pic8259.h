// Dual 8259A PIC (0x20/0x21, 0xA0/0xA1) and ELCR (0x4D0/0x4D1).
// On this platform all interrupts are routed through the IOAPIC (declared in
// the MP table), so Linux initialises and then masks the 8259s. This model
// keeps the register state a real 8259 exposes; it does not deliver
// interrupts. If the guest unmasks a line we log it, because that would mean
// the guest fell back to legacy PIC mode, which this VMM does not support.
#pragma once
#include <cstdint>
#include <mutex>

#include "device.h"

namespace Aetheron {

class Pic8259 : public PortDevice {
public:
    uint32_t PortRead(uint16_t port, uint8_t size) override;
    void PortWrite(uint16_t port, uint8_t size, uint32_t value) override;

private:
    struct Chip {
        uint8_t imr = 0xFF, irr = 0, isr = 0;
        uint8_t icw_step = 0;
        bool icw4 = false;
        bool read_isr = false;
        uint8_t vector_base = 0;
    };
    std::mutex mutex_;
    Chip chip_[2];
    uint8_t elcr_[2] = {0, 0};
};

}  // namespace Aetheron
