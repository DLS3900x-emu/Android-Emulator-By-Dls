#include "vsock_stream.h"

#include <chrono>

namespace Aetheron {

bool VsockStream::Connect(uint32_t guest_port, int timeout_ms) {
    Close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rx_.clear();
        connected_ = closed_ = false;
    }
    // The callbacks run on a vCPU thread with the device locked; they only touch our own state.
    handle_ = vsock_->HostConnect(
        guest_port,
        [this](const uint8_t* data, size_t len) {
            std::lock_guard<std::mutex> lock(mutex_);
            rx_.insert(rx_.end(), data, data + len);
            cv_.notify_all();
        },
        [this](bool connected) {
            std::lock_guard<std::mutex> lock(mutex_);
            (connected ? connected_ : closed_) = true;
            cv_.notify_all();
        });
    open_ = true;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return connected_ || closed_; });
    const bool ok = connected_ && !closed_;
    lock.unlock();
    if (!ok) Close();
    return ok;
}

void VsockStream::Close() {
    if (!open_) return;
    open_ = false;
    vsock_->HostClose(handle_);  // after this no callback runs any more
}

bool VsockStream::Write(const uint8_t* data, size_t len) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
    }
    return open_ && vsock_->HostSend(handle_, data, len);
}

bool VsockStream::ReadExact(uint8_t* data, size_t len, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return rx_.size() >= len || closed_; }))
        return false;
    if (rx_.size() < len) return false;
    for (size_t i = 0; i < len; ++i) {
        data[i] = rx_.front();
        rx_.pop_front();
    }
    return true;
}

}  // namespace Aetheron
