// A small client for Android's debug protocol (adb), enough to run shell
// commands and to install apps. It talks to adbd inside Android over any
// byte stream; Aetheron uses a virtio-vsock stream to port 5555, so no
// adb.exe and no TCP port are involved.
//
// Wire format: 24-byte header (command, arg0, arg1, length, checksum, magic)
// followed by the payload. See packages/modules/adb/protocol.txt in AOSP.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Aetheron {

class ByteStream {
public:
    virtual ~ByteStream() = default;
    virtual bool Write(const uint8_t* data, size_t len) = 0;
    // Reads exactly `len` bytes; false on timeout or when the stream closed.
    virtual bool ReadExact(uint8_t* data, size_t len, int timeout_ms) = 0;
};

class AdbClient {
public:
    // Fills a buffer with up to `max` bytes of input; returns 0 at the end.
    using InputFn = std::function<size_t(uint8_t* buf, size_t max)>;

    explicit AdbClient(ByteStream* stream) : stream_(stream) {}

    // Handshake. Fails with a readable reason, e.g. when Android demands key authorisation.
    bool Connect(std::string* error);

    // Runs an adbd service ("shell:<command>", "exec:<command>") and collects
    // what it prints. `input`, if given, is streamed to the service first.
    bool Run(const std::string& service, const InputFn& input, std::string* output, std::string* error);

    bool Shell(const std::string& command, std::string* output, std::string* error) {
        return Run("shell:" + command, nullptr, output, error);
    }

private:
    struct Message {
        uint32_t command = 0, arg0 = 0, arg1 = 0;
        std::vector<uint8_t> data;
    };
    bool Send(uint32_t command, uint32_t arg0, uint32_t arg1, const uint8_t* data, size_t len);
    bool Receive(Message* m, int timeout_ms);

    ByteStream* stream_;
    uint32_t max_payload_ = 4096;
    uint32_t next_local_id_ = 1;
};

}  // namespace Aetheron
