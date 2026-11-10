// Virtio 1.x PCI transport ("modern", virtio spec v1.2 section 4.1) with split
// virtqueues and INTx interrupts. Device-specific behaviour lives in
// subclasses (block, console, gpu, input).
//
// BAR0 layout (16 KiB):
//   0x0000 common configuration
//   0x1000 ISR status
//   0x2000 device-specific configuration
//   0x3000 notifications (queue N at 0x3000 + N*4)
#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

#include "../vmm/guest_memory.h"
#include "pci.h"

namespace Aetheron {

struct VirtqBuffer {
    uint64_t gpa;
    uint32_t len;
    bool device_writable;
};

struct VirtqChain {
    uint16_t head = 0;
    std::vector<VirtqBuffer> bufs;
};

class VirtioPciDevice : public PciDevice {
public:
    static constexpr uint64_t kFeatureVersion1 = 1ULL << 32;

    VirtioPciDevice(GuestMemory* mem, uint16_t virtio_id, uint32_t pci_class, uint16_t num_queues,
                    uint16_t queue_size, uint32_t device_config_size);

    uint32_t Bar0Size() const override { return 0x4000; }
    uint64_t BarRead(uint64_t offset, uint8_t size) override;
    void BarWrite(uint64_t offset, uint8_t size, uint64_t value) override;

    bool driver_ok() const { return (status_ & 0x04) != 0; }

protected:
    // Called with mutex_ held when the driver notifies queue `q`.
    virtual void OnQueueNotify(uint16_t q) = 0;
    // Device config access; default reads from device_config_ and ignores writes.
    virtual uint32_t ReadDeviceConfig(uint32_t offset, uint8_t size);
    virtual void WriteDeviceConfig(uint32_t offset, uint8_t size, uint32_t value) {}
    virtual void OnReset() {}
    virtual void OnDriverOk() {}

    // Virtqueue helpers (mutex_ held).
    bool QueueReady(uint16_t q) const;
    bool PopChain(uint16_t q, VirtqChain* chain);
    void PushUsed(uint16_t q, uint16_t head, uint32_t written);
    void NotifyQueue(uint16_t q);  // raise used-buffer interrupt unless suppressed
    void NotifyConfigChange();

    // Copies from a descriptor list (only device-readable or writable parts).
    std::vector<uint8_t> GatherReadable(const VirtqChain& c) const;
    uint32_t ScatterWritable(const VirtqChain& c, const void* data, uint32_t len, uint32_t skip = 0) const;

    GuestMemory* mem_;
    std::recursive_mutex mutex_;
    uint64_t device_features_ = kFeatureVersion1;
    std::vector<uint8_t> device_config_;
    uint8_t config_generation_ = 0;

private:
    struct Queue {
        uint16_t size = 0;
        uint16_t max_size = 0;
        uint16_t msix_vector = 0xFFFF;
        bool enabled = false;
        uint64_t desc = 0, driver = 0, device = 0;
        uint16_t last_avail = 0;
        uint16_t used_idx = 0;
    };

    uint32_t CommonRead(uint32_t off, uint8_t size);
    void CommonWrite(uint32_t off, uint8_t size, uint32_t value);
    void Reset();
    void AddCapability(uint8_t& next_ptr, uint8_t type, uint32_t offset, uint32_t length, uint32_t extra);

    std::vector<Queue> queues_;
    uint32_t device_feature_select_ = 0;
    uint32_t driver_feature_select_ = 0;
    uint64_t driver_features_ = 0;
    uint16_t queue_select_ = 0;
    uint16_t config_msix_ = 0xFFFF;
    uint8_t status_ = 0;
    uint8_t isr_ = 0;
};

}  // namespace Aetheron
