#include "guest_memory.h"

#include <windows.h>

namespace Aetheron {

bool GuestMemory::Allocate(uint64_t total_bytes) {
    Free();
    host_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, total_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!host_) return false;
    total_ = total_bytes;
    low_size_ = total_bytes < kLowRamLimit ? total_bytes : kLowRamLimit;
    return true;
}

void GuestMemory::Free() {
    if (host_) VirtualFree(host_, 0, MEM_RELEASE);
    host_ = nullptr;
    total_ = low_size_ = 0;
}

void* GuestMemory::Translate(uint64_t gpa, uint64_t len) const {
    if (!host_ || len == 0) return nullptr;
    if (gpa < low_size_) {
        if (gpa + len > low_size_ || gpa + len < gpa) return nullptr;
        return host_ + gpa;
    }
    if (gpa >= kHighRamBase) {
        uint64_t off = gpa - kHighRamBase;
        if (off + len > high_size() || off + len < off) return nullptr;
        return host_ + low_size_ + off;
    }
    return nullptr;
}

bool GuestMemory::Read(uint64_t gpa, void* dst, uint64_t len) const {
    void* p = Translate(gpa, len);
    if (!p) return false;
    std::memcpy(dst, p, len);
    return true;
}

bool GuestMemory::Write(uint64_t gpa, const void* src, uint64_t len) const {
    void* p = Translate(gpa, len);
    if (!p) return false;
    std::memcpy(p, src, len);
    return true;
}

}  // namespace Aetheron
