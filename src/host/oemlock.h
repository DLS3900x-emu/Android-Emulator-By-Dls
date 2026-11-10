// Host side of Cuttlefish's OEM-lock HAL (android.hardware.oemlock-service.remote),
// which talks to the launcher's "secure_env" over /dev/hvc10. Without an
// answer the HAL never registers and system_server blocks in OemLockService.
//
// Wire format (cuttlefish/common/libs/transport/channel.h), little endian:
//   u32 command:31, is_response:1 | u32 payload_size | payload
// Commands are OemLockField values; an empty payload reads the field, a
// one-byte payload sets it. Every reply carries one bool byte.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Aetheron {

class OemLockResponder {
public:
    // `state_path` keeps the three flags across runs; empty = memory only.
    explicit OemLockResponder(const std::string& state_path);

    // Feeds bytes written by the guest; returns the bytes to send back.
    std::vector<uint8_t> Feed(const uint8_t* data, size_t len);

private:
    enum Field : uint32_t { kAllowedByCarrier = 0, kAllowedByDevice = 1, kAllowed = 2, kLocked = 3 };
    void Load();
    void Save() const;

    std::string state_path_;
    std::vector<uint8_t> buf_;
    bool allowed_by_carrier_ = true;
    bool allowed_by_device_ = true;
    bool locked_ = false;  // matches androidboot.vbmeta.device_state = unlocked
};

}  // namespace Aetheron
