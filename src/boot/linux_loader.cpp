#include "linux_loader.h"

#include <cstring>

#include "../common/log.h"

namespace Aetheron {

namespace {
// Offsets inside struct boot_params / setup_header (arch/x86/include/uapi/asm/bootparam.h)
constexpr size_t kE820Entries = 0x1E8;
constexpr size_t kSetupHeader = 0x1F1;
constexpr size_t kSetupSects = 0x1F1;
constexpr size_t kBootFlag = 0x1FE;
constexpr size_t kHeaderMagic = 0x202;
constexpr size_t kVersion = 0x206;
constexpr size_t kTypeOfLoader = 0x210;
constexpr size_t kLoadflags = 0x211;
constexpr size_t kCode32Start = 0x214;
constexpr size_t kRamdiskImage = 0x218;
constexpr size_t kRamdiskSize = 0x21C;
constexpr size_t kHeapEndPtr = 0x224;
constexpr size_t kCmdLinePtr = 0x228;
constexpr size_t kInitrdAddrMax = 0x22C;
constexpr size_t kCmdlineSize = 0x238;
constexpr size_t kE820Table = 0x2D0;
constexpr size_t kBootParamsSize = 0x1000;

constexpr uint32_t kE820Ram = 1, kE820Reserved = 2;

uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t Rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
void Wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void Wr16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
}  // namespace

bool LoadLinux(GuestMemory& mem, const std::vector<uint8_t>& k, const std::vector<uint8_t>& initrd,
               const std::string& cmdline, LinuxBootResult* out, std::string* error) {
    if (k.size() < 0x1000 || Rd32(&k[kHeaderMagic]) != 0x53726448 /* HdrS */ || Rd16(&k[kBootFlag]) != 0xAA55) {
        *error = "kernel is not an x86 bzImage";
        return false;
    }
    uint16_t version = Rd16(&k[kVersion]);
    if (version < 0x020C) {
        *error = "boot protocol too old";
        return false;
    }
    uint32_t setup_sects = k[kSetupSects] ? k[kSetupSects] : 4;
    uint64_t setup_bytes = (uint64_t(setup_sects) + 1) * 512;
    if (setup_bytes >= k.size()) {
        *error = "bad setup_sects";
        return false;
    }
    uint64_t pm_size = k.size() - setup_bytes;
    uint8_t* ram = mem.low_host();
    uint64_t low_end = mem.low_size();

    // Protected-mode kernel at 1 MiB. It decompresses/relocates itself.
    if (LinuxBootLayout::kKernelGpa + pm_size > low_end) {
        *error = "kernel does not fit in RAM";
        return false;
    }
    std::memcpy(ram + LinuxBootLayout::kKernelGpa, k.data() + setup_bytes, pm_size);

    uint8_t* bp = ram + LinuxBootLayout::kBootParamsGpa;
    std::memset(bp, 0, kBootParamsSize);
    // Copy the setup header exactly as long as the kernel declares it.
    size_t hdr_end = 0x202 + k[0x201];
    std::memcpy(bp + kSetupHeader, k.data() + kSetupHeader, hdr_end - kSetupHeader);

    bp[kTypeOfLoader] = 0xFF;  // undefined/other bootloader
    bp[kLoadflags] |= 0x01 | 0x80;  // LOADED_HIGH | CAN_USE_HEAP
    Wr16(bp + kHeapEndPtr, 0xFE00);
    Wr32(bp + kCode32Start, uint32_t(LinuxBootLayout::kKernelGpa));

    uint32_t cmdline_max = Rd32(bp + kCmdlineSize);
    if (cmdline_max == 0) cmdline_max = 255;
    if (cmdline.size() > cmdline_max) {
        *error = "kernel command line too long (" + std::to_string(cmdline.size()) + " > " +
                 std::to_string(cmdline_max) + ")";
        return false;
    }
    std::memcpy(ram + LinuxBootLayout::kCmdlineGpa, cmdline.c_str(), cmdline.size() + 1);
    Wr32(bp + kCmdLinePtr, uint32_t(LinuxBootLayout::kCmdlineGpa));

    // initrd: as high as possible below initrd_addr_max and the MMIO hole.
    if (!initrd.empty()) {
        uint64_t max_addr = uint64_t(Rd32(bp + kInitrdAddrMax)) + 1;
        if (max_addr > low_end) max_addr = low_end;
        uint64_t addr = (max_addr - initrd.size()) & ~0xFFFFFULL;  // 1 MiB aligned
        uint64_t kernel_reserved_end = 0x10000000;  // keep 256 MiB for the decompressed kernel
        if (initrd.size() >= max_addr || addr < kernel_reserved_end) {
            *error = "initrd too large for low memory";
            return false;
        }
        std::memcpy(ram + addr, initrd.data(), initrd.size());
        Wr32(bp + kRamdiskImage, uint32_t(addr));
        Wr32(bp + kRamdiskSize, uint32_t(initrd.size()));
        out->initrd_gpa = addr;
        out->initrd_size = initrd.size();
    }

    // E820 map: exactly what is mapped as RAM.
    struct E820 {
        uint64_t addr, size;
        uint32_t type;
    };
    std::vector<E820> map = {
        {0x0, 0x9F000, kE820Ram},
        {0x9F000, 0x100000 - 0x9F000, kE820Reserved},  // EBDA, VGA hole, BIOS area (MP table)
        {0x100000, low_end - 0x100000, kE820Ram},
    };
    if (mem.high_size()) map.push_back({GuestMemory::kHighRamBase, mem.high_size(), kE820Ram});
    for (size_t i = 0; i < map.size(); ++i) {
        uint8_t* e = bp + kE820Table + i * 20;
        std::memcpy(e, &map[i].addr, 8);
        std::memcpy(e + 8, &map[i].size, 8);
        std::memcpy(e + 16, &map[i].type, 4);
    }
    bp[kE820Entries] = uint8_t(map.size());

    // Flat GDT for the 32-bit entry: 0x10 = code, 0x18 = data.
    uint64_t* gdt = reinterpret_cast<uint64_t*>(ram + LinuxBootLayout::kGdtGpa);
    gdt[0] = 0;
    gdt[1] = 0;
    gdt[2] = 0x00CF9B000000FFFFULL;
    gdt[3] = 0x00CF93000000FFFFULL;

    out->entry_gpa = LinuxBootLayout::kKernelGpa;
    out->boot_params_gpa = LinuxBootLayout::kBootParamsGpa;
    Log("linux: boot protocol %u.%02u, kernel %llu KiB at 0x%llx, initrd %llu KiB at 0x%llx, cmdline %zu bytes",
        version >> 8, version & 0xFF, (unsigned long long)(pm_size >> 10),
        (unsigned long long)LinuxBootLayout::kKernelGpa, (unsigned long long)(out->initrd_size >> 10),
        (unsigned long long)out->initrd_gpa, cmdline.size());
    return true;
}

}  // namespace Aetheron
