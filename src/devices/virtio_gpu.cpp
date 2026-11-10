#include "virtio_gpu.h"

#include <cstring>

#include "../common/log.h"

namespace Aetheron {

namespace {
enum : uint32_t {
    kGetDisplayInfo = 0x0100,
    kResCreate2d = 0x0101,
    kResUnref = 0x0102,
    kSetScanout = 0x0103,
    kResFlush = 0x0104,
    kTransferToHost2d = 0x0105,
    kResAttachBacking = 0x0106,
    kResDetachBacking = 0x0107,
    kUpdateCursor = 0x0300,
    kMoveCursor = 0x0301,
    kRespOkNodata = 0x1100,
    kRespOkDisplayInfo = 0x1101,
    kRespErrUnspec = 0x1200,
    kRespErrOutOfMemory = 0x1201,
    kRespErrInvalidScanout = 0x1202,
    kRespErrInvalidResource = 0x1203,
    kRespErrInvalidParameter = 0x1205,
};
constexpr uint32_t kFlagFence = 1;

// virtio_gpu_formats -> byte offsets of B, G, R inside a 4-byte pixel.
bool FormatOffsets(uint32_t fmt, int* b, int* g, int* r) {
    switch (fmt) {
        case 1: case 2: *b = 0; *g = 1; *r = 2; return true;    // B8G8R8A8 / B8G8R8X8
        case 3: case 4: *b = 3; *g = 2; *r = 1; return true;    // A8R8G8B8 / X8R8G8B8
        case 67: case 134: *b = 2; *g = 1; *r = 0; return true; // R8G8B8A8 / R8G8B8X8
        case 121: case 68: *b = 1; *g = 2; *r = 3; return true; // A8B8G8R8 / X8B8G8R8
        default: return false;
    }
}

uint32_t U32(const std::vector<uint8_t>& v, size_t off) {
    uint32_t x = 0;
    if (off + 4 <= v.size()) std::memcpy(&x, v.data() + off, 4);
    return x;
}
uint64_t U64(const std::vector<uint8_t>& v, size_t off) {
    uint64_t x = 0;
    if (off + 8 <= v.size()) std::memcpy(&x, v.data() + off, 8);
    return x;
}
}  // namespace

VirtioGpu::VirtioGpu(GuestMemory* mem, ScanoutBuffer* scanout, uint32_t width, uint32_t height)
    : VirtioPciDevice(mem, 16, 0x038000, 2, 256, 16), scanout_(scanout), width_(width), height_(height) {
    uint32_t num_scanouts = 1;
    std::memcpy(device_config_.data() + 8, &num_scanouts, 4);
}

void VirtioGpu::WriteDeviceConfig(uint32_t offset, uint8_t size, uint32_t value) {
    if (offset == 4) {  // events_clear
        uint32_t ev;
        std::memcpy(&ev, device_config_.data(), 4);
        ev &= ~value;
        std::memcpy(device_config_.data(), &ev, 4);
    }
}

void VirtioGpu::OnReset() {
    resources_.clear();
    scanout_resource_ = 0;
    std::lock_guard<std::mutex> lock(scanout_->mutex);
    scanout_->enabled = false;
}

void VirtioGpu::OnQueueNotify(uint16_t q) {
    VirtqChain c;
    bool any = false;
    while (PopChain(q, &c)) {
        uint32_t written = 0;
        if (q == 0) {
            ProcessControl(c, &written);
        } else {
            // cursor queue: this display has no hardware cursor plane
            std::vector<uint8_t> cmd = GatherReadable(c);
            uint32_t type = U32(cmd, 0);
            if (type != kUpdateCursor && type != kMoveCursor)
                LogOnce("gpu-cursor-" + std::to_string(type), "virtio-gpu: unknown cursor command 0x%x", type);
        }
        PushUsed(q, c.head, written);
        any = true;
    }
    if (any) NotifyQueue(q);
}

bool VirtioGpu::CopyFromBacking(const Resource& r, uint64_t offset, void* dst, uint32_t len) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    uint64_t pos = 0;
    for (const auto& e : r.backing) {
        if (len == 0) return true;
        if (offset < pos + e.second) {
            uint64_t within = offset - pos;
            uint32_t n = uint32_t(e.second - within);
            if (n > len) n = len;
            if (!mem_->Read(e.first + within, out, n)) return false;
            out += n;
            offset += n;
            len -= n;
        }
        pos += e.second;
    }
    return len == 0;
}

void VirtioGpu::FlushToScanout(uint32_t res_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    auto it = resources_.find(res_id);
    if (it == resources_.end() || res_id != scanout_resource_) return;
    const Resource& r = it->second;
    int bo, go, ro;
    if (!FormatOffsets(r.format, &bo, &go, &ro)) return;
    // The visible part is the rectangle given with SET_SCANOUT. The guest's
    // framebuffer can be larger (rows/columns added for alignment), and that
    // padding must not be shown or counted for touch coordinates.
    const uint32_t sx = scan_x_ < r.width ? scan_x_ : 0, sy = scan_y_ < r.height ? scan_y_ : 0;
    const uint32_t sw = scan_w_ && scan_w_ <= r.width - sx ? scan_w_ : r.width - sx;
    const uint32_t sh = scan_h_ && scan_h_ <= r.height - sy ? scan_h_ : r.height - sy;
    if (!sw || !sh) return;
    std::lock_guard<std::mutex> lock(scanout_->mutex);
    if (scanout_->width != sw || scanout_->height != sh) {
        scanout_->width = sw;
        scanout_->height = sh;
        scanout_->bgra.assign(size_t(sw) * sh, 0xFF000000u);
        x = sx;
        y = sy;
        w = sw;
        h = sh;
    }
    // Intersect the flushed rectangle with the visible one.
    const uint64_t x0 = x > sx ? x : sx, y0 = y > sy ? y : sy;
    const uint64_t x1 = uint64_t(x) + w < uint64_t(sx) + sw ? uint64_t(x) + w : uint64_t(sx) + sw;
    const uint64_t y1 = uint64_t(y) + h < uint64_t(sy) + sh ? uint64_t(y) + h : uint64_t(sy) + sh;
    if (x0 >= x1 || y0 >= y1) return;
    for (uint64_t row = y0; row < y1; ++row) {
        const uint8_t* src = r.pixels.data() + (size_t(row) * r.width + x0) * 4;
        uint32_t* dst = scanout_->bgra.data() + size_t(row - sy) * sw + (x0 - sx);
        for (uint64_t i = 0; i < x1 - x0; ++i, src += 4)
            dst[i] = 0xFF000000u | (uint32_t(src[ro]) << 16) | (uint32_t(src[go]) << 8) | src[bo];
    }
    scanout_->enabled = true;
    scanout_->frame++;
}

void VirtioGpu::ProcessControl(const VirtqChain& c, uint32_t* written) {
    std::vector<uint8_t> cmd = GatherReadable(c);
    if (cmd.size() < 24) return;
    uint32_t type = U32(cmd, 0), flags = U32(cmd, 4), ctx = U32(cmd, 16);
    uint64_t fence = U64(cmd, 8);

    std::vector<uint8_t> resp(24, 0);
    uint32_t rtype = kRespOkNodata;

    switch (type) {
        case kGetDisplayInfo: {
            rtype = kRespOkDisplayInfo;
            resp.resize(24 + 16 * 24, 0);
            uint32_t mode[6] = {0, 0, width_, height_, 1, 0};  // rect, enabled, flags
            std::memcpy(resp.data() + 24, mode, sizeof(mode));
            break;
        }
        case kResCreate2d: {
            uint32_t id = U32(cmd, 24), fmt = U32(cmd, 28), w = U32(cmd, 32), h = U32(cmd, 36);
            int b, g, r;
            if (!id || !w || !h || w > 16384 || h > 16384) {
                rtype = kRespErrInvalidParameter;
            } else if (!FormatOffsets(fmt, &b, &g, &r)) {
                LogOnce("gpu-fmt-" + std::to_string(fmt), "virtio-gpu: unsupported format %u", fmt);
                rtype = kRespErrInvalidParameter;
            } else if (resources_.count(id)) {
                rtype = kRespErrInvalidResource;
            } else {
                Resource res;
                res.format = fmt;
                res.width = w;
                res.height = h;
                try {
                    res.pixels.assign(size_t(w) * h * 4, 0);
                    resources_[id] = std::move(res);
                } catch (...) {
                    rtype = kRespErrOutOfMemory;
                }
            }
            break;
        }
        case kResUnref: {
            uint32_t id = U32(cmd, 24);
            if (!resources_.erase(id)) rtype = kRespErrInvalidResource;
            if (id == scanout_resource_) scanout_resource_ = 0;
            break;
        }
        case kSetScanout: {
            uint32_t sid = U32(cmd, 40), id = U32(cmd, 44);
            if (sid != 0) {
                rtype = kRespErrInvalidScanout;
            } else if (id == 0) {
                scanout_resource_ = 0;
                std::lock_guard<std::mutex> lock(scanout_->mutex);
                scanout_->enabled = false;
            } else if (!resources_.count(id)) {
                rtype = kRespErrInvalidResource;
            } else {
                scanout_resource_ = id;
                scan_x_ = U32(cmd, 24);
                scan_y_ = U32(cmd, 28);
                scan_w_ = U32(cmd, 32);
                scan_h_ = U32(cmd, 36);
                auto& r = resources_[id];
                FlushToScanout(id, 0, 0, r.width, r.height);
            }
            break;
        }
        case kResFlush: {
            uint32_t x = U32(cmd, 24), y = U32(cmd, 28), w = U32(cmd, 32), h = U32(cmd, 36), id = U32(cmd, 40);
            if (!resources_.count(id))
                rtype = kRespErrInvalidResource;
            else
                FlushToScanout(id, x, y, w, h);
            break;
        }
        case kTransferToHost2d: {
            uint32_t x = U32(cmd, 24), y = U32(cmd, 28), w = U32(cmd, 32), h = U32(cmd, 36);
            uint64_t offset = U64(cmd, 40);
            uint32_t id = U32(cmd, 48);
            auto it = resources_.find(id);
            if (it == resources_.end()) {
                rtype = kRespErrInvalidResource;
                break;
            }
            Resource& r = it->second;
            if (x + w > r.width || y + h > r.height || r.backing.empty()) {
                rtype = kRespErrInvalidParameter;
                break;
            }
            uint32_t stride = r.width * 4;
            for (uint32_t row = 0; row < h; ++row) {
                uint64_t src = offset + uint64_t(row) * stride;
                uint8_t* dst = r.pixels.data() + size_t(y + row) * stride + size_t(x) * 4;
                if (!CopyFromBacking(r, src, dst, w * 4)) {
                    rtype = kRespErrInvalidParameter;
                    break;
                }
            }
            break;
        }
        case kResAttachBacking: {
            uint32_t id = U32(cmd, 24), n = U32(cmd, 28);
            auto it = resources_.find(id);
            if (it == resources_.end()) {
                rtype = kRespErrInvalidResource;
            } else if (32 + size_t(n) * 16 > cmd.size()) {
                rtype = kRespErrInvalidParameter;
            } else {
                it->second.backing.clear();
                for (uint32_t i = 0; i < n; ++i)
                    it->second.backing.emplace_back(U64(cmd, 32 + i * 16), U32(cmd, 32 + i * 16 + 8));
            }
            break;
        }
        case kResDetachBacking: {
            auto it = resources_.find(U32(cmd, 24));
            if (it == resources_.end())
                rtype = kRespErrInvalidResource;
            else
                it->second.backing.clear();
            break;
        }
        default:
            LogOnce("gpu-cmd-" + std::to_string(type), "virtio-gpu: unsupported control command 0x%x", type);
            rtype = kRespErrUnspec;
    }

    std::memcpy(resp.data(), &rtype, 4);
    if (flags & kFlagFence) {
        std::memcpy(resp.data() + 4, &flags, 4);
        std::memcpy(resp.data() + 8, &fence, 8);
        std::memcpy(resp.data() + 16, &ctx, 4);
    }
    *written = ScatterWritable(c, resp.data(), uint32_t(resp.size()));
}

}  // namespace Aetheron
