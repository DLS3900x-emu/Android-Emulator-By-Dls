// Intel MultiProcessor Specification 1.4 tables. They tell Linux where the
// IOAPIC is and how ISA and PCI interrupts are wired, without needing ACPI.
#pragma once
#include <cstdint>
#include <vector>

namespace Aetheron {

struct MpPciIrq {
    uint8_t slot;    // PCI device number on bus 0
    uint8_t ioapic_pin;
};

// Writes the floating pointer + configuration table at `gpa` (must be in
// 0xF0000-0xFFFFF). Returns the number of bytes used.
uint32_t WriteMpTable(uint8_t* ram, uint64_t gpa, uint32_t num_cpus, uint8_t ioapic_id, uint64_t ioapic_addr,
                      const std::vector<MpPciIrq>& pci_irqs);

}  // namespace Aetheron
