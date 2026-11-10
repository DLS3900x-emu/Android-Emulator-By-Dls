// MC146818-compatible CMOS RTC + NVRAM (ports 0x70/0x71).
// Time comes from the host's real UTC clock. If the guest sets the clock, the
// difference is kept as an offset (like a real battery-backed RTC).
#pragma once
#include <cstdint>
#include <mutex>

#include "device.h"

namespace Aetheron {

class CmosRtc : public PortDevice {
public:
    explicit CmosRtc(IrqSink* irq);
    uint32_t PortRead(uint16_t port, uint8_t size) override;
    void PortWrite(uint16_t port, uint8_t size, uint32_t value) override;

    // Stores the memory size fields BIOSes normally put in CMOS.
    void SetMemorySize(uint64_t low_bytes, uint64_t high_bytes);

private:
    int64_t GuestEpochSeconds() const;
    uint8_t ToReg(int v) const;
    int FromReg(uint8_t v) const;
    uint8_t ReadTimeReg(uint8_t index);
    void WriteTimeReg(uint8_t index, uint8_t v);

    std::mutex mutex_;
    IrqSink* irq_;
    uint8_t index_ = 0;
    uint8_t ram_[128] = {};
    int64_t offset_seconds_ = 0;
};

}  // namespace Aetheron
