// virtio-net (virtio spec 5.1) connected to the user-mode NAT. No offloads:
// the guest computes its own checksums and sends plain Ethernet frames.
#pragma once
#include <memory>

#include "../net/user_net.h"
#include "virtio_pci.h"

namespace Aetheron {

class VirtioNet : public VirtioPciDevice {
public:
    VirtioNet(GuestMemory* mem, const uint8_t mac[6]);
    ~VirtioNet() override;
    void StartNetwork();
    // Moves frames from the NAT into guest receive buffers (thread-safe).
    void PumpRx();

protected:
    void OnQueueNotify(uint16_t q) override;

private:
    void PumpRxLocked();
    std::unique_ptr<UserNet> net_;
    UserNet::Frame held_;  // a frame waiting for a receive buffer
    bool have_held_ = false;
};

}  // namespace Aetheron
