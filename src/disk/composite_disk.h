// A GPT-partitioned virtual disk assembled from separate image files, in the
// style of Cuttlefish's "os_composite" disk. Partition contents are read from
// (and written to) the backing files directly; the protective MBR and the
// primary/backup GPT structures are generated in memory.
#pragma once
#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../devices/virtio_blk.h"

namespace Aetheron {

struct PartitionSpec {
    std::string name;
    std::wstring file;      // backing file
    bool writable = false;
};

class CompositeDisk : public BlockBackend {
public:
    ~CompositeDisk() override;
    bool Build(const std::vector<PartitionSpec>& parts);

    uint64_t SizeBytes() const override { return total_bytes_; }
    bool Read(uint64_t offset, void* buf, uint32_t len) override;
    bool Write(uint64_t offset, const void* buf, uint32_t len) override;
    bool Flush() override;

private:
    struct Extent {
        std::string name;
        uint64_t start = 0, size = 0;  // byte offsets on the virtual disk
        HANDLE file = INVALID_HANDLE_VALUE;
        bool writable = false;
    };
    bool Access(uint64_t offset, uint8_t* buf, uint32_t len, bool write);

    std::vector<Extent> extents_;
    std::vector<uint8_t> head_;  // LBA 0..2047 (MBR, GPT header, entries)
    std::vector<uint8_t> tail_;  // backup entries + header
    uint64_t tail_start_ = 0;
    uint64_t total_bytes_ = 0;
    std::mutex mutex_;
};

}  // namespace Aetheron
