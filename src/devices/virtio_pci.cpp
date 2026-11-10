#include "virtio_pci.h"

#include <windows.h>

#include <cstring>

#include "../common/log.h"

namespace Aetheron {

namespace {
constexpr uint8_t kCapCommon = 1, kCapNotify = 2, kCapIsr = 3, kCapDevice = 4;
constexpr uint32_t kCommonOff = 0x0000, kIsrOff = 0x1000, kDeviceOff = 0x2000, kNotifyOff = 0x3000;
constexpr uint32_t kNotifyMultiplier = 4;
}  // namespace

VirtioPciDevice::VirtioPciDevice(GuestMemory* mem, uint16_t virtio_id, uint32_t pci_class, uint16_t num_queues,
                                 uint16_t queue_size, uint32_t device_config_size)
    : mem_(mem), device_config_(device_config_size, 0), queues_(num_queues) {
    for (auto& q : queues_) q.max_size = q.size = queue_size;
    uint16_t did = uint16_t(0x1040 + virtio_id);
    config[0x00] = 0xF4; config[0x01] = 0x1A;  // Red Hat / virtio
    config[0x02] = uint8_t(did); config[0x03] = uint8_t(did >> 8);
    config[0x06] = 0x10;                       // status: capabilities list
    config[0x08] = 0x01;                       // revision 1 = modern device
    config[0x09] = uint8_t(pci_class);
    config[0x0A] = uint8_t(pci_class >> 8);
    config[0x0B] = uint8_t(pci_class >> 16);
    config[0x2C] = 0xF4; config[0x2D] = 0x1A;  // subsystem vendor
    config[0x2E] = uint8_t(did); config[0x2F] = uint8_t(did >> 8);
    config[0x34] = 0x40;                       // capabilities pointer

    uint8_t ptr = 0x40;
    AddCapability(ptr, kCapCommon, kCommonOff, 0x40, 0);
    AddCapability(ptr, kCapNotify, kNotifyOff, 0x1000, kNotifyMultiplier);
    AddCapability(ptr, kCapIsr, kIsrOff, 0x4, 0);
    AddCapability(ptr, kCapDevice, kDeviceOff, 0x1000, 0);
}

void VirtioPciDevice::AddCapability(uint8_t& ptr, uint8_t type, uint32_t offset, uint32_t length, uint32_t extra) {
    uint8_t len = type == kCapNotify ? 20 : 16;
    uint8_t* c = config + ptr;
    c[0] = 0x09;  // vendor-specific capability
    c[1] = 0;     // next (patched below)
    c[2] = len;
    c[3] = type;
    c[4] = 0;     // BAR0
    std::memcpy(c + 8, &offset, 4);
    std::memcpy(c + 12, &length, 4);
    if (type == kCapNotify) std::memcpy(c + 16, &extra, 4);
    // link previous capability to this one
    if (ptr != 0x40) {
        uint8_t p = 0x40;
        while (config[p + 1]) p = config[p + 1];
        config[p + 1] = ptr;
    }
    ptr = uint8_t(ptr + ((len + 3) & ~3));
}

void VirtioPciDevice::Reset() {
    status_ = 0;
    isr_ = 0;
    driver_features_ = 0;
    device_feature_select_ = driver_feature_select_ = 0;
    queue_select_ = 0;
    config_msix_ = 0xFFFF;
    for (auto& q : queues_) {
        q.size = q.max_size;
        q.enabled = false;
        q.desc = q.driver = q.device = 0;
        q.last_avail = q.used_idx = 0;
        q.msix_vector = 0xFFFF;
    }
    SetIntx(false);
    OnReset();
}

uint64_t VirtioPciDevice::BarRead(uint64_t offset, uint8_t size) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (offset < kIsrOff) return CommonRead(uint32_t(offset), size);
    if (offset < kDeviceOff) {
        if (offset != kIsrOff) return 0;
        uint8_t v = isr_;
        isr_ = 0;  // read-to-clear, deasserts INTx
        SetIntx(false);
        return v;
    }
    if (offset < kNotifyOff) return ReadDeviceConfig(uint32_t(offset - kDeviceOff), size);
    return 0;
}

void VirtioPciDevice::BarWrite(uint64_t offset, uint8_t size, uint64_t value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (offset < kIsrOff) {
        CommonWrite(uint32_t(offset), size, uint32_t(value));
    } else if (offset >= kDeviceOff && offset < kNotifyOff) {
        WriteDeviceConfig(uint32_t(offset - kDeviceOff), size, uint32_t(value));
    } else if (offset >= kNotifyOff) {
        uint32_t q = uint32_t((offset - kNotifyOff) / kNotifyMultiplier);
        if (q < queues_.size() && driver_ok()) OnQueueNotify(uint16_t(q));
    }
}

uint32_t VirtioPciDevice::ReadDeviceConfig(uint32_t offset, uint8_t size) {
    uint32_t v = 0;
    if (offset + size <= device_config_.size()) std::memcpy(&v, device_config_.data() + offset, size);
    return v;
}

uint32_t VirtioPciDevice::CommonRead(uint32_t off, uint8_t size) {
    Queue* q = queue_select_ < queues_.size() ? &queues_[queue_select_] : nullptr;
    switch (off) {
        case 0x00: return device_feature_select_;
        case 0x04:
            if (device_feature_select_ == 0) return uint32_t(device_features_);
            if (device_feature_select_ == 1) return uint32_t(device_features_ >> 32);
            return 0;
        case 0x08: return driver_feature_select_;
        case 0x0C:
            if (driver_feature_select_ == 0) return uint32_t(driver_features_);
            if (driver_feature_select_ == 1) return uint32_t(driver_features_ >> 32);
            return 0;
        case 0x10: return config_msix_;
        case 0x12: return uint32_t(queues_.size());
        case 0x14: return status_;
        case 0x15: return config_generation_;
        case 0x16: return queue_select_;
        case 0x18: return q ? q->size : 0;
        case 0x1A: return q ? q->msix_vector : 0xFFFF;
        case 0x1C: return q ? q->enabled : 0;
        case 0x1E: return q ? queue_select_ : 0;  // queue_notify_off
        case 0x20: return q ? uint32_t(q->desc) : 0;
        case 0x24: return q ? uint32_t(q->desc >> 32) : 0;
        case 0x28: return q ? uint32_t(q->driver) : 0;
        case 0x2C: return q ? uint32_t(q->driver >> 32) : 0;
        case 0x30: return q ? uint32_t(q->device) : 0;
        case 0x34: return q ? uint32_t(q->device >> 32) : 0;
        default: return 0;
    }
}

void VirtioPciDevice::CommonWrite(uint32_t off, uint8_t size, uint32_t v) {
    Queue* q = queue_select_ < queues_.size() ? &queues_[queue_select_] : nullptr;
    auto set_lo = [](uint64_t& r, uint32_t v) { r = (r & 0xFFFFFFFF00000000ULL) | v; };
    auto set_hi = [](uint64_t& r, uint32_t v) { r = (r & 0xFFFFFFFFULL) | (uint64_t(v) << 32); };
    switch (off) {
        case 0x00: device_feature_select_ = v; break;
        case 0x08: driver_feature_select_ = v; break;
        case 0x0C:
            if (status_ & 0x08) break;  // FEATURES_OK already set
            if (driver_feature_select_ == 0) set_lo(driver_features_, v);
            else if (driver_feature_select_ == 1) set_hi(driver_features_, v);
            break;
        case 0x10: config_msix_ = uint16_t(v); break;
        case 0x14: {
            uint8_t nv = uint8_t(v);
            if (nv == 0) {
                Reset();
                break;
            }
            if ((nv & 0x08) && !(status_ & 0x08)) {
                // FEATURES_OK: accept only subsets of what we offer, and v1 is mandatory.
                if ((driver_features_ & ~device_features_) || !(driver_features_ & kFeatureVersion1)) {
                    Log("virtio %04x: driver features 0x%llx rejected", config[2] | (config[3] << 8),
                        (unsigned long long)driver_features_);
                    nv &= ~0x08;
                }
            }
            bool became_ok = (nv & 0x04) && !(status_ & 0x04);
            status_ = nv;
            if (became_ok) OnDriverOk();
            break;
        }
        case 0x16: queue_select_ = uint16_t(v); break;
        case 0x18:
            if (q && v && v <= q->max_size && (v & (v - 1)) == 0) q->size = uint16_t(v);
            break;
        case 0x1A: if (q) q->msix_vector = uint16_t(v); break;
        case 0x1C:
            if (q && v == 1) {
                q->enabled = true;
                q->last_avail = q->used_idx = 0;
            }
            break;
        case 0x20: if (q) set_lo(q->desc, v); break;
        case 0x24: if (q) set_hi(q->desc, v); break;
        case 0x28: if (q) set_lo(q->driver, v); break;
        case 0x2C: if (q) set_hi(q->driver, v); break;
        case 0x30: if (q) set_lo(q->device, v); break;
        case 0x34: if (q) set_hi(q->device, v); break;
        default: break;
    }
}

bool VirtioPciDevice::QueueReady(uint16_t qi) const {
    return qi < queues_.size() && queues_[qi].enabled && driver_ok();
}

bool VirtioPciDevice::PopChain(uint16_t qi, VirtqChain* chain) {
    if (!QueueReady(qi)) return false;
    Queue& q = queues_[qi];
    uint16_t avail_idx;
    if (!mem_->Read(q.driver + 2, &avail_idx, 2)) return false;
    if (avail_idx == q.last_avail) return false;
    MemoryBarrier();
    uint16_t head;
    if (!mem_->Read(q.driver + 4 + 2ULL * (q.last_avail % q.size), &head, 2)) return false;
    q.last_avail++;

    chain->head = head;
    chain->bufs.clear();
    uint16_t idx = head;
    for (uint32_t n = 0; n < q.size; ++n) {
        if (idx >= q.size) {
            LogOnce("virtq-bad-desc", "virtio: descriptor index %u out of range", idx);
            return false;
        }
        uint8_t d[16];
        if (!mem_->Read(q.desc + 16ULL * idx, d, 16)) return false;
        VirtqBuffer b;
        uint16_t flags, next;
        std::memcpy(&b.gpa, d, 8);
        std::memcpy(&b.len, d + 8, 4);
        std::memcpy(&flags, d + 12, 2);
        std::memcpy(&next, d + 14, 2);
        if (flags & 4) {
            LogOnce("virtq-indirect", "virtio: indirect descriptor used but not negotiated");
            return false;
        }
        b.device_writable = (flags & 2) != 0;
        chain->bufs.push_back(b);
        if (!(flags & 1)) return true;
        idx = next;
    }
    LogOnce("virtq-loop", "virtio: descriptor chain loop detected");
    return false;
}

void VirtioPciDevice::PushUsed(uint16_t qi, uint16_t head, uint32_t written) {
    Queue& q = queues_[qi];
    uint32_t elem[2] = {head, written};
    mem_->Write(q.device + 4 + 8ULL * (q.used_idx % q.size), elem, 8);
    MemoryBarrier();
    q.used_idx++;
    mem_->Write(q.device + 2, &q.used_idx, 2);
    MemoryBarrier();
}

void VirtioPciDevice::NotifyQueue(uint16_t qi) {
    uint16_t flags = 0;
    mem_->Read(queues_[qi].driver, &flags, 2);
    if (flags & 1) return;  // VIRTQ_AVAIL_F_NO_INTERRUPT
    isr_ |= 1;
    SetIntx(true);
}

void VirtioPciDevice::NotifyConfigChange() {
    config_generation_++;
    isr_ |= 2;
    SetIntx(true);
}

std::vector<uint8_t> VirtioPciDevice::GatherReadable(const VirtqChain& c) const {
    std::vector<uint8_t> out;
    for (const auto& b : c.bufs) {
        if (b.device_writable) continue;
        size_t at = out.size();
        out.resize(at + b.len);
        if (!mem_->Read(b.gpa, out.data() + at, b.len)) out.resize(at);
    }
    return out;
}

uint32_t VirtioPciDevice::ScatterWritable(const VirtqChain& c, const void* data, uint32_t len, uint32_t skip) const {
    const uint8_t* src = static_cast<const uint8_t*>(data);
    uint32_t done = 0;
    for (const auto& b : c.bufs) {
        if (!b.device_writable) continue;
        uint32_t off = 0;
        if (skip) {
            uint32_t s = skip < b.len ? skip : b.len;
            skip -= s;
            off = s;
            if (off == b.len) continue;
        }
        uint32_t n = b.len - off;
        if (n > len - done) n = len - done;
        if (n == 0) break;
        mem_->Write(b.gpa + off, src + done, n);
        done += n;
    }
    return done;
}

}  // namespace Aetheron
