#include "virtio_vsock.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

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
constexpr size_t kMaxQueued = 256 * 1024;   // per stream, host -> guest
constexpr uint32_t kMaxPayload = 64 * 1024;

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

std::vector<uint8_t> MakeHeader(uint32_t host_port, uint32_t guest_port, uint16_t op, uint32_t len, uint32_t fwd_cnt) {
    std::vector<uint8_t> h(kHdrLen, 0);
    Put<uint64_t>(h, kOffSrcCid, VirtioVsock::kHostCid);
    Put<uint64_t>(h, kOffDstCid, VirtioVsock::kGuestCid);
    Put<uint32_t>(h, kOffSrcPort, host_port);
    Put<uint32_t>(h, kOffDstPort, guest_port);
    Put<uint32_t>(h, kOffLen, len);
    Put<uint16_t>(h, kOffType, kTypeStream);
    Put<uint16_t>(h, kOffOp, op);
    Put<uint32_t>(h, kOffBufAlloc, kBufAlloc);
    Put<uint32_t>(h, kOffFwdCnt, fwd_cnt);
    return h;
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
    std::map<uint32_t, HostConn> gone;
    gone.swap(host_conns_);
    for (auto& e : gone)
        if (e.second.on_event) e.second.on_event(false);
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

uint32_t VirtioVsock::HostConnect(uint32_t guest_port, DataFn on_data, EventFn on_event) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const uint32_t port = next_host_port_++;
    HostConn& hc = host_conns_[port];
    hc.guest_port = guest_port;
    hc.on_data = std::move(on_data);
    hc.on_event = std::move(on_event);
    Reply(port, guest_port, kOpRequest, 0);
    PumpRx();
    return port;
}

bool VirtioVsock::HostSend(uint32_t handle, const uint8_t* data, size_t len) {
    for (;;) {
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto it = host_conns_.find(handle);
            if (it == host_conns_.end()) return false;
            if (it->second.out.size() < kMaxQueued) {
                it->second.out.insert(it->second.out.end(), data, data + len);
                PumpRx();
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // guest is behind; wait outside the lock
    }
}

void VirtioVsock::HostClose(uint32_t handle) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = host_conns_.find(handle);
    if (it == host_conns_.end()) return;
    Reply(handle, it->second.guest_port, kOpRst, it->second.rx_cnt);
    host_conns_.erase(it);
    PumpRx();
}

void VirtioVsock::HandleHostConnPacket(uint32_t host_port, const std::vector<uint8_t>& pkt) {
    auto it = host_conns_.find(host_port);
    HostConn& hc = it->second;
    const uint32_t len = Get<uint32_t>(pkt, kOffLen);
    const uint16_t op = Get<uint16_t>(pkt, kOffOp);
    hc.peer_buf_alloc = Get<uint32_t>(pkt, kOffBufAlloc);
    hc.peer_fwd_cnt = Get<uint32_t>(pkt, kOffFwdCnt);
    hc.credit_requested = false;
    switch (op) {
        case kOpResponse:
            hc.established = true;
            if (hc.on_event) hc.on_event(true);
            break;
        case kOpRw: {
            const size_t n = std::min<size_t>(len, pkt.size() - kHdrLen);
            hc.rx_cnt += uint32_t(n);
            if (n && hc.on_data) hc.on_data(pkt.data() + kHdrLen, n);
            if (hc.rx_cnt - hc.last_credit >= kBufAlloc / 4) {
                hc.last_credit = hc.rx_cnt;
                Reply(host_port, hc.guest_port, kOpCreditUpdate, hc.rx_cnt);
            }
            break;
        }
        case kOpCreditRequest:
            hc.last_credit = hc.rx_cnt;
            Reply(host_port, hc.guest_port, kOpCreditUpdate, hc.rx_cnt);
            break;
        case kOpShutdown:
        case kOpRst: {
            if (op == kOpShutdown) Reply(host_port, hc.guest_port, kOpRst, hc.rx_cnt);
            EventFn ev = std::move(hc.on_event);
            host_conns_.erase(it);
            if (ev) ev(false);
            break;
        }
        default:
            break;  // credit update: the new window is already recorded
    }
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
    auto hit = host_conns_.find(host_port);
    if (hit != host_conns_.end() && hit->second.guest_port == guest_port) {
        HandleHostConnPacket(host_port, pkt);
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
    pending_.push_back(MakeHeader(host_port, guest_port, op, 0, fwd_cnt));
}

void VirtioVsock::PumpRx() {
    if (!QueueReady(0)) return;
    VirtqChain c;
    bool any = false;
    while (!pending_.empty() && PopChain(0, &c)) {
        uint32_t n = ScatterWritable(c, pending_.front().data(), uint32_t(kHdrLen));
        PushUsed(0, c.head, n);
        pending_.pop_front();
        any = true;
    }
    // Stream data, only once every control packet is out (keeps ordering).
    bool buffers_left = pending_.empty();
    for (auto& e : host_conns_) {
        if (!buffers_left) break;
        HostConn& hc = e.second;
        while (hc.established && !hc.out.empty()) {
            const uint32_t credit = hc.peer_buf_alloc - (hc.tx_cnt - hc.peer_fwd_cnt);
            if (credit == 0 || credit > hc.peer_buf_alloc) {
                // Window full: ask once for an update instead of waiting for the guest to volunteer one.
                if (!hc.credit_requested) {
                    hc.credit_requested = true;
                    Reply(e.first, hc.guest_port, kOpCreditRequest, hc.rx_cnt);
                }
                break;
            }
            if (!PopChain(0, &c)) {
                buffers_left = false;
                break;
            }
            uint32_t cap = 0;
            for (auto& b : c.bufs)
                if (b.device_writable) cap += b.len;
            if (cap <= kHdrLen) {
                PushUsed(0, c.head, 0);
                any = true;
                continue;
            }
            const uint32_t n = uint32_t(std::min<size_t>({hc.out.size(), cap - kHdrLen, credit, kMaxPayload}));
            std::vector<uint8_t> pkt = MakeHeader(e.first, hc.guest_port, kOpRw, n, hc.rx_cnt);
            pkt.insert(pkt.end(), hc.out.begin(), hc.out.begin() + n);
            hc.out.erase(hc.out.begin(), hc.out.begin() + n);
            hc.last_credit = hc.rx_cnt;
            hc.tx_cnt += n;
            PushUsed(0, c.head, ScatterWritable(c, pkt.data(), uint32_t(pkt.size())));
            any = true;
        }
    }
    // A credit request queued above still needs a buffer.
    while (buffers_left && !pending_.empty() && PopChain(0, &c)) {
        uint32_t n = ScatterWritable(c, pending_.front().data(), uint32_t(kHdrLen));
        PushUsed(0, c.head, n);
        pending_.pop_front();
        any = true;
    }
    if (any) NotifyQueue(0);
}

}  // namespace Aetheron
