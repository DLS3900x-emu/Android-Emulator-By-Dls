// Reads aetheron_disk.cfg and prepares the partition backing files.
//
// Line format:   <partition-name>  <source>  [options...]
//   source:  an image file name (relative to the images folder), or
//            blank:<size>   a zero-filled partition (e.g. blank:16M)
//   options: rw              guest may write (sparse/blank images are always
//                            expanded into cache\ and are writable)
//            min=<size>      grow the expanded cache file to at least <size>
//            ifmissing=blank:<size>  use a blank partition if the file is absent
// Sizes accept K, M, G suffixes.
#pragma once
#include <string>
#include <vector>

#include "composite_disk.h"

namespace Aetheron {

bool PrepareDiskPartitions(const std::wstring& config_path, const std::wstring& images_dir,
                           const std::wstring& cache_dir, std::vector<PartitionSpec>* out, std::string* error);

}  // namespace Aetheron
