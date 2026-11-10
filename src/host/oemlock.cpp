#include "oemlock.h"

#include <cstdio>
#include <cstring>

#include "../common/log.h"

namespace Aetheron {

namespace {
constexpr size_t kHeaderLen = 8;
constexpr uint32_t kMaxPayload = 4096;
}  // namespace

OemLockResponder::OemLockResponder(const std::string& state_path) : state_path_(state_path) { Load(); }

void OemLockResponder::Load() {
    if (state_path_.empty()) return;
    FILE* f = fopen(state_path_.c_str(), "rb");
    if (!f) return;
    uint8_t b[3];
    if (fread(b, 1, 3, f) == 3) {
        allowed_by_carrier_ = b[0] != 0;
        allowed_by_device_ = b[1] != 0;
        locked_ = b[2] != 0;
    }
    fclose(f);
}

void OemLockResponder::Save() const {
    if (state_path_.empty()) return;
    FILE* f = fopen(state_path_.c_str(), "wb");
    if (!f) return;
    const uint8_t b[3] = {uint8_t(allowed_by_carrier_), uint8_t(allowed_by_device_), uint8_t(locked_)};
    fwrite(b, 1, 3, f);
    fclose(f);
}

std::vector<uint8_t> OemLockResponder::Feed(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    buf_.insert(buf_.end(), data, data + len);
    for (;;) {
        if (buf_.size() < kHeaderLen) break;
        uint32_t word, size;
        std::memcpy(&word, buf_.data(), 4);
        std::memcpy(&size, buf_.data() + 4, 4);
        if (size > kMaxPayload) {
            // Not this protocol (e.g. tty noise); a resync is impossible, so start over.
            LogOnce("oemlock-desync", "oemlock: dropped %zu bytes that are not a valid request", buf_.size());
            buf_.clear();
            break;
        }
        if (buf_.size() < kHeaderLen + size) break;
        const uint32_t command = word & 0x7FFFFFFF;
        if (word & 0x80000000u) {
            // Our own reply echoed by a tty that is not in raw mode yet; never answer it.
            buf_.erase(buf_.begin(), buf_.begin() + kHeaderLen + size);
            continue;
        }
        const bool set = size == 1;
        const bool value = set && buf_[kHeaderLen] != 0;
        bool result = false;
        switch (command) {
            case kAllowedByCarrier:
                if (set) allowed_by_carrier_ = value;
                result = allowed_by_carrier_;
                break;
            case kAllowedByDevice:
                if (set) allowed_by_device_ = value;
                result = allowed_by_device_;
                break;
            case kAllowed:
                result = allowed_by_carrier_ && allowed_by_device_;
                break;
            case kLocked:
                if (set) locked_ = value;
                result = locked_;
                break;
            default:
                LogOnce("oemlock-cmd-" + std::to_string(command), "oemlock: unknown command %u (answered false)",
                        command);
                break;
        }
        if (set && command != kAllowed) Save();
        LogOnce("oemlock-first", "oemlock: guest HAL connected on hvc10 (first request: field %u %s)", command,
                set ? "set" : "get");
        const uint32_t reply_word = command | 0x80000000u, reply_size = 1;
        const size_t at = out.size();
        out.resize(at + kHeaderLen + 1);
        std::memcpy(&out[at], &reply_word, 4);
        std::memcpy(&out[at + 4], &reply_size, 4);
        out[at + kHeaderLen] = result ? 1 : 0;
        buf_.erase(buf_.begin(), buf_.begin() + kHeaderLen + size);
    }
    return out;
}

}  // namespace Aetheron
