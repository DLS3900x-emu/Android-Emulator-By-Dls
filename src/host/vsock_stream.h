// A blocking byte stream from the host to a vsock port inside Android, built
// on VirtioVsock's host-initiated streams. Used by the adb client.
#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>

#include "../devices/virtio_vsock.h"
#include "adb_client.h"

namespace Aetheron {

class VsockStream : public ByteStream {
public:
    explicit VsockStream(VirtioVsock* vsock) : vsock_(vsock) {}
    ~VsockStream() override { Close(); }

    // False if nothing listens on the port (yet) or Android does not answer in time.
    bool Connect(uint32_t guest_port, int timeout_ms);
    void Close();

    bool Write(const uint8_t* data, size_t len) override;
    bool ReadExact(uint8_t* data, size_t len, int timeout_ms) override;

private:
    VirtioVsock* vsock_;
    uint32_t handle_ = 0;
    bool open_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<uint8_t> rx_;
    bool connected_ = false, closed_ = false;
};

}  // namespace Aetheron
