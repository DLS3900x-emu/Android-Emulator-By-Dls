#include "adb_client.h"

#include <algorithm>
#include <cstring>

namespace Aetheron {

namespace {
constexpr uint32_t kCnxn = 0x4E584E43, kAuth = 0x48545541, kOpen = 0x4E45504F, kOkay = 0x59414B4F,
                   kClse = 0x45534C43, kWrte = 0x45545257;
constexpr uint32_t kVersion = 0x01000001;         // peers at this version may skip checksums
constexpr uint32_t kOurMaxPayload = 256 * 1024;
constexpr uint32_t kChunk = 32 * 1024;            // per WRTE when streaming input
constexpr size_t kMaxOutput = 1 << 20;
constexpr int kHandshakeMs = 10000, kServiceMs = 300000;
}  // namespace

bool AdbClient::Send(uint32_t command, uint32_t arg0, uint32_t arg1, const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += data[i];
    const uint32_t header[6] = {command, arg0, arg1, uint32_t(len), sum, command ^ 0xFFFFFFFFu};
    std::vector<uint8_t> packet(sizeof(header) + len);
    std::memcpy(packet.data(), header, sizeof(header));
    if (len) std::memcpy(packet.data() + sizeof(header), data, len);
    return stream_->Write(packet.data(), packet.size());
}

bool AdbClient::Receive(Message* m, int timeout_ms) {
    uint32_t header[6];
    if (!stream_->ReadExact(reinterpret_cast<uint8_t*>(header), sizeof(header), timeout_ms)) return false;
    if (header[5] != (header[0] ^ 0xFFFFFFFFu) || header[3] > kOurMaxPayload) return false;  // not adb
    m->command = header[0];
    m->arg0 = header[1];
    m->arg1 = header[2];
    m->data.resize(header[3]);
    return header[3] == 0 || stream_->ReadExact(m->data.data(), header[3], timeout_ms);
}

bool AdbClient::Connect(std::string* error) {
    static const char kBanner[] = "host::";
    if (!Send(kCnxn, kVersion, kOurMaxPayload, reinterpret_cast<const uint8_t*>(kBanner), sizeof(kBanner))) {
        *error = "the connection to Android closed";
        return false;
    }
    Message m;
    if (!Receive(&m, kHandshakeMs)) {
        *error = "Android's debug service did not answer";
        return false;
    }
    if (m.command == kAuth) {
        *error = "Android asks for adb key authorisation (ro.adb.secure=1), which Aetheron does not do";
        return false;
    }
    if (m.command != kCnxn) {
        *error = "unexpected reply from Android's debug service";
        return false;
    }
    max_payload_ = std::max<uint32_t>(1024, std::min(m.arg1, kOurMaxPayload));
    return true;
}

bool AdbClient::Run(const std::string& service, const InputFn& input, std::string* output, std::string* error) {
    const uint32_t local = next_local_id_++;
    if (!Send(kOpen, local, 0, reinterpret_cast<const uint8_t*>(service.c_str()), service.size() + 1)) {
        *error = "the connection to Android closed";
        return false;
    }
    uint32_t remote = 0;
    bool closed = false;
    Message m;
    // Handles one incoming message for this stream; `acked` reports an OKAY.
    auto pump = [&](bool* acked) {
        if (!Receive(&m, kServiceMs)) {
            *error = "Android stopped answering";
            return false;
        }
        if (m.arg1 != local) return true;  // for a stream that no longer exists
        if (m.command == kOkay) {
            remote = m.arg0;
            if (acked) *acked = true;
        } else if (m.command == kWrte) {
            if (output && output->size() < kMaxOutput) output->append(m.data.begin(), m.data.end());
            if (!Send(kOkay, local, m.arg0, nullptr, 0)) {
                *error = "the connection to Android closed";
                return false;
            }
        } else if (m.command == kClse) {
            closed = true;
        }
        return true;
    };

    bool opened = false;
    while (!opened && !closed)
        if (!pump(&opened)) return false;
    if (!opened) {
        *error = "Android refused to run: " + service.substr(0, 60);
        return false;
    }

    if (input) {
        std::vector<uint8_t> buf(std::min(max_payload_, kChunk));
        while (!closed) {
            const size_t n = input(buf.data(), buf.size());
            if (n == 0) break;
            if (!Send(kWrte, local, remote, buf.data(), n)) {
                *error = "the connection to Android closed";
                return false;
            }
            bool acked = false;  // adb allows one unacknowledged write per stream
            while (!acked && !closed)
                if (!pump(&acked)) return false;
        }
    }
    while (!closed)
        if (!pump(nullptr)) return false;
    Send(kClse, local, remote, nullptr, 0);
    return true;
}

}  // namespace Aetheron
