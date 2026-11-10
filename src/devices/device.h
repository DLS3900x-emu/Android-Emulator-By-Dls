// Interfaces shared by emulated devices.
#pragma once
#include <cstdint>

namespace Aetheron {

class PortDevice {
public:
    virtual ~PortDevice() = default;
    virtual uint32_t PortRead(uint16_t port, uint8_t size) = 0;
    virtual void PortWrite(uint16_t port, uint8_t size, uint32_t value) = 0;
};

class MmioDevice {
public:
    virtual ~MmioDevice() = default;
    virtual uint64_t MmioRead(uint64_t offset, uint8_t size) = 0;
    virtual void MmioWrite(uint64_t offset, uint8_t size, uint64_t value) = 0;
};

// An interrupt line into the IOAPIC (GSI number).
class IrqSink {
public:
    virtual ~IrqSink() = default;
    // Level-style: set the line high or low. Edge-triggered pins deliver on
    // rising edge.
    virtual void SetIrqLevel(uint32_t gsi, bool level) = 0;
    // Convenience for edge-triggered ISA devices: raise then lower.
    void PulseIrq(uint32_t gsi) {
        SetIrqLevel(gsi, true);
        SetIrqLevel(gsi, false);
    }
};

}  // namespace Aetheron
