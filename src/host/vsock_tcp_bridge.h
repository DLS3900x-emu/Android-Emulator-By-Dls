// Makes a service inside Android reachable from Windows: listens on a TCP port
// of 127.0.0.1 and carries each connection over virtio-vsock to a port in the
// guest. Used for adb (adbd listens on vsock port 5555), which is how apps are
// installed and files are copied: `adb connect 127.0.0.1:6520`.
#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

#include "../devices/virtio_vsock.h"
#include "../net/sock.h"

namespace Aetheron {

class VsockTcpBridge {
public:
    VsockTcpBridge(VirtioVsock* vsock, uint16_t tcp_port, uint32_t guest_port)
        : vsock_(vsock), tcp_port_(tcp_port), guest_port_(guest_port) {}
    ~VsockTcpBridge() { Stop(); }

    bool Start();
    void Stop();
    uint16_t tcp_port() const { return tcp_port_; }

private:
    void AcceptLoop();
    void Serve(sock_t s);

    VirtioVsock* vsock_;
    uint16_t tcp_port_;
    uint32_t guest_port_;
    sock_t listener_ = AETH_BAD_SOCK;
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
};

}  // namespace Aetheron
