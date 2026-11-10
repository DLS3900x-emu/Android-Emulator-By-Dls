#include "virtio_blk.h"

#include <algorithm>
#include <cstring>

#include "../common/log.h"

namespace Aetheron {

namespace {
constexpr uint64_t kFSegMax = 1ULL << 2, kFRo = 1ULL << 5, kFBlkSize = 1ULL << 6, kFFlush = 1ULL << 9;
constexpr uint32_t kTIn = 0, kTOut = 1, kTFlush = 4, kTGetId = 8;
constexpr uint8_t kSOk = 0, kSIoErr = 1, kSUnsupp = 2;
constexpr uint16_t kQueueSize = 256;
constexpr uint32_t kSegMax = kQueueSize - 2;
}  // namespace

VirtioBlk::VirtioBlk(GuestMemory* mem, std::unique_ptr<BlockBackend> backend, const char* serial)
    : VirtioPciDevice(mem, 2, 0x018000, 1, kQueueSize, 24), backend_(std::move(backend)) {
    std::memcpy(serial_, serial, std::min(sizeof(serial_), std::strlen(serial)));
    device_features_ |= kFSegMax | kFBlkSize | kFFlush;
    if (backend_->ReadOnly()) device_features_ |= kFRo;
    uint64_t sectors = backend_->SizeBytes() / 512;
    uint32_t seg_max = kSegMax, blk = 512;
    std::memcpy(device_config_.data() + 0, &sectors, 8);
    std::memcpy(device_config_.data() + 12, &seg_max, 4);
    std::memcpy(device_config_.data() + 20, &blk, 4);
    Log("virtio-blk '%s': %llu sectors (%llu MiB)", serial_, (unsigned long long)sectors,
        (unsigned long long)(backend_->SizeBytes() >> 20));
}

void VirtioBlk::OnQueueNotify(uint16_t q) {
    if (q != 0) return;
    VirtqChain c;
    bool any = false;
    while (PopChain(0, &c)) {
        uint32_t written = 0;
        uint8_t status = HandleRequest(c, &written);
        const VirtqBuffer& last = c.bufs.back();
        if (last.device_writable && last.len >= 1) {
            mem_->Write(last.gpa + last.len - 1, &status, 1);
            written += 1;
        }
        PushUsed(0, c.head, written);
        any = true;
    }
    if (any) NotifyQueue(0);
}

uint8_t VirtioBlk::HandleRequest(const VirtqChain& c, uint32_t* written) {
    *written = 0;
    if (c.bufs.size() < 2 || c.bufs[0].device_writable || c.bufs[0].len < 16) {
        LogOnce("blk-malformed", "virtio-blk: malformed request");
        return kSIoErr;
    }
    uint8_t hdr[16];
    if (!mem_->Read(c.bufs[0].gpa, hdr, 16)) return kSIoErr;
    uint32_t type;
    uint64_t sector;
    std::memcpy(&type, hdr, 4);
    std::memcpy(&sector, hdr + 8, 8);

    // Data descriptors: everything between the header and the status byte.
    // The status byte is the last byte of the last descriptor.
    size_t first = 1, last = c.bufs.size() - 1;
    const VirtqBuffer& st = c.bufs.back();
    std::vector<VirtqBuffer> data(c.bufs.begin() + first, c.bufs.begin() + last);
    if (st.len > 1) data.push_back({st.gpa, st.len - 1, st.device_writable});

    switch (type) {
        case kTIn:
        case kTOut: {
            uint64_t off = sector * 512;
            for (const auto& b : data) {
                if ((type == kTIn) != b.device_writable) return kSIoErr;
                if (off + b.len > backend_->SizeBytes()) return kSIoErr;
                void* host = mem_->Translate(b.gpa, b.len);
                bool ok;
                if (host) {
                    ok = type == kTIn ? backend_->Read(off, host, b.len) : backend_->Write(off, host, b.len);
                } else {
                    bounce_.resize(b.len);
                    if (type == kTIn) {
                        ok = backend_->Read(off, bounce_.data(), b.len) && mem_->Write(b.gpa, bounce_.data(), b.len);
                    } else {
                        ok = mem_->Read(b.gpa, bounce_.data(), b.len) && backend_->Write(off, bounce_.data(), b.len);
                    }
                }
                if (!ok) {
                    Log("virtio-blk: %s failed at offset 0x%llx len %u", type == kTIn ? "read" : "write",
                        (unsigned long long)off, b.len);
                    return kSIoErr;
                }
                if (type == kTIn) *written += b.len;
                off += b.len;
            }
            return kSOk;
        }
        case kTFlush:
            return backend_->Flush() ? kSOk : kSIoErr;
        case kTGetId: {
            for (const auto& b : data) {
                if (!b.device_writable) continue;
                uint32_t n = b.len < 20 ? b.len : 20;
                mem_->Write(b.gpa, serial_, n);
                *written += n;
                break;
            }
            return kSOk;
        }
        default:
            LogOnce("blk-type-" + std::to_string(type), "virtio-blk: unsupported request type %u", type);
            return kSUnsupp;
    }
}

}  // namespace Aetheron
