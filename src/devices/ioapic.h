// 82093AA-compatible I/O APIC (version 0x20, 24 pins) at 0xFEC00000.
// Interrupts are delivered to the in-hypervisor local APIC via a callback
// (WHvRequestInterrupt). Level-triggered pins implement Remote IRR and are
// re-delivered after EOI if the line is still asserted.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>

#include "device.h"

namespace Aetheron {

struct ApicMessage {
    uint8_t vector;
    uint8_t delivery_mode;  // 0 fixed, 1 lowest-prio, 4 NMI, 5 INIT, 7 ExtINT
    bool logical_dest;
    bool level_triggered;
    uint8_t destination;
};

class IoApic : public MmioDevice, public IrqSink {
public:
    static constexpr uint64_t kBase = 0xFEC00000ULL;
    static constexpr uint64_t kSize = 0x1000;
    static constexpr int kPins = 24;

    using Deliver = std::function<void(const ApicMessage&)>;

    IoApic(uint8_t id, Deliver deliver);

    uint64_t MmioRead(uint64_t offset, uint8_t size) override;
    void MmioWrite(uint64_t offset, uint8_t size, uint64_t value) override;

    void SetIrqLevel(uint32_t gsi, bool level) override;
    // Called when the local APIC signals EOI for `vector`.
    void OnEoi(uint8_t vector);

    uint8_t id() const { return id_; }

private:
    uint32_t ReadReg(uint8_t index);
    void WriteReg(uint8_t index, uint32_t value);
    void ServicePin(int pin);  // requires mutex held

    std::mutex mutex_;
    Deliver deliver_;
    uint8_t id_;
    uint8_t regsel_ = 0;
    uint64_t redir_[kPins];
    bool line_[kPins] = {};
};

}  // namespace Aetheron
