// Intel 8254 Programmable Interval Timer (ports 0x40-0x43) plus the port
// 0x61 "NMI status and control" register (channel 2 gate / OUT2).
// The counters are derived from the host monotonic clock at 1.193182 MHz,
// so the guest can calibrate its TSC and LAPIC timer against real time.
#pragma once
#include <cstdint>
#include <mutex>

#include "device.h"

namespace Aetheron {

class Pit : public PortDevice {
public:
    static constexpr uint64_t kFreqHz = 1193182;

    explicit Pit(IrqSink* irq) : irq_(irq) {}

    uint32_t PortRead(uint16_t port, uint8_t size) override;
    void PortWrite(uint16_t port, uint8_t size, uint32_t value) override;

    // Called by the platform timer thread. Raises IRQ0 when channel 0 fires.
    void Poll(uint64_t now_ns);
    // Absolute host time of the next channel-0 event, or UINT64_MAX.
    uint64_t NextDeadlineNs();

private:
    struct Channel {
        uint8_t mode = 0;
        uint8_t access = 3;  // 1=lobyte 2=hibyte 3=lo/hi
        bool bcd = false;
        uint32_t reload = 0x10000;
        bool loaded = false;  // a count has been written since the control word
        bool gate = true;
        uint64_t start_ns = 0;     // when counting (re)started
        uint32_t frozen = 0;       // count while gate is low (mode 0/4)
        bool write_lsb_next = true;
        uint8_t pending_lsb = 0;
        bool read_lsb_next = true;
        bool latched = false;
        uint16_t latch_value = 0;
        bool status_latched = false;
        uint8_t status = 0;
        uint64_t fired_index = 0;  // channel-0 periods already signalled
    };

    uint64_t Ticks(const Channel& c, uint64_t now_ns) const;
    uint16_t Count(const Channel& c, uint64_t now_ns) const;
    bool Out(const Channel& c, uint64_t now_ns) const;
    void Load(int ch, uint32_t value, uint64_t now_ns);
    void SetGate(int ch, bool gate, uint64_t now_ns);

    std::mutex mutex_;
    IrqSink* irq_;
    Channel ch_[3];
    uint8_t port61_ = 0;  // bits 0..3 writable
};

}  // namespace Aetheron
