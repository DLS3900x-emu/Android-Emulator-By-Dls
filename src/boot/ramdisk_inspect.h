// Looks inside the boot ramdisks (LZ4-legacy or plain "newc" cpio) to find
// which /fstab.<suffix> files exist, so androidboot.fstab_suffix can be set
// the way Cuttlefish's launcher does (cf.<userdata fs>.<encryption mode>).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Aetheron {

// Returns every file path in the (possibly concatenated) ramdisk archives.
// `format` receives "lz4-legacy", "cpio" or "unknown".
std::vector<std::string> ListRamdiskFiles(const std::vector<uint8_t>& ramdisk, std::string* format);

// "f2fs", "ext4" or "" from the superblock of a raw partition image.
std::string DetectFilesystem(const std::wstring& image_path);

// Picks the suffix of the fstab.* file matching the userdata filesystem;
// empty if the ramdisk has no fstab.* file.
std::string ChooseFstabSuffix(const std::vector<std::string>& files, const std::string& userdata_fs);

}  // namespace Aetheron
