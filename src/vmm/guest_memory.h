// Guest physical RAM: one host allocation split into a low region (below the
// 32-bit MMIO hole) and a high region starting at 4 GiB.
#pragma once
#include <cstdint>
#include <cstring>

namespace Aetheron {

class GuestMemory {
public:
    // Start of the 32-bit MMIO hole (PCI BARs, IOAPIC, LAPIC). Nothing but
    // devices lives between kLowRamEnd and 4 GiB.
    static constexpr uint64_t kLowRamLimit = 0xC0000000ULL;
    static constexpr uint64_t kHighRamBase = 0x100000000ULL;

    bool Allocate(uint64_t total_bytes);
    void Free();

    uint64_t total() const { return total_; }
    uint64_t low_size() const { return low_size_; }
    uint64_t high_size() const { return total_ - low_size_; }
    uint8_t* low_host() const { return host_; }
    uint8_t* high_host() const { return host_ + low_size_; }

    // Returns host pointer for [gpa, gpa+len) if fully inside one RAM region.
    void* Translate(uint64_t gpa, uint64_t len = 1) const;
    bool Read(uint64_t gpa, void* dst, uint64_t len) const;
    bool Write(uint64_t gpa, const void* src, uint64_t len) const;

private:
    uint8_t* host_ = nullptr;
    uint64_t total_ = 0;
    uint64_t low_size_ = 0;
};

}  // namespace Aetheron
