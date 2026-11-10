#include "virtio_net.h"

#include <cstring>

#include "../common/log.h"

namespace Aetheron {

namespace {
constexpr uint64_t kFMac = 1ULL << 5, kFStatus = 1ULL << 16;
constexpr size_t kHdrLen = 12;  // struct virtio_net_hdr with num_buffers (VERSION_1)
}  // namespace

VirtioNet::VirtioNet(GuestMemory* mem, const uint8_t mac[6])
    : VirtioPciDevice(mem, 1, 0x020000, 2, 256, 8) {
    device_features_ |= kFMac | kFStatus;
    std::memcpy(device_config_.data(), mac, 6);
    uint16_t status = 1;  // VIRTIO_NET_S_LINK_UP
    std::memcpy(device_config_.data() + 6, &status, 2);
    net_ = std::make_unique<UserNet>(mac, [this] { PumpRx(); });
}

VirtioNet::~VirtioNet() { net_->Stop(); }

void VirtioNet::StartNetwork() { net_->Start(); }

void VirtioNet::PumpRx() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    PumpRxLocked();
}

void VirtioNet::PumpRxLocked() {
    if (!QueueReady(0)) return;
    bool any = false;
    VirtqChain c;
    for (;;) {
        if (!have_held_) {
            if (!net_->PopToGuest(&held_)) break;
            have_held_ = true;
        }
        if (!PopChain(0, &c)) break;  // no receive buffer: keep the frame for later
        uint8_t hdr[kHdrLen] = {};
        hdr[10] = 1;  // num_buffers = 1
        uint32_t cap = 0;
        for (auto& b : c.bufs)
            if (b.device_writable) cap += b.len;
        uint32_t n = 0;
        if (cap >= kHdrLen + held_.size()) {
            n = ScatterWritable(c, hdr, kHdrLen);
            n += ScatterWritable(c, held_.data(), uint32_t(held_.size()), kHdrLen);
        } else {
            LogOnce("net-rx-small", "virtio-net: receive buffer too small (%u bytes) for a %zu-byte frame", cap,
                    held_.size());
        }
        PushUsed(0, c.head, n);
        have_held_ = false;
        any = true;
    }
    if (any) NotifyQueue(0);
}

void VirtioNet::OnQueueNotify(uint16_t q) {
    if (q == 0) {
        PumpRxLocked();  // guest added receive buffers
        return;
    }
    VirtqChain c;
    bool any = false;
    while (PopChain(1, &c)) {
        std::vector<uint8_t> pkt = GatherReadable(c);
        if (pkt.size() > kHdrLen) net_->FromGuest(pkt.data() + kHdrLen, pkt.size() - kHdrLen);
        PushUsed(1, c.head, 0);
        any = true;
    }
    if (any) NotifyQueue(1);
    PumpRxLocked();  // replies generated synchronously (ARP, DHCP...)
}

}  // namespace Aetheron
