// virtio-vsock (virtio spec 5.10). The guest is CID 3 and the only peer is the
// host, CID 2.
//
// Host -> guest streams (HostConnect) are how Windows reaches services inside
// Android, e.g. adbd on vsock port 5555. Guest -> host streams come from
// Cuttlefish HALs that report to host-side tools; nothing listens for those,
// so they are accepted and their data is consumed.
#pragma once
#include <deque>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "virtio_pci.h"

namespace Aetheron {

class VirtioVsock : public VirtioPciDevice {
public:
    static constexpr uint64_t kHostCid = 2, kGuestCid = 3;
    using DataFn = std::function<void(const uint8_t*, size_t)>;
    // true: the guest accepted the stream; false: it was refused or closed.
    using EventFn = std::function<void(bool)>;

    explicit VirtioVsock(GuestMemory* mem);

    // Opens a stream to `guest_port`. Callbacks run with the device lock held
    // (on a vCPU thread) and must not call back into this device. Returns a
    // handle for HostSend/HostClose.
    uint32_t HostConnect(uint32_t guest_port, DataFn on_data, EventFn on_event);
    // Queues bytes for the guest, waiting while the guest is slow to read.
    // Returns false once the stream is gone.
    bool HostSend(uint32_t handle, const uint8_t* data, size_t len);
    void HostClose(uint32_t handle);

protected:
    void OnQueueNotify(uint16_t q) override;
    void OnReset() override;

private:
    struct Conn {  // guest-initiated
        uint32_t fwd_cnt = 0;       // bytes received from the guest and consumed
        uint32_t last_credit = 0;   // fwd_cnt the guest was last told about
    };
    struct HostConn {
        uint32_t guest_port = 0;
        bool established = false;
        bool credit_requested = false;
        DataFn on_data;
        EventFn on_event;
        uint32_t rx_cnt = 0, last_credit = 0;             // guest -> host bytes, and what the guest was told
        uint32_t tx_cnt = 0;                              // host -> guest bytes sent
        uint32_t peer_buf_alloc = 0, peer_fwd_cnt = 0;    // guest's receive window
        std::deque<uint8_t> out;                          // waiting for buffers / credit
    };
    void HandlePacket(const std::vector<uint8_t>& pkt);
    void HandleHostConnPacket(uint32_t host_port, const std::vector<uint8_t>& pkt);
    void Reply(uint32_t host_port, uint32_t guest_port, uint16_t op, uint32_t fwd_cnt);
    void PumpRx();

    std::map<std::pair<uint32_t, uint32_t>, Conn> conns_;  // (guest port, host port)
    std::map<uint32_t, HostConn> host_conns_;              // by host port (= handle)
    uint32_t next_host_port_ = 0x40000000;
    std::deque<std::vector<uint8_t>> pending_;             // control packets waiting for a receive buffer
};

}  // namespace Aetheron
