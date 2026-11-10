// PCI bus 0 with type-1 configuration access (ports 0xCF8/0xCFC) and
// memory BAR decoding. Each device's INTA# is wired to an IOAPIC pin; the MP
// table tells the guest the same routing.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "device.h"

namespace Aetheron {

class PciDevice {
public:
    virtual ~PciDevice() = default;

    // 256-byte type-0 configuration space.
    uint8_t config[256] = {};

    // Size of BAR0 (32-bit memory BAR). Must be a power of two >= 4 KiB.
    virtual uint32_t Bar0Size() const = 0;
    virtual uint64_t BarRead(uint64_t offset, uint8_t size) = 0;
    virtual void BarWrite(uint64_t offset, uint8_t size, uint64_t value) = 0;

    void SetIrq(IrqSink* sink, uint32_t gsi) {
        irq_ = sink;
        gsi_ = gsi;
    }
    uint32_t gsi() const { return gsi_; }

    uint32_t ConfigRead(uint32_t reg, uint8_t size);
    void ConfigWrite(uint32_t reg, uint8_t size, uint32_t value);
    uint32_t bar0_address() const;
    bool memory_enabled() const { return config[4] & 0x02; }

protected:
    void SetIntx(bool level);
    IrqSink* irq_ = nullptr;
    uint32_t gsi_ = 0;
    bool bar0_sizing_ = false;
};

class PciBus : public PortDevice, public MmioDevice {
    // Per-device INTx source. PCI INTx lines are shared (wired-OR): the IOAPIC
    // pin stays asserted while any device on it asserts.
    class IntxSource : public IrqSink {
    public:
        IntxSource(PciBus* bus, uint8_t slot) : bus_(bus), slot_(slot) {}
        void SetIrqLevel(uint32_t gsi, bool level) override { bus_->SetDeviceIntx(slot_, gsi, level); }

    private:
        PciBus* bus_;
        uint8_t slot_;
    };

public:
    // Default BAR placement inside the 32-bit MMIO hole.
    static constexpr uint64_t kMmioBase = 0xC0000000ULL;
    static constexpr uint64_t kMmioLimit = 0xFEC00000ULL;

    explicit PciBus(IrqSink* irq) : irq_(irq) {}

    // Adds a device at `slot`, wires INTA# to `gsi`, assigns a default BAR.
    void AddDevice(uint8_t slot, PciDevice* dev, uint32_t gsi);
    const std::map<uint8_t, PciDevice*>& devices() const { return devices_; }

    uint32_t PortRead(uint16_t port, uint8_t size) override;
    void PortWrite(uint16_t port, uint8_t size, uint32_t value) override;

    // MMIO in [kMmioBase, kMmioLimit): dispatched to whichever BAR decodes it.
    // Returns false if no BAR claims the address.
    bool HandleMmio(uint64_t gpa, bool write, uint8_t size, uint64_t* value);
    uint64_t MmioRead(uint64_t, uint8_t) override { return 0; }
    void MmioWrite(uint64_t, uint8_t, uint64_t) override {}

private:
    void SetDeviceIntx(uint8_t slot, uint32_t gsi, bool level);

    IrqSink* irq_;
    std::mutex mutex_;
    std::mutex intx_mutex_;
    std::map<uint32_t, uint32_t> asserted_;  // gsi -> bitmask of asserting slots
    std::vector<std::unique_ptr<IntxSource>> sources_;
    uint32_t address_ = 0;
    uint8_t host_bridge_[256] = {};
    std::map<uint8_t, PciDevice*> devices_;
    uint64_t next_bar_ = kMmioBase;
    bool host_bridge_init_ = false;
};

}  // namespace Aetheron
