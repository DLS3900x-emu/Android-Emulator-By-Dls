// Minimal .zip reader (stored and deflated entries, zip64) used to unpack
// .xapk app bundles. Entries are streamed, so large files never have to fit
// in memory; sizes and CRC-32 are verified.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace Aetheron {

struct ZipEntry {
    std::string name;  // as stored, '/' separated
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t crc = 0;
    uint64_t comp_size = 0, size = 0, header_offset = 0;
};

class ZipReader {
public:
    using Sink = std::function<bool(const uint8_t*, size_t)>;

    ~ZipReader() { Close(); }
    bool Open(const std::wstring& path, std::string* error);
    void Close();
    const std::vector<ZipEntry>& entries() const { return entries_; }

    // Streams the entry's uncompressed bytes to `sink`.
    bool Extract(const ZipEntry& e, const Sink& sink, std::string* error);
    bool ExtractToFile(const ZipEntry& e, const std::wstring& dst, std::string* error);

private:
    FILE* f_ = nullptr;
    std::vector<ZipEntry> entries_;
};

}  // namespace Aetheron
