// virtio-console (virtio spec 5.3), single port, no multiport. Appears in the
// guest as /dev/hvcN. Output is written to a host log file; host input can be
// queued with SendToGuest().
#pragma once
#include <cstdio>
#include <deque>
#include <functional>
#include <string>

#include "virtio_pci.h"

namespace Aetheron {

class VirtioConsole : public VirtioPciDevice {
public:
    VirtioConsole(GuestMemory* mem, const std::string& log_path);
    ~VirtioConsole() override;
    void SendToGuest(const std::string& data);
    // Called (device mutex held) with every chunk the guest writes to the port.
    void SetOutputHandler(std::function<void(const uint8_t*, size_t)> fn) { on_output_ = std::move(fn); }

protected:
    void OnQueueNotify(uint16_t q) override;
    void OnReset() override { pending_rx_.clear(); }

private:
    void PumpRx();  // mutex held
    FILE* log_ = nullptr;
    std::function<void(const uint8_t*, size_t)> on_output_;
    std::deque<uint8_t> pending_rx_;
};

}  // namespace Aetheron
