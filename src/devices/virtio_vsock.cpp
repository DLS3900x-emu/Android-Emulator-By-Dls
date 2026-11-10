#include "virtio_vsock.h"

#include <cstring>
#include <string>

#include "../common/log.h"

namespace Aetheron {

namespace {
// struct virtio_vsock_hdr, 44 bytes, little endian.
constexpr size_t kHdrLen = 44;
constexpr size_t kOffSrcCid = 0, kOffDstCid = 8, kOffSrcPort = 16, kOffDstPort = 20, kOffLen = 24, kOffType = 28,
                 kOffOp = 30, kOffBufAlloc = 36, kOffFwdCnt = 40;
constexpr uint16_t kTypeStream = 1;
constexpr uint16_t kOpRequest = 1, kOpResponse = 2, kOpRst = 3, kOpShutdown = 4, kOpRw = 5, kOpCreditUpdate = 6,
                   kOpCreditRequest = 7;
constexpr uint32_t kBufAlloc = 256 * 1024;
constexpr size_t kMaxPending = 1024;

template <typename T>
T Get(const std::vector<uint8_t>& p, size_t off) {
    T v;
    std::memcpy(&v, p.data() + off, sizeof(T));
    return v;
}
template <typename T>
void Put(std::vector<uint8_t>& p, size_t off, T v) {
    std::memcpy(p.data() + off, &v, sizeof(T));
}
}  // namespace

// Queues: 0 = rx (device to guest), 1 = tx, 2 = event.
VirtioVsock::VirtioVsock(GuestMemory* mem) : VirtioPciDevice(mem, 19, 0x078000, 3, 128, 8) {
    const uint64_t cid = kGuestCid;
    std::memcpy(device_config_.data(), &cid, 8);
}

void VirtioVsock::OnReset() {
    conns_.clear();
    pending_.clear();
}

void VirtioVsock::OnQueueNotify(uint16_t q) {
    if (q == 1) {
        VirtqChain c;
        bool any = false;
        while (PopChain(1, &c)) {
            std::vector<uint8_t> pkt = GatherReadable(c);
            if (pkt.size() >= kHdrLen) HandlePacket(pkt);
            PushUsed(1, c.head, 0);
            any = true;
        }
        if (any) NotifyQueue(1);
    }
    if (q != 2) PumpRx();
}

void VirtioVsock::HandlePacket(const std::vector<uint8_t>& pkt) {
    const uint64_t dst_cid = Get<uint64_t>(pkt, kOffDstCid);
    const uint32_t guest_port = Get<uint32_t>(pkt, kOffSrcPort), host_port = Get<uint32_t>(pkt, kOffDstPort);
    const uint32_t len = Get<uint32_t>(pkt, kOffLen);
    const uint16_t type = Get<uint16_t>(pkt, kOffType), op = Get<uint16_t>(pkt, kOffOp);
    const auto key = std::make_pair(guest_port, host_port);

    if (dst_cid != kHostCid || type != kTypeStream) {
        if (dst_cid != kHostCid)
            LogOnce("vsock-cid-" + std::to_string(dst_cid),
                    "vsock: guest connected to CID %llu (only the host, CID 2, exists)", (unsigned long long)dst_cid);
        if (op != kOpRst) Reply(host_port, guest_port, kOpRst, 0);
        return;
    }
    auto it = conns_.find(key);
    switch (op) {
        case kOpRequest:
            LogOnce("vsock-port-" + std::to_string(host_port),
                    "vsock: guest opened a stream to host port %u (accepted; data is consumed, no host app for it)",
                    host_port);
            conns_[key] = Conn();
            Reply(host_port, guest_port, kOpResponse, 0);
            break;
        case kOpRw:
            if (it == conns_.end()) {
                Reply(host_port, guest_port, kOpRst, 0);
                break;
            }
            it->second.fwd_cnt += len;
            // Tell the sender before its view of our free space runs low.
            if (it->second.fwd_cnt - it->second.last_credit >= kBufAlloc / 4) {
                it->second.last_credit = it->second.fwd_cnt;
                Reply(host_port, guest_port, kOpCreditUpdate, it->second.fwd_cnt);
            }
            break;
        case kOpCreditRequest:
            if (it == conns_.end()) {
                Reply(host_port, guest_port, kOpRst, 0);
                break;
            }
            it->second.last_credit = it->second.fwd_cnt;
            Reply(host_port, guest_port, kOpCreditUpdate, it->second.fwd_cnt);
            break;
        case kOpShutdown:
            // The peer answers a shutdown with RST to finish the close.
            Reply(host_port, guest_port, kOpRst, it == conns_.end() ? 0 : it->second.fwd_cnt);
            if (it != conns_.end()) conns_.erase(it);
            break;
        case kOpRst:
            if (it != conns_.end()) conns_.erase(it);
            break;
        case kOpResponse:
        case kOpCreditUpdate:
            break;
        default:
            LogOnce("vsock-op-" + std::to_string(op), "vsock: unknown op %u", op);
            Reply(host_port, guest_port, kOpRst, 0);
            break;
    }
}

void VirtioVsock::Reply(uint32_t host_port, uint32_t guest_port, uint16_t op, uint32_t fwd_cnt) {
    if (pending_.size() >= kMaxPending) return;  // guest stopped reading; nothing useful to add
    std::vector<uint8_t> h(kHdrLen, 0);
    Put<uint64_t>(h, kOffSrcCid, kHostCid);
    Put<uint64_t>(h, kOffDstCid, kGuestCid);
    Put<uint32_t>(h, kOffSrcPort, host_port);
    Put<uint32_t>(h, kOffDstPort, guest_port);
    Put<uint16_t>(h, kOffType, kTypeStream);
    Put<uint16_t>(h, kOffOp, op);
    Put<uint32_t>(h, kOffBufAlloc, kBufAlloc);
    Put<uint32_t>(h, kOffFwdCnt, fwd_cnt);
    pending_.push_back(std::move(h));
}

void VirtioVsock::PumpRx() {
    if (pending_.empty() || !QueueReady(0)) return;
    VirtqChain c;
    bool any = false;
    while (!pending_.empty() && PopChain(0, &c)) {
        uint32_t n = ScatterWritable(c, pending_.front().data(), uint32_t(kHdrLen));
        PushUsed(0, c.head, n);
        pending_.pop_front();
        any = true;
    }
    if (any) NotifyQueue(0);
}

}  // namespace Aetheron
