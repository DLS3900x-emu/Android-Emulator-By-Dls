// User-mode NAT network ("slirp"-style), so the guest gets Internet access
// through ordinary host sockets with no drivers or admin rights.
//
//   guest      10.0.2.15   (assigned by the built-in DHCP server)
//   gateway    10.0.2.2    (also reaches the host's own 127.0.0.1)
//   DNS        10.0.2.3    (answered with the host's resolver)
//
// Supported: ARP, DHCP, DNS (A records; other types get an empty answer),
// outbound TCP and UDP, ICMP echo to the gateway. IPv6 frames are ignored.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "sock.h"

namespace Aetheron {

class UserNet {
public:
    using Frame = std::vector<uint8_t>;
    // Called from the NAT thread (holding no NAT locks) when frames are waiting
    // for the guest. Frames produced synchronously by FromGuest() are not
    // signalled; the caller drains them itself.
    using Wakeup = std::function<void()>;

    UserNet(const uint8_t guest_mac[6], Wakeup wakeup);
    ~UserNet();

    void Start();  // starts the socket polling thread
    void Stop();

    // Ethernet frame sent by the guest.
    void FromGuest(const uint8_t* frame, size_t len);
    // Pops the next frame destined to the guest; false if none.
    bool PopToGuest(Frame* out);
    bool HasToGuest();

    // Runs one polling iteration (used by the thread and by tests).
    void Poll(int timeout_ms);

    static constexpr uint32_t kGuestIp = 0x0A00020F;    // 10.0.2.15
    static constexpr uint32_t kGatewayIp = 0x0A000202;  // 10.0.2.2
    static constexpr uint32_t kDnsIp = 0x0A000203;      // 10.0.2.3

private:
    struct TcpConn;
    struct UdpFlow;
    using TcpKey = std::tuple<uint16_t, uint32_t, uint16_t>;  // guest port, remote ip, remote port

    void PollLocked(int timeout_ms);
    void HandleArp(const uint8_t* p, size_t len);
    void HandleIpv4(const uint8_t* p, size_t len);
    void HandleUdp(uint32_t src, uint32_t dst, const uint8_t* p, size_t len);
    void HandleTcp(uint32_t src, uint32_t dst, const uint8_t* p, size_t len);
    void HandleDhcp(const uint8_t* p, size_t len);
    void HandleDns(uint16_t sport, const uint8_t* p, size_t len);
    void HandleIcmp(uint32_t src, uint32_t dst, const uint8_t* p, size_t len);

    void SendIpv4(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* payload, size_t len,
                  bool broadcast = false);
    void SendUdp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t* data, size_t len,
                 bool broadcast = false);
    void SendTcp(TcpConn& c, uint8_t flags, const uint8_t* data, size_t len, bool syn_options = false);
    void SendTcpRst(uint32_t remote_ip, uint16_t remote_port, uint16_t guest_port, uint32_t seq, uint32_t ack,
                    bool ack_flag);
    void PushTcpData(TcpConn& c);
    void CloseTcp(const TcpKey& key);

    static uint32_t HostAddrFor(uint32_t guest_dst);

    std::mutex mutex_;     // protects all NAT state
    std::mutex out_mutex_; // protects to_guest_ only (leaf lock)
    std::deque<Frame> to_guest_;
    Wakeup wakeup_;
    uint8_t guest_mac_[6];
    uint8_t gw_mac_[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
    std::atomic<uint16_t> ip_id_{1};
    std::atomic<int> dns_inflight_{0};
    std::map<TcpKey, std::unique_ptr<TcpConn>> tcp_;
    std::map<TcpKey, std::unique_ptr<UdpFlow>> udp_;
    std::thread thread_;
    bool running_ = false;
};

}  // namespace Aetheron
