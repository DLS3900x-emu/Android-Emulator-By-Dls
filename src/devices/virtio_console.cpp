#include "virtio_console.h"

#include <cstring>

namespace Aetheron {

VirtioConsole::VirtioConsole(GuestMemory* mem, const std::string& log_path)
    : VirtioPciDevice(mem, 3, 0x078000, 2, 128, 12) {
    uint16_t cols = 80, rows = 25;
    std::memcpy(device_config_.data(), &cols, 2);
    std::memcpy(device_config_.data() + 2, &rows, 2);
    if (!log_path.empty()) log_ = fopen(log_path.c_str(), "wb");
}

VirtioConsole::~VirtioConsole() {
    if (log_) fclose(log_);
}

void VirtioConsole::OnQueueNotify(uint16_t q) {
    if (q == 0) {  // receiveq: guest posted buffers
        PumpRx();
        return;
    }
    VirtqChain c;
    bool any = false;
    while (PopChain(1, &c)) {  // transmitq
        std::vector<uint8_t> data = GatherReadable(c);
        if (log_ && !data.empty()) {
            fwrite(data.data(), 1, data.size(), log_);
            fflush(log_);
        }
        if (on_output_ && !data.empty()) on_output_(data.data(), data.size());
        PushUsed(1, c.head, 0);
        any = true;
    }
    if (any) NotifyQueue(1);
}

void VirtioConsole::SendToGuest(const std::string& data) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    pending_rx_.insert(pending_rx_.end(), data.begin(), data.end());
    PumpRx();
}

void VirtioConsole::PumpRx() {
    if (pending_rx_.empty() || !QueueReady(0)) return;
    VirtqChain c;
    bool any = false;
    while (!pending_rx_.empty() && PopChain(0, &c)) {
        uint32_t cap = 0;
        for (auto& b : c.bufs)
            if (b.device_writable) cap += b.len;
        std::vector<uint8_t> chunk;
        while (chunk.size() < cap && !pending_rx_.empty()) {
            chunk.push_back(pending_rx_.front());
            pending_rx_.pop_front();
        }
        uint32_t n = ScatterWritable(c, chunk.data(), uint32_t(chunk.size()));
        PushUsed(0, c.head, n);
        any = true;
    }
    if (any) NotifyQueue(0);
}

}  // namespace Aetheron
