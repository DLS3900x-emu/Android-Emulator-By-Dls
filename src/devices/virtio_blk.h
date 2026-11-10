// virtio-blk device (virtio spec 5.2) backed by a BlockBackend.
#pragma once
#include <memory>

#include "virtio_pci.h"

namespace Aetheron {

class BlockBackend {
public:
    virtual ~BlockBackend() = default;
    virtual uint64_t SizeBytes() const = 0;
    virtual bool Read(uint64_t offset, void* buf, uint32_t len) = 0;
    virtual bool Write(uint64_t offset, const void* buf, uint32_t len) = 0;
    virtual bool Flush() = 0;
    virtual bool ReadOnly() const { return false; }
};

class VirtioBlk : public VirtioPciDevice {
public:
    VirtioBlk(GuestMemory* mem, std::unique_ptr<BlockBackend> backend, const char* serial);

protected:
    void OnQueueNotify(uint16_t q) override;

private:
    uint8_t HandleRequest(const VirtqChain& c, uint32_t* written);
    std::unique_ptr<BlockBackend> backend_;
    char serial_[20] = {};
    std::vector<uint8_t> bounce_;
};

}  // namespace Aetheron
