#include "ioapic.h"

#include "../common/log.h"

namespace Aetheron {

namespace {
constexpr uint64_t kMasked = 1ULL << 16;
constexpr uint64_t kLevel = 1ULL << 15;
constexpr uint64_t kRemoteIrr = 1ULL << 14;
constexpr uint64_t kDelivStatus = 1ULL << 12;
// Bits the guest may write in a redirection entry (Remote IRR and delivery
// status are read-only).
constexpr uint64_t kWritableMask = ~(kRemoteIrr | kDelivStatus);
}  // namespace

IoApic::IoApic(uint8_t id, Deliver deliver) : deliver_(std::move(deliver)), id_(id) {
    for (auto& r : redir_) r = kMasked;
}

uint64_t IoApic::MmioRead(uint64_t offset, uint8_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (offset) {
        case 0x00:
            return regsel_;
        case 0x10:
            return ReadReg(regsel_);
        default:
            LogOnce("ioapic-rd-" + std::to_string(offset), "IOAPIC: read of unimplemented offset 0x%llx",
                    (unsigned long long)offset);
            return 0;
    }
}

void IoApic::MmioWrite(uint64_t offset, uint8_t size, uint64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (offset) {
        case 0x00:
            regsel_ = static_cast<uint8_t>(value);
            break;
        case 0x10:
            WriteReg(regsel_, static_cast<uint32_t>(value));
            break;
        case 0x40: {  // EOI register (version 0x20)
            uint8_t vector = static_cast<uint8_t>(value);
            for (int pin = 0; pin < kPins; ++pin) {
                if ((redir_[pin] & 0xFF) == vector && (redir_[pin] & kLevel)) {
                    redir_[pin] &= ~kRemoteIrr;
                    ServicePin(pin);
                }
            }
            break;
        }
        default:
            LogOnce("ioapic-wr-" + std::to_string(offset), "IOAPIC: write to unimplemented offset 0x%llx",
                    (unsigned long long)offset);
    }
}

uint32_t IoApic::ReadReg(uint8_t index) {
    if (index == 0x00) return uint32_t(id_) << 24;
    if (index == 0x01) return (uint32_t(kPins - 1) << 16) | 0x20;
    if (index == 0x02) return uint32_t(id_) << 24;
    if (index >= 0x10 && index < 0x10 + kPins * 2) {
        int pin = (index - 0x10) / 2;
        uint64_t v = redir_[pin];
        return (index & 1) ? uint32_t(v >> 32) : uint32_t(v);
    }
    return 0;
}

void IoApic::WriteReg(uint8_t index, uint32_t value) {
    if (index == 0x00) {
        id_ = (value >> 24) & 0x0F;
        return;
    }
    if (index >= 0x10 && index < 0x10 + kPins * 2) {
        int pin = (index - 0x10) / 2;
        uint64_t old = redir_[pin];
        uint64_t nv;
        if (index & 1)
            nv = (old & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
        else
            nv = (old & 0xFFFFFFFF00000000ULL) | value;
        redir_[pin] = (nv & kWritableMask) | (old & kRemoteIrr);
        if (!(redir_[pin] & kLevel)) redir_[pin] &= ~kRemoteIrr;
        // Unmasking a level pin whose line is asserted must deliver.
        if ((redir_[pin] & kLevel) && line_[pin]) ServicePin(pin);
        return;
    }
}

void IoApic::ServicePin(int pin) {
    uint64_t e = redir_[pin];
    if (e & kMasked) return;
    if (!line_[pin]) return;
    bool level = (e & kLevel) != 0;
    if (level && (e & kRemoteIrr)) return;  // waiting for EOI
    ApicMessage m;
    m.vector = static_cast<uint8_t>(e & 0xFF);
    m.delivery_mode = static_cast<uint8_t>((e >> 8) & 7);
    m.logical_dest = ((e >> 11) & 1) != 0;
    m.level_triggered = level;
    m.destination = static_cast<uint8_t>(e >> 56);
    if (level) redir_[pin] |= kRemoteIrr;
    if (deliver_) deliver_(m);
}

void IoApic::SetIrqLevel(uint32_t gsi, bool level) {
    if (gsi >= kPins) return;
    std::lock_guard<std::mutex> lock(mutex_);
    bool was = line_[gsi];
    line_[gsi] = level;
    bool is_level = (redir_[gsi] & kLevel) != 0;
    if (is_level) {
        if (level) ServicePin(gsi);
    } else if (level && !was) {
        ServicePin(gsi);  // rising edge
    }
}

void IoApic::OnEoi(uint8_t vector) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int pin = 0; pin < kPins; ++pin) {
        if ((redir_[pin] & kLevel) && (redir_[pin] & kRemoteIrr) && (redir_[pin] & 0xFF) == vector) {
            redir_[pin] &= ~kRemoteIrr;
            ServicePin(pin);
        }
    }
}

}  // namespace Aetheron
