// Parsing of Android boot images (header v3/v4): boot.img, init_boot.img and
// vendor_boot.img, and assembly of the initrd the way an Android bootloader
// does it: vendor ramdisk(s) + generic ramdisk + bootconfig trailer.
// Reference: AOSP system/tools/mkbootimg/include/bootimg/bootimg.h and
// https://source.android.com/docs/core/architecture/bootloader/partitions/vendor-boot-partitions
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Aetheron {

struct AndroidBootFiles {
    std::vector<uint8_t> kernel;          // from boot.img
    std::vector<uint8_t> generic_ramdisk; // from init_boot.img (or boot.img if no init_boot)
    std::vector<uint8_t> vendor_ramdisk;  // concatenated vendor ramdisks (normal-boot ones)
    std::string boot_cmdline;             // boot.img header cmdline
    std::string vendor_cmdline;           // vendor_boot header cmdline
    std::string vendor_bootconfig;        // vendor_boot bootconfig section
    uint32_t boot_header_version = 0;
    uint32_t vendor_header_version = 0;
};

bool LoadAndroidBootImages(const std::wstring& boot_img, const std::wstring& init_boot_img,
                           const std::wstring& vendor_boot_img, AndroidBootFiles* out, std::string* error);

// Builds the final initrd: vendor ramdisk + generic ramdisk + bootconfig
// block ("params", size, checksum, "#BOOTCONFIG\n") as parsed by the
// kernel's init/main.c get_boot_config_from_initrd().
std::vector<uint8_t> BuildInitrdWithBootconfig(const AndroidBootFiles& f, const std::string& bootconfig_params);

}  // namespace Aetheron
