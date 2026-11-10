// virtio-vsock (virtio spec 5.10). The guest is CID 3 and the only peer is the
// host, CID 2. Cuttlefish HALs that report to host-side tools (lights, ...)
// connect out over vsock; there is no host application behind those ports,
// so streams are accepted and their data is consumed.
#pragma once
#include <deque>
#include <map>
#include <utility>
#include <vector>

#include "virtio_pci.h"

namespace Aetheron {

class VirtioVsock : public VirtioPciDevice {
public:
    static constexpr uint64_t kHostCid = 2, kGuestCid = 3;

    explicit VirtioVsock(GuestMemory* mem);

protected:
    void OnQueueNotify(uint16_t q) override;
    void OnReset() override;

private:
    struct Conn {
        uint32_t fwd_cnt = 0;       // bytes received from the guest and consumed
        uint32_t last_credit = 0;   // fwd_cnt the guest was last told about
    };
    void HandlePacket(const std::vector<uint8_t>& pkt);
    void Reply(uint32_t host_port, uint32_t guest_port, uint16_t op, uint32_t fwd_cnt);
    void PumpRx();

    std::map<std::pair<uint32_t, uint32_t>, Conn> conns_;  // (guest port, host port)
    std::deque<std::vector<uint8_t>> pending_;             // headers waiting for a receive buffer
};

}  // namespace Aetheron
