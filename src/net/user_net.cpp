#include "sock.h"  // first: sets FD_SETSIZE before winsock2.h
#include "user_net.h"

#include <chrono>
#include <cstring>

#include "../common/log.h"

namespace Aetheron {

namespace {

uint64_t NowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
void Put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
uint32_t SumWords(const uint8_t* p, size_t n, uint32_t sum = 0) {
    for (size_t i = 0; i + 1 < n; i += 2) sum += uint32_t(p[i] << 8 | p[i + 1]);
    if (n & 1) sum += uint32_t(p[n - 1] << 8);
    return sum;
}
uint16_t Fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return uint16_t(~sum);
}
uint16_t L4Checksum(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* seg, size_t len) {
    uint8_t ph[12];
    Put32(ph, src);
    Put32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = proto;
    Put16(ph + 10, uint16_t(len));
    return Fold(SumWords(seg, len, SumWords(ph, 12)));
}
bool SeqLe(uint32_t a, uint32_t b) { return int32_t(a - b) <= 0; }
std::string IpStr(uint32_t ip) {
    char b[20];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

constexpr uint8_t kFin = 0x01, kSyn = 0x02, kRst = 0x04, kPsh = 0x08, kAck = 0x10;
constexpr size_t kMss = 1460;
constexpr size_t kMaxBuffered = 256 * 1024;  // host->guest bytes buffered per connection
constexpr uint16_t kOurWindow = 65535;

}  // namespace

struct UserNet::TcpConn {
    TcpKey key;
    sock_t s = AETH_BAD_SOCK;
    bool connected = false;
    uint32_t remote_ip = 0;  // as seen by the guest
    uint16_t remote_port = 0, guest_port = 0;
    uint32_t snd_una = 0, snd_nxt = 0;  // our sequence space
    uint32_t rcv_nxt = 0;               // next byte expected from the guest
    uint32_t guest_win = 65535;
    size_t guest_mss = 536;
    std::vector<uint8_t> buf;  // bytes from snd_una not yet acknowledged by the guest
    bool host_eof = false, fin_sent = false, fin_acked = false, guest_fin = false;
    uint64_t last_progress = 0;
};

struct UserNet::UdpFlow {
    sock_t s = AETH_BAD_SOCK;
    uint32_t remote_ip = 0;
    uint16_t remote_port = 0, guest_port = 0;
    uint64_t last_used = 0;
};

UserNet::UserNet(const uint8_t guest_mac[6], Wakeup wakeup) : wakeup_(std::move(wakeup)) {
    std::memcpy(guest_mac_, guest_mac, 6);
    SockStartup();
}

UserNet::~UserNet() {
    Stop();
    for (auto& kv : tcp_) SockClose(kv.second->s);
    for (auto& kv : udp_) SockClose(kv.second->s);
}

void UserNet::Start() {
    running_ = true;
    thread_ = std::thread([this] {
        while (running_) Poll(10);
    });
}

void UserNet::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    while (dns_inflight_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

uint32_t UserNet::HostAddrFor(uint32_t guest_dst) {
    return guest_dst == kGatewayIp ? 0x7F000001 /* 127.0.0.1 */ : guest_dst;
}

bool UserNet::PopToGuest(Frame* out) {
    std::lock_guard<std::mutex> lock(out_mutex_);
    if (to_guest_.empty()) return false;
    *out = std::move(to_guest_.front());
    to_guest_.pop_front();
    return true;
}

bool UserNet::HasToGuest() {
    std::lock_guard<std::mutex> lock(out_mutex_);
    return !to_guest_.empty();
}

// ---------------------------------------------------------------- output

void UserNet::SendIpv4(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* payload, size_t len,
                       bool broadcast) {
    Frame f(14 + 20 + len);
    uint8_t* e = f.data();
    if (broadcast)
        std::memset(e, 0xFF, 6);
    else
        std::memcpy(e, guest_mac_, 6);
    std::memcpy(e + 6, gw_mac_, 6);
    Put16(e + 12, 0x0800);
    uint8_t* ip = e + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    Put16(ip + 2, uint16_t(20 + len));
    Put16(ip + 4, ip_id_++);
    Put16(ip + 6, 0x4000);  // don't fragment
    ip[8] = 64;
    ip[9] = proto;
    Put16(ip + 10, 0);
    Put32(ip + 12, src);
    Put32(ip + 16, dst);
    Put16(ip + 10, Fold(SumWords(ip, 20)));
    std::memcpy(ip + 20, payload, len);
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        if (to_guest_.size() > 4096) to_guest_.pop_front();  // guest stopped reading
        to_guest_.push_back(std::move(f));
    }
}

void UserNet::SendUdp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t* data, size_t len,
                      bool broadcast) {
    std::vector<uint8_t> u(8 + len);
    Put16(&u[0], sport);
    Put16(&u[2], dport);
    Put16(&u[4], uint16_t(8 + len));
    Put16(&u[6], 0);
    if (len) std::memcpy(&u[8], data, len);
    uint16_t c = L4Checksum(src, dst, 17, u.data(), u.size());
    Put16(&u[6], c ? c : 0xFFFF);
    SendIpv4(src, dst, 17, u.data(), u.size(), broadcast);
}

void UserNet::SendTcp(TcpConn& c, uint8_t flags, const uint8_t* data, size_t len, bool syn_options) {
    size_t hl = syn_options ? 24 : 20;
    std::vector<uint8_t> t(hl + len);
    Put16(&t[0], c.remote_port);
    Put16(&t[2], c.guest_port);
    Put32(&t[4], c.snd_nxt);
    Put32(&t[8], c.rcv_nxt);
    t[12] = uint8_t((hl / 4) << 4);
    t[13] = flags;
    Put16(&t[14], kOurWindow);
    if (syn_options) {
        t[20] = 2;  // MSS
        t[21] = 4;
        Put16(&t[22], uint16_t(kMss));
    }
    if (len) std::memcpy(&t[hl], data, len);
    Put16(&t[16], L4Checksum(c.remote_ip, kGuestIp, 6, t.data(), t.size()));
    SendIpv4(c.remote_ip, kGuestIp, 6, t.data(), t.size());
}

void UserNet::SendTcpRst(uint32_t remote_ip, uint16_t remote_port, uint16_t guest_port, uint32_t seq, uint32_t ack,
                         bool ack_flag) {
    TcpConn tmp;
    tmp.remote_ip = remote_ip;
    tmp.remote_port = remote_port;
    tmp.guest_port = guest_port;
    tmp.snd_nxt = seq;
    tmp.rcv_nxt = ack;
    SendTcp(tmp, uint8_t(kRst | (ack_flag ? kAck : 0)), nullptr, 0);
}

// Sends as much buffered host data as the guest's window allows, then FIN.
void UserNet::PushTcpData(TcpConn& c) {
    size_t mss = c.guest_mss < kMss ? c.guest_mss : kMss;
    size_t sent = c.snd_nxt - c.snd_una;
    if (sent > c.buf.size()) sent = c.buf.size();  // FIN already counted
    while (sent < c.buf.size() && sent < c.guest_win) {
        size_t n = c.buf.size() - sent;
        if (n > mss) n = mss;
        if (n > c.guest_win - sent) n = c.guest_win - sent;
        c.snd_nxt = c.snd_una + uint32_t(sent);
        SendTcp(c, kAck | kPsh, c.buf.data() + sent, n);
        sent += n;
        c.snd_nxt = c.snd_una + uint32_t(sent);
    }
    if (c.host_eof && !c.fin_sent && sent == c.buf.size()) {
        c.snd_nxt = c.snd_una + uint32_t(sent);
        SendTcp(c, kFin | kAck, nullptr, 0);
        c.snd_nxt++;
        c.fin_sent = true;
    }
}

void UserNet::CloseTcp(const TcpKey& key) {
    auto it = tcp_.find(key);
    if (it == tcp_.end()) return;
    SockClose(it->second->s);
    tcp_.erase(it);
}

// ---------------------------------------------------------------- input

void UserNet::FromGuest(const uint8_t* f, size_t len) {
    if (len < 14) return;
    std::lock_guard<std::mutex> lock(mutex_);
    uint16_t type = Be16(f + 12);
    if (type == 0x0806)
        HandleArp(f + 14, len - 14);
    else if (type == 0x0800)
        HandleIpv4(f + 14, len - 14);
    else if (type == 0x86DD)
        LogOnce("net-ipv6", "net: guest sent IPv6 traffic; IPv6 is not supported by the user-mode NAT");
}

void UserNet::HandleArp(const uint8_t* p, size_t len) {
    if (len < 28 || Be16(p) != 1 || Be16(p + 2) != 0x0800 || Be16(p + 6) != 1) return;  // only requests
    uint32_t target = Be32(p + 24);
    if ((target & 0xFFFFFF00) != 0x0A000200 || target == kGuestIp) return;
    Frame f(42);
    std::memcpy(&f[0], p + 8, 6);  // to the sender
    std::memcpy(&f[6], gw_mac_, 6);
    Put16(&f[12], 0x0806);
    uint8_t* a = &f[14];
    Put16(a, 1);
    Put16(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    Put16(a + 6, 2);  // reply
    std::memcpy(a + 8, gw_mac_, 6);
    Put32(a + 14, target);
    std::memcpy(a + 18, p + 8, 10);  // sender MAC + IP become target
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        to_guest_.push_back(std::move(f));
    }
}

void UserNet::HandleIpv4(const uint8_t* p, size_t len) {
    if (len < 20 || (p[0] >> 4) != 4) return;
    size_t ihl = size_t(p[0] & 0x0F) * 4;
    size_t total = Be16(p + 2);
    if (ihl < 20 || total < ihl || total > len) return;
    if ((Be16(p + 6) & 0x3FFF) != 0) {
        LogOnce("net-frag", "net: fragmented IPv4 packet from guest dropped");
        return;
    }
    uint32_t src = Be32(p + 12), dst = Be32(p + 16);
    const uint8_t* l4 = p + ihl;
    size_t l4len = total - ihl;
    switch (p[9]) {
        case 1: HandleIcmp(src, dst, l4, l4len); break;
        case 6: HandleTcp(src, dst, l4, l4len); break;
        case 17: HandleUdp(src, dst, l4, l4len); break;
        default: LogOnce("net-proto-" + std::to_string(p[9]), "net: IP protocol %u not supported", p[9]);
    }
}

void UserNet::HandleIcmp(uint32_t src, uint32_t dst, const uint8_t* p, size_t len) {
    if (len < 8 || p[0] != 8) return;  // echo request only
    if (dst != kGatewayIp && dst != kDnsIp) {
        LogOnce("net-icmp", "net: ICMP echo to %s not forwarded (needs raw sockets); only 10.0.2.2/3 answer ping",
                IpStr(dst).c_str());
        return;
    }
    std::vector<uint8_t> r(p, p + len);
    r[0] = 0;  // echo reply
    Put16(&r[2], 0);
    Put16(&r[2], Fold(SumWords(r.data(), r.size())));
    SendIpv4(dst, src, 1, r.data(), r.size());
}

void UserNet::HandleUdp(uint32_t src, uint32_t dst, const uint8_t* p, size_t len) {
    if (len < 8) return;
    uint16_t sport = Be16(p), dport = Be16(p + 2);
    size_t ulen = Be16(p + 4);
    if (ulen < 8 || ulen > len) return;
    const uint8_t* data = p + 8;
    size_t dlen = ulen - 8;
    if (dport == 67) {
        HandleDhcp(data, dlen);
        return;
    }
    if (dst == kDnsIp && dport == 53) {
        HandleDns(sport, data, dlen);
        return;
    }
    if (dst == 0xFFFFFFFF || (dst & 0xF0000000) == 0xE0000000) return;  // broadcast / multicast stay local

    TcpKey key{sport, dst, dport};
    auto it = udp_.find(key);
    if (it == udp_.end()) {
        auto fl = std::make_unique<UdpFlow>();
        fl->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fl->s == AETH_BAD_SOCK) return;
        SockNonBlock(fl->s);
        fl->remote_ip = dst;
        fl->remote_port = dport;
        fl->guest_port = sport;
        it = udp_.emplace(key, std::move(fl)).first;
    }
    UdpFlow& fl = *it->second;
    fl.last_used = NowMs();
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(dport);
    to.sin_addr.s_addr = htonl(HostAddrFor(dst));
    sendto(fl.s, reinterpret_cast<const char*>(data), int(dlen), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

void UserNet::HandleDhcp(const uint8_t* p, size_t len) {
    if (len < 240 || p[0] != 1 || Be32(p + 236) != 0x63825363) return;
    uint8_t type = 0;
    for (size_t i = 240; i + 1 < len;) {
        uint8_t opt = p[i];
        if (opt == 0) {
            ++i;
            continue;
        }
        if (opt == 255) break;
        uint8_t olen = p[i + 1];
        if (opt == 53 && olen >= 1 && i + 2 < len) type = p[i + 2];
        i += 2 + olen;
    }
    uint8_t reply;
    if (type == 1) reply = 2;       // DISCOVER -> OFFER
    else if (type == 3) reply = 5;  // REQUEST  -> ACK
    else return;                    // RELEASE / DECLINE / INFORM: nothing to do
    std::vector<uint8_t> r(300, 0);
    r[0] = 2;
    r[1] = 1;
    r[2] = 6;
    std::memcpy(&r[4], p + 4, 4);    // xid
    std::memcpy(&r[10], p + 10, 2);  // flags
    Put32(&r[16], kGuestIp);         // yiaddr
    Put32(&r[20], kGatewayIp);       // siaddr
    std::memcpy(&r[28], p + 28, 16); // chaddr
    Put32(&r[236], 0x63825363);
    size_t o = 240;
    auto opt = [&](uint8_t code, const std::vector<uint8_t>& v) {
        r[o++] = code;
        r[o++] = uint8_t(v.size());
        for (uint8_t b : v) r[o++] = b;
    };
    auto ip = [](uint32_t a) { return std::vector<uint8_t>{uint8_t(a >> 24), uint8_t(a >> 16), uint8_t(a >> 8), uint8_t(a)}; };
    opt(53, {reply});
    opt(54, ip(kGatewayIp));
    opt(51, {0x00, 0x01, 0x51, 0x80});  // lease 86400 s
    opt(1, ip(0xFFFFFF00));
    opt(3, ip(kGatewayIp));
    opt(6, ip(kDnsIp));
    opt(28, ip(0x0A0002FF));
    opt(26, {0x05, 0xDC});  // MTU 1500
    r[o++] = 255;
    Log("net: DHCP %s -> guest 10.0.2.15", reply == 2 ? "OFFER" : "ACK");
    SendUdp(kGatewayIp, 67, 0xFFFFFFFF, 68, r.data(), r.size(), true);
}

void UserNet::HandleDns(uint16_t sport, const uint8_t* p, size_t len) {
    if (len < 12 || Be16(p + 4) != 1) return;  // exactly one question
    std::string name;
    size_t i = 12;
    while (i < len && p[i]) {
        uint8_t l = p[i];
        if ((l & 0xC0) || i + 1 + l > len) return;
        if (!name.empty()) name += '.';
        name.append(reinterpret_cast<const char*>(p + i + 1), l);
        i += 1 + l;
    }
    if (i + 5 > len) return;
    size_t qend = i + 5;
    uint16_t qtype = Be16(p + i + 1), qclass = Be16(p + i + 3);
    std::vector<uint8_t> query(p, p + qend);

    // Resolve off the NAT thread: lookups can take a while.
    dns_inflight_++;
    std::thread([this, query, name, qtype, qclass, sport] {
        std::vector<uint32_t> addrs;
        bool nxdomain = false;
        if (qclass == 1 && qtype == 1) {
            addrinfo hints = {}, *res = nullptr;
            hints.ai_family = AF_INET;
            int rc = getaddrinfo(name.c_str(), nullptr, &hints, &res);
            if (rc == 0) {
                for (addrinfo* a = res; a && addrs.size() < 8; a = a->ai_next) {
                    uint32_t v = ntohl(reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr.s_addr);
                    bool dup = false;
                    for (uint32_t x : addrs) dup |= x == v;
                    if (!dup) addrs.push_back(v);
                }
                freeaddrinfo(res);
            } else if (rc == EAI_NONAME) {
                nxdomain = true;
            }
        }
        std::vector<uint8_t> r = query;
        r[2] = 0x81 | (query[2] & 0x01);  // QR, opcode 0, keep RD
        r[3] = nxdomain ? 0x83 : 0x80;    // RA, rcode
        Put16(&r[6], uint16_t(addrs.size()));
        Put16(&r[8], 0);
        Put16(&r[10], 0);
        for (uint32_t a : addrs) {
            uint8_t rr[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            Put32(rr + 12, a);
            r.insert(r.end(), rr, rr + 16);
        }
        SendUdp(kDnsIp, 53, kGuestIp, sport, r.data(), r.size());
        dns_inflight_--;
    }).detach();
}

void UserNet::HandleTcp(uint32_t src, uint32_t dst, const uint8_t* p, size_t len) {
    if (len < 20) return;
    uint16_t sport = Be16(p), dport = Be16(p + 2);
    uint32_t seq = Be32(p + 4), ack = Be32(p + 8);
    size_t off = size_t(p[12] >> 4) * 4;
    uint8_t flags = p[13];
    uint16_t win = Be16(p + 14);
    if (off < 20 || off > len) return;
    const uint8_t* data = p + off;
    size_t dlen = len - off;
    TcpKey key{sport, dst, dport};
    auto it = tcp_.find(key);

    if (flags & kSyn) {
        if (flags & kAck) return;
        if (it != tcp_.end()) {
            TcpConn& old = *it->second;
            if (old.rcv_nxt == seq + 1) {  // retransmitted SYN
                if (old.connected && old.snd_una == old.snd_nxt && old.buf.empty()) {
                    old.snd_nxt--;  // our SYN-ACK was lost: send it again
                    SendTcp(old, kSyn | kAck, nullptr, 0, true);
                    old.snd_nxt++;
                }
                return;
            }
            CloseTcp(key);
        }
        auto c = std::make_unique<TcpConn>();
        c->key = key;
        c->remote_ip = dst;
        c->remote_port = dport;
        c->guest_port = sport;
        c->rcv_nxt = seq + 1;
        c->guest_win = win;
        c->snd_una = c->snd_nxt = uint32_t(NowMs() * 2654435761u);
        for (size_t i = 20; i + 1 < off;) {  // options: look for MSS
            uint8_t k = p[i];
            if (k == 0) break;
            if (k == 1) {
                ++i;
                continue;
            }
            uint8_t l = p[i + 1];
            if (l < 2) break;
            if (k == 2 && l == 4 && i + 4 <= off) c->guest_mss = Be16(p + i + 2);
            i += l;
        }
        c->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c->s == AETH_BAD_SOCK) {
            SendTcpRst(dst, dport, sport, 0, seq + 1, true);
            return;
        }
        SockNonBlock(c->s);
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(dport);
        to.sin_addr.s_addr = htonl(HostAddrFor(dst));
        int rc = connect(c->s, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        if (rc != 0 && !SockWouldBlock(SockErr())) {
            SockClose(c->s);
            SendTcpRst(dst, dport, sport, 0, seq + 1, true);
            return;
        }
        c->last_progress = NowMs();
        tcp_.emplace(key, std::move(c));
        return;
    }

    if (it == tcp_.end()) {
        // RFC 793 reset generation for a segment that matches no connection.
        if (!(flags & kRst)) {
            if (flags & kAck)
                SendTcpRst(dst, dport, sport, ack, 0, false);
            else
                SendTcpRst(dst, dport, sport, 0, seq + uint32_t(dlen) + ((flags & kFin) ? 1 : 0), true);
        }
        return;
    }
    TcpConn& c = *it->second;
    if (flags & kRst) {
        CloseTcp(key);
        return;
    }
    if (!c.connected) return;  // guest should not send before our SYN-ACK
    c.guest_win = win;

    if (flags & kAck) {
        uint32_t acked = ack - c.snd_una;
        if (acked > 0 && SeqLe(ack, c.snd_nxt)) {
            size_t data_acked = acked > c.buf.size() ? c.buf.size() : acked;
            c.buf.erase(c.buf.begin(), c.buf.begin() + data_acked);
            c.snd_una = ack;
            c.last_progress = NowMs();
            if (c.fin_sent && ack == c.snd_nxt) c.fin_acked = true;
        }
    }

    bool need_ack = false;
    if (dlen || (flags & kFin)) {
        if (seq != c.rcv_nxt) {
            need_ack = true;  // out of order / duplicate: re-ACK what we have
        } else {
            size_t taken = 0;
            if (dlen && !c.guest_fin) {
                int n = send(c.s, reinterpret_cast<const char*>(data), int(dlen), 0);
                if (n > 0) taken = size_t(n);
                else if (n < 0 && !SockWouldBlock(SockErr())) {
                    SendTcpRst(c.remote_ip, c.remote_port, c.guest_port, c.snd_nxt, c.rcv_nxt, true);
                    CloseTcp(key);
                    return;
                }
                // Anything not taken is not ACKed; the guest will retransmit it.
            }
            c.rcv_nxt += uint32_t(taken);
            if ((flags & kFin) && taken == dlen && !c.guest_fin) {
                c.guest_fin = true;
                c.rcv_nxt++;
                shutdown(c.s, AETH_SHUT_WR);
            }
            need_ack = true;
        }
    }
    if (c.fin_acked && c.guest_fin) {
        if (need_ack) SendTcp(c, kAck, nullptr, 0);
        CloseTcp(key);
        return;
    }
    PushTcpData(c);  // window may have opened
    if (need_ack) SendTcp(c, kAck, nullptr, 0);
}

// ---------------------------------------------------------------- host side

void UserNet::Poll(int timeout_ms) {
    PollLocked(timeout_ms);
    // Deliver outside mutex_ so the device may take its own lock without
    // inverting the lock order used by the vCPU thread (device -> NAT).
    if (wakeup_ && HasToGuest()) wakeup_();
}

void UserNet::PollLocked(int timeout_ms) {
    fd_set rd, wr, ex;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    int maxfd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t now = NowMs();
        for (auto it = udp_.begin(); it != udp_.end();) {
            if (now - it->second->last_used > 120000) {
                SockClose(it->second->s);
                it = udp_.erase(it);
                continue;
            }
            ++it;
        }
        int count = 0;
        for (auto& kv : tcp_) {
            TcpConn& c = *kv.second;
            if (count >= FD_SETSIZE - 8) break;
            if (!c.connected) {
                FD_SET(c.s, &wr);
                FD_SET(c.s, &ex);
            } else if (!c.host_eof && c.buf.size() < kMaxBuffered) {
                FD_SET(c.s, &rd);
            } else {
                continue;
            }
            if (int(c.s) > maxfd) maxfd = int(c.s);
            ++count;
        }
        for (auto& kv : udp_) {
            if (count >= FD_SETSIZE - 8) break;
            FD_SET(kv.second->s, &rd);
            if (int(kv.second->s) > maxfd) maxfd = int(kv.second->s);
            ++count;
        }
    }
    if (maxfd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    } else {
        timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(maxfd + 1, &rd, &wr, &ex, &tv) < 0) return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = NowMs();
    std::vector<TcpKey> dead;
    std::vector<uint8_t> tmp(65536);
    for (auto& kv : tcp_) {
        TcpConn& c = *kv.second;
        if (!c.connected) {
            if (FD_ISSET(c.s, &wr) || FD_ISSET(c.s, &ex)) {
                int err = 0;
                socklen_t el = sizeof(err);
                getsockopt(c.s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                if (err) {
                    SendTcpRst(c.remote_ip, c.remote_port, c.guest_port, 0, c.rcv_nxt, true);
                    dead.push_back(kv.first);
                    continue;
                }
                c.connected = true;
                SendTcp(c, kSyn | kAck, nullptr, 0, true);
                c.snd_nxt++;  // SYN consumes one sequence number
                c.snd_una = c.snd_nxt;
                c.last_progress = now;
            } else if (now - c.last_progress > 30000) {
                SendTcpRst(c.remote_ip, c.remote_port, c.guest_port, 0, c.rcv_nxt, true);
                dead.push_back(kv.first);
            }
            continue;
        }
        if (FD_ISSET(c.s, &rd)) {
            size_t room = kMaxBuffered - c.buf.size();
            int n = recv(c.s, reinterpret_cast<char*>(tmp.data()), int(room < tmp.size() ? room : tmp.size()), 0);
            if (n > 0) {
                c.buf.insert(c.buf.end(), tmp.begin(), tmp.begin() + n);
            } else if (n == 0) {
                c.host_eof = true;
            } else if (!SockWouldBlock(SockErr())) {
                SendTcpRst(c.remote_ip, c.remote_port, c.guest_port, c.snd_nxt, c.rcv_nxt, true);
                dead.push_back(kv.first);
                continue;
            }
        }
        // Retransmit (go-back-N) if the guest has not acknowledged for a second.
        if (c.snd_nxt != c.snd_una && now - c.last_progress > 1000) {
            c.snd_nxt = c.snd_una;
            if (!c.fin_acked) c.fin_sent = false;
            c.last_progress = now;
        }
        PushTcpData(c);
    }
    for (auto& k : dead) CloseTcp(k);

    for (auto& kv : udp_) {
        UdpFlow& fl = *kv.second;
        if (!FD_ISSET(fl.s, &rd)) continue;
        sockaddr_in from = {};
        socklen_t fl_len = sizeof(from);
        int n = recvfrom(fl.s, reinterpret_cast<char*>(tmp.data()), int(tmp.size()), 0,
                         reinterpret_cast<sockaddr*>(&from), &fl_len);
        if (n <= 0 || size_t(n) > 1472) continue;  // larger datagrams would need IP fragmentation
        fl.last_used = now;
        SendUdp(fl.remote_ip, fl.remote_port, kGuestIp, fl.guest_port, tmp.data(), size_t(n));
    }
}

}  // namespace Aetheron
