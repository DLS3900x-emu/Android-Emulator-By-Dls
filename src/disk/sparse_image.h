// Android sparse image (libsparse format, magic 0xED26FF3A) handling.
#pragma once
#include <cstdint>
#include <string>

namespace Aetheron {

bool IsAndroidSparse(const std::wstring& path);
// Expanded size of a sparse image in bytes (0 on error).
uint64_t SparseExpandedSize(const std::wstring& path);
// Expands `src` into raw file `dst` (created as an NTFS sparse file, so
// "don't care" regions use no disk space). Returns false on any error.
bool UnsparseImage(const std::wstring& src, const std::wstring& dst);

}  // namespace Aetheron
