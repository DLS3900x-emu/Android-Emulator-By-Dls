#include "vsock_tcp_bridge.h"

#include <chrono>
#include <memory>
#include <mutex>

#include "../common/log.h"

namespace Aetheron {

namespace {
#ifdef _WIN32
constexpr int kShutBoth = SD_BOTH;
constexpr int kSendFlags = 0;
#else
constexpr int kShutBoth = SHUT_RDWR;
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

struct StreamState {
    std::mutex m;
    bool connected = false, closed = false;
};

bool SendAll(sock_t s, const uint8_t* data, size_t len) {
    while (len) {
        const int n = send(s, reinterpret_cast<const char*>(data), int(len), kSendFlags);
        if (n <= 0) return false;
        data += n;
        len -= size_t(n);
    }
    return true;
}
}  // namespace

bool VsockTcpBridge::Start() {
    SockStartup();
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == AETH_BAD_SOCK) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(tcp_port_);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this PC only
    if (bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(listener_, 8) != 0) {
        Log("bridge: cannot listen on 127.0.0.1:%u (error %d) - is another Aetheron or emulator using it?", tcp_port_,
            SockErr());
        SockClose(listener_);
        listener_ = AETH_BAD_SOCK;
        return false;
    }
    Log("bridge: 127.0.0.1:%u -> Android vsock port %u (adb connect 127.0.0.1:%u)", tcp_port_, guest_port_, tcp_port_);
    accept_thread_ = std::thread(&VsockTcpBridge::AcceptLoop, this);
    return true;
}

void VsockTcpBridge::Stop() {
    stopping_ = true;
    if (listener_ != AETH_BAD_SOCK) {
        shutdown(listener_, kShutBoth);
        SockClose(listener_);
        listener_ = AETH_BAD_SOCK;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

void VsockTcpBridge::AcceptLoop() {
    while (!stopping_) {
        const sock_t s = accept(listener_, nullptr, nullptr);
        if (s == AETH_BAD_SOCK) {
            if (stopping_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // Streams end with their TCP connection; the device outlives them.
        std::thread(&VsockTcpBridge::Serve, this, s).detach();
    }
}

void VsockTcpBridge::Serve(sock_t s) {
    auto st = std::make_shared<StreamState>();
    const uint32_t handle = vsock_->HostConnect(
        guest_port_,
        [s](const uint8_t* data, size_t len) { SendAll(s, data, len); },
        [s, st](bool connected) {
            std::lock_guard<std::mutex> lock(st->m);
            if (connected) {
                st->connected = true;
            } else {
                st->closed = true;
                shutdown(s, kShutBoth);  // wakes the recv() below
            }
        });
    // Android answers within milliseconds once adbd is up; before that nothing listens.
    bool ok = false;
    for (int i = 0; i < 300 && !stopping_; ++i) {
        {
            std::lock_guard<std::mutex> lock(st->m);
            if (st->closed) break;
            if (st->connected) {
                ok = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (ok) {
        uint8_t buf[16384];
        for (;;) {
            const int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) break;
            if (!vsock_->HostSend(handle, buf, size_t(n))) break;
        }
    }
    vsock_->HostClose(handle);  // after this no callback can touch the socket
    SockClose(s);
}

}  // namespace Aetheron
