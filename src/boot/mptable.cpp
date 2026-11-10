#include "mptable.h"

#include <intrin.h>

#include <cstring>

namespace Aetheron {

namespace {
uint8_t Checksum(const uint8_t* p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; ++i) s += p[i];
    return uint8_t(0 - s);
}
}  // namespace

uint32_t WriteMpTable(uint8_t* ram, uint64_t gpa, uint32_t num_cpus, uint8_t ioapic_id, uint64_t ioapic_addr,
                      const std::vector<MpPciIrq>& pci_irqs) {
    uint8_t* fp = ram + gpa;           // floating pointer structure (16 bytes)
    uint8_t* ct = fp + 16;             // configuration table
    uint32_t ct_gpa = uint32_t(gpa + 16);
    std::memset(fp, 0, 16);

    std::vector<uint8_t> e;  // entries
    int cpuid1[4];
    __cpuid(cpuid1, 1);
    for (uint32_t i = 0; i < num_cpus; ++i) {
        uint8_t p[20] = {};
        p[0] = 0;              // processor
        p[1] = uint8_t(i);     // local APIC id
        p[2] = 0x14;           // local APIC version
        p[3] = 0x01 | (i == 0 ? 0x02 : 0);  // enabled, BSP
        std::memcpy(p + 4, &cpuid1[0], 4);   // signature (family/model/stepping)
        std::memcpy(p + 8, &cpuid1[3], 4);   // feature flags
        e.insert(e.end(), p, p + 20);
    }
    const uint8_t kBusPci = 0, kBusIsa = 1;
    uint8_t bus_pci[8] = {1, kBusPci, 'P', 'C', 'I', ' ', ' ', ' '};
    uint8_t bus_isa[8] = {1, kBusIsa, 'I', 'S', 'A', ' ', ' ', ' '};
    e.insert(e.end(), bus_pci, bus_pci + 8);
    e.insert(e.end(), bus_isa, bus_isa + 8);
    uint8_t ioapic[8] = {2, ioapic_id, 0x20, 0x01};
    uint32_t ia = uint32_t(ioapic_addr);
    std::memcpy(ioapic + 4, &ia, 4);
    e.insert(e.end(), ioapic, ioapic + 8);

    auto io_int = [&](uint8_t type, uint16_t flags, uint8_t bus, uint8_t bus_irq, uint8_t pin) {
        uint8_t x[8] = {3, type, uint8_t(flags), uint8_t(flags >> 8), bus, bus_irq, ioapic_id, pin};
        e.insert(e.end(), x, x + 8);
    };
    // ISA: IRQ0 (PIT) -> pin 2, as on PC chipsets; other ISA IRQs identity
    // mapped. Flags 0 = conforms to bus (ISA: edge, active high).
    io_int(3, 0, kBusIsa, 0, 0);  // ExtINT from the 8259 on pin 0
    io_int(0, 0, kBusIsa, 0, 2);
    for (uint8_t irq = 1; irq < 16; ++irq) {
        if (irq == 2) continue;
        io_int(0, 0, kBusIsa, irq, irq);
    }
    // PCI INTA#: source bus irq = (device << 2) | pin(0=INTA); level, active low.
    for (const auto& p : pci_irqs) io_int(0, 0x000F, kBusPci, uint8_t(p.slot << 2), p.ioapic_pin);
    // Local interrupts: ExtINT -> LINT0, NMI -> LINT1 on all CPUs.
    uint8_t lint0[8] = {4, 3, 0, 0, kBusIsa, 0, 0xFF, 0};
    uint8_t lint1[8] = {4, 1, 0, 0, kBusIsa, 0, 0xFF, 1};
    e.insert(e.end(), lint0, lint0 + 8);
    e.insert(e.end(), lint1, lint1 + 8);

    uint16_t entry_count = uint16_t(num_cpus + 2 + 1 + 1 + 15 + pci_irqs.size() + 2);
    uint16_t base_len = uint16_t(44 + e.size());
    std::memset(ct, 0, 44);
    std::memcpy(ct, "PCMP", 4);
    std::memcpy(ct + 4, &base_len, 2);
    ct[6] = 4;  // spec revision 1.4
    std::memcpy(ct + 8, "AETHERON", 8);
    std::memcpy(ct + 16, "WHPX-VM     ", 12);
    std::memcpy(ct + 34, &entry_count, 2);
    uint32_t lapic = 0xFEE00000;
    std::memcpy(ct + 36, &lapic, 4);
    std::memcpy(ct + 44, e.data(), e.size());
    ct[7] = Checksum(ct, base_len);

    std::memcpy(fp, "_MP_", 4);
    std::memcpy(fp + 4, &ct_gpa, 4);
    fp[8] = 1;  // length in 16-byte units
    fp[9] = 4;  // spec revision
    fp[11] = 0; // feature byte 1: 0 = configuration table present
    fp[12] = 0; // IMCR not present (virtual wire mode)
    fp[10] = Checksum(fp, 16);
    return 16 + base_len;
}

}  // namespace Aetheron
