// virtio-gpu 2D device (virtio spec 5.7), one scanout. Pixels flushed by the
// guest are converted to BGRA and handed to the host display.
#pragma once
#include <atomic>
#include <map>
#include <mutex>

#include "virtio_pci.h"

namespace Aetheron {

// Host-side copy of the guest's visible scanout, read by the renderer.
struct ScanoutBuffer {
    std::mutex mutex;
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> bgra;  // width*height
    bool enabled = false;
    std::atomic<uint64_t> frame{0};
};

class VirtioGpu : public VirtioPciDevice {
public:
    VirtioGpu(GuestMemory* mem, ScanoutBuffer* scanout, uint32_t width, uint32_t height);

protected:
    void OnQueueNotify(uint16_t q) override;
    void WriteDeviceConfig(uint32_t offset, uint8_t size, uint32_t value) override;
    void OnReset() override;

private:
    struct Resource {
        uint32_t format = 0, width = 0, height = 0;
        std::vector<uint8_t> pixels;  // width*height*4, guest format
        std::vector<std::pair<uint64_t, uint32_t>> backing;
    };
    void ProcessControl(const VirtqChain& c, uint32_t* written);
    bool CopyFromBacking(const Resource& r, uint64_t offset, void* dst, uint32_t len);
    void FlushToScanout(uint32_t res_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    ScanoutBuffer* scanout_;
    uint32_t width_, height_;
    std::map<uint32_t, Resource> resources_;
    uint32_t scanout_resource_ = 0;
};

}  // namespace Aetheron
