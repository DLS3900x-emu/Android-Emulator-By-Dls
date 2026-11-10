// x86 Linux boot protocol (Documentation/arch/x86/boot.rst), 32-bit entry.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../vmm/guest_memory.h"

namespace Aetheron {

struct LinuxBootLayout {
    static constexpr uint64_t kGdtGpa = 0x1000;
    static constexpr uint64_t kBootParamsGpa = 0x10000;
    static constexpr uint64_t kCmdlineGpa = 0x20000;
    static constexpr uint64_t kMpTableGpa = 0xF0000;
    static constexpr uint64_t kKernelGpa = 0x100000;
};

struct LinuxBootResult {
    uint64_t entry_gpa = 0;        // code32_start
    uint64_t boot_params_gpa = 0;
    uint64_t initrd_gpa = 0;
    uint64_t initrd_size = 0;
};

bool LoadLinux(GuestMemory& mem, const std::vector<uint8_t>& bzimage, const std::vector<uint8_t>& initrd,
               const std::string& cmdline, LinuxBootResult* out, std::string* error);

}  // namespace Aetheron
