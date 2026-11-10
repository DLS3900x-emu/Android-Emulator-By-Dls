#include "pci.h"

#include <cstring>

#include "../common/log.h"

namespace Aetheron {

uint32_t PciDevice::bar0_address() const {
    uint32_t v;
    std::memcpy(&v, config + 0x10, 4);
    return v & ~0xFu;
}

uint32_t PciDevice::ConfigRead(uint32_t reg, uint8_t size) {
    if (reg >= 0x10 && reg < 0x14 && bar0_sizing_) {
        uint32_t mask = ~(Bar0Size() - 1);  // memory, 32-bit, non-prefetchable
        return (mask >> ((reg - 0x10) * 8)) & (size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1);
    }
    if (reg + size > 256) return 0xFFFFFFFF;
    uint32_t v = 0;
    std::memcpy(&v, config + reg, size);
    return v;
}

void PciDevice::ConfigWrite(uint32_t reg, uint8_t size, uint32_t value) {
    if (reg == 0x10 && size == 4) {
        if (value == 0xFFFFFFFF) {
            bar0_sizing_ = true;
        } else {
            bar0_sizing_ = false;
            uint32_t addr = value & ~(Bar0Size() - 1) & ~0xFu;
            std::memcpy(config + 0x10, &addr, 4);
        }
        return;
    }
    if (reg >= 0x14 && reg < 0x28) return;  // unimplemented BARs read as 0
    if (reg == 0x30) return;               // no expansion ROM
    if (reg == 0x04) {
        // command: I/O, memory, bus master, INTx disable are writable
        uint16_t cmd = uint16_t(value) & 0x0407;
        std::memcpy(config + 4, &cmd, 2);
        if (size == 4) {
            // status: write-1-to-clear bits; we never set error bits
        }
        return;
    }
    if (reg == 0x06) return;  // status
    if (reg == 0x0C || reg == 0x0D || reg == 0x3C) {
        for (uint8_t i = 0; i < size && reg + i < 256; ++i) config[reg + i] = uint8_t(value >> (8 * i));
        return;
    }
    // Everything else (IDs, class, capabilities) is read-only.
}

void PciDevice::SetIntx(bool level) {
    if (config[5] & 0x04) level = false;  // INTx disable
    // status bit 3: interrupt status
    if (level)
        config[6] |= 0x08;
    else
        config[6] &= ~0x08;
    if (irq_) irq_->SetIrqLevel(gsi_, level);
}

void PciBus::AddDevice(uint8_t slot, PciDevice* dev, uint32_t gsi) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.push_back(std::make_unique<IntxSource>(this, slot));
    dev->SetIrq(sources_.back().get(), gsi);
    dev->config[0x3C] = uint8_t(gsi);
    dev->config[0x3D] = 1;  // INTA#
    uint32_t size = dev->Bar0Size();
    next_bar_ = (next_bar_ + size - 1) & ~uint64_t(size - 1);
    uint32_t addr = uint32_t(next_bar_);
    std::memcpy(dev->config + 0x10, &addr, 4);
    next_bar_ += size;
    devices_[slot] = dev;
    Log("PCI: 00:%02x.0 vendor %02x%02x device %02x%02x BAR0=0x%08x size 0x%x INTA->GSI %u", slot,
        dev->config[1], dev->config[0], dev->config[3], dev->config[2], addr, size, gsi);
}

void PciBus::SetDeviceIntx(uint8_t slot, uint32_t gsi, bool level) {
    std::lock_guard<std::mutex> lock(intx_mutex_);
    uint32_t& mask = asserted_[gsi];
    uint32_t before = mask;
    if (level)
        mask |= 1u << slot;
    else
        mask &= ~(1u << slot);
    if (irq_ && (mask != 0) != (before != 0)) irq_->SetIrqLevel(gsi, mask != 0);
    else if (irq_ && level) irq_->SetIrqLevel(gsi, true);  // re-assert: lets a level pin re-evaluate
}

uint32_t PciBus::PortRead(uint16_t port, uint8_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (port == 0xCF8 && size == 4) return address_;
    if (port >= 0xCFC && port <= 0xCFF) {
        if (!(address_ & 0x80000000u)) return 0xFFFFFFFF;
        uint8_t bus = (address_ >> 16) & 0xFF, slot = (address_ >> 11) & 0x1F, fn = (address_ >> 8) & 7;
        uint32_t reg = (address_ & 0xFC) + (port - 0xCFC);
        if (bus != 0 || fn != 0) return 0xFFFFFFFF;
        if (slot == 0) {
            if (!host_bridge_init_) {
                host_bridge_[0] = 0x86; host_bridge_[1] = 0x80;  // Intel
                host_bridge_[2] = 0x37; host_bridge_[3] = 0x12;  // 82441FX host bridge
                host_bridge_[0x0B] = 0x06;                     // class: bridge / host
                host_bridge_init_ = true;
            }
            uint32_t v = 0;
            if (reg + size <= 256) std::memcpy(&v, host_bridge_ + reg, size);
            return v;
        }
        auto it = devices_.find(slot);
        if (it == devices_.end()) return 0xFFFFFFFF;
        return it->second->ConfigRead(reg, size);
    }
    return 0xFFFFFFFF;
}

void PciBus::PortWrite(uint16_t port, uint8_t size, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (port == 0xCF8 && size == 4) {
        address_ = value;
        return;
    }
    if (port >= 0xCFC && port <= 0xCFF) {
        if (!(address_ & 0x80000000u)) return;
        uint8_t bus = (address_ >> 16) & 0xFF, slot = (address_ >> 11) & 0x1F, fn = (address_ >> 8) & 7;
        uint32_t reg = (address_ & 0xFC) + (port - 0xCFC);
        if (bus != 0 || fn != 0 || slot == 0) return;
        auto it = devices_.find(slot);
        if (it != devices_.end()) it->second->ConfigWrite(reg, size, value);
    }
}

bool PciBus::HandleMmio(uint64_t gpa, bool write, uint8_t size, uint64_t* value) {
    PciDevice* target = nullptr;
    uint64_t offset = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : devices_) {
            PciDevice* d = kv.second;
            uint64_t base = d->bar0_address();
            if (base && d->memory_enabled() && gpa >= base && gpa < base + d->Bar0Size()) {
                target = d;
                offset = gpa - base;
                break;
            }
        }
    }
    if (!target) return false;
    if (write)
        target->BarWrite(offset, size, *value);
    else
        *value = target->BarRead(offset, size);
    return true;
}

}  // namespace Aetheron
