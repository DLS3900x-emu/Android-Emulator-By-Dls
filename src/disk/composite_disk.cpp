#include "composite_disk.h"

#include <cstring>

#include "../common/log.h"

namespace Aetheron {

namespace {
constexpr uint64_t kSector = 512;
constexpr uint64_t kAlign = 1ULL << 20;  // partitions start on 1 MiB boundaries
constexpr uint32_t kEntries = 128, kEntrySize = 128;
constexpr uint64_t kEntryBytes = kEntries * kEntrySize;  // 16 KiB = 32 sectors

uint32_t Crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Deterministic version-4-style GUID derived from a string (FNV-1a based).
void NameGuid(const std::string& s, uint8_t out[16]) {
    uint64_t h1 = 1469598103934665603ULL, h2 = 0x9E3779B97F4A7C15ULL;
    for (unsigned char ch : s) {
        h1 = (h1 ^ ch) * 1099511628211ULL;
        h2 = (h2 ^ ch) * 0x100000001B3ULL + 0x7F4A7C15;
    }
    std::memcpy(out, &h1, 8);
    std::memcpy(out + 8, &h2, 8);
    out[7] = (out[7] & 0x0F) | 0x40;  // version 4 (byte 7 is the high byte of field 3, little-endian)
    out[8] = (out[8] & 0x3F) | 0x80;  // RFC 4122 variant
}

// Linux filesystem data: 0FC63DAF-8483-4772-8E79-3D69D8477DE4 (mixed-endian on disk)
const uint8_t kLinuxDataGuid[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};

void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void Put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

void MakeHeader(uint8_t* h, uint64_t my_lba, uint64_t alt_lba, uint64_t first_usable, uint64_t last_usable,
                const uint8_t disk_guid[16], uint64_t entries_lba, uint32_t entries_crc) {
    std::memset(h, 0, kSector);
    std::memcpy(h, "EFI PART", 8);
    Put32(h + 8, 0x00010000);
    Put32(h + 12, 92);
    Put64(h + 24, my_lba);
    Put64(h + 32, alt_lba);
    Put64(h + 40, first_usable);
    Put64(h + 48, last_usable);
    std::memcpy(h + 56, disk_guid, 16);
    Put64(h + 72, entries_lba);
    Put32(h + 80, kEntries);
    Put32(h + 84, kEntrySize);
    Put32(h + 88, entries_crc);
    Put32(h + 16, Crc32(h, 92));
}
}  // namespace

CompositeDisk::~CompositeDisk() {
    for (auto& e : extents_)
        if (e.file != INVALID_HANDLE_VALUE) CloseHandle(e.file);
}

bool CompositeDisk::Build(const std::vector<PartitionSpec>& parts) {
    if (parts.empty() || parts.size() > kEntries) return false;
    uint64_t pos = kAlign;
    for (const auto& p : parts) {
        Extent e;
        e.name = p.name;
        e.writable = p.writable;
        e.file = CreateFileW(p.file.c_str(), GENERIC_READ | (p.writable ? GENERIC_WRITE : 0),
                             FILE_SHARE_READ | (p.writable ? 0 : FILE_SHARE_WRITE), nullptr, OPEN_EXISTING,
                             FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (e.file == INVALID_HANDLE_VALUE) {
            Log("disk: cannot open %ls for partition '%s' (error %lu)", p.file.c_str(), p.name.c_str(),
                GetLastError());
            return false;
        }
        LARGE_INTEGER sz;
        GetFileSizeEx(e.file, &sz);
        e.size = (uint64_t(sz.QuadPart) + kSector - 1) & ~(kSector - 1);
        if (e.size == 0) {
            Log("disk: partition '%s' backing file is empty", p.name.c_str());
            return false;
        }
        e.start = pos;
        pos = (pos + e.size + kAlign - 1) & ~(kAlign - 1);
        Log("disk: %-22s LBA %10llu  %8llu KiB  %s  <- %ls", e.name.c_str(),
            (unsigned long long)(e.start / kSector), (unsigned long long)(e.size >> 10), e.writable ? "rw" : "ro",
            p.file.c_str());
        extents_.push_back(e);
    }
    // Backup GPT: 32 sectors of entries + 1 header sector at the very end.
    total_bytes_ = pos + kEntryBytes + kSector;
    uint64_t last_lba = total_bytes_ / kSector - 1;
    uint64_t backup_entries_lba = last_lba - kEntryBytes / kSector;
    uint64_t first_usable = 2 + kEntryBytes / kSector;  // 34
    uint64_t last_usable = backup_entries_lba - 1;

    std::vector<uint8_t> entries(kEntryBytes, 0);
    for (size_t i = 0; i < extents_.size(); ++i) {
        uint8_t* e = entries.data() + i * kEntrySize;
        std::memcpy(e, kLinuxDataGuid, 16);
        NameGuid("aetheron-part-" + extents_[i].name, e + 16);
        Put64(e + 32, extents_[i].start / kSector);
        Put64(e + 40, (extents_[i].start + extents_[i].size) / kSector - 1);
        for (size_t c = 0; c < extents_[i].name.size() && c < 36; ++c) e[56 + c * 2] = uint8_t(extents_[i].name[c]);
    }
    uint32_t entries_crc = Crc32(entries.data(), entries.size());
    uint8_t disk_guid[16];
    NameGuid("aetheron-os-disk", disk_guid);

    head_.assign(kAlign, 0);
    uint8_t* mbr = head_.data();
    uint8_t* pe = mbr + 446;
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;  // CHS of LBA 1
    pe[4] = 0xEE;                              // GPT protective
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;
    Put32(pe + 8, 1);
    Put32(pe + 12, last_lba > 0xFFFFFFFFULL ? 0xFFFFFFFFu : uint32_t(last_lba));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    MakeHeader(head_.data() + kSector, 1, last_lba, first_usable, last_usable, disk_guid, 2, entries_crc);
    std::memcpy(head_.data() + 2 * kSector, entries.data(), entries.size());

    tail_start_ = backup_entries_lba * kSector;
    tail_.assign(kEntryBytes + kSector, 0);
    std::memcpy(tail_.data(), entries.data(), entries.size());
    MakeHeader(tail_.data() + kEntryBytes, last_lba, 1, first_usable, last_usable, disk_guid, backup_entries_lba,
               entries_crc);
    Log("disk: composite GPT disk ready, %zu partitions, %llu MiB", extents_.size(),
        (unsigned long long)(total_bytes_ >> 20));
    return true;
}

bool CompositeDisk::Access(uint64_t offset, uint8_t* buf, uint32_t len, bool write) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t end = offset + len;
    if (end > total_bytes_) return false;
    while (offset < end) {
        uint32_t n = uint32_t(end - offset);
        if (offset < head_.size()) {
            uint64_t lim = head_.size() - offset;
            if (n > lim) n = uint32_t(lim);
            if (write) {
                LogOnce("gpt-write", "disk: guest wrote to the partition table area (kept in memory only)");
                std::memcpy(head_.data() + offset, buf, n);
            } else {
                std::memcpy(buf, head_.data() + offset, n);
            }
        } else if (offset >= tail_start_) {
            if (write)
                std::memcpy(tail_.data() + (offset - tail_start_), buf, n);
            else
                std::memcpy(buf, tail_.data() + (offset - tail_start_), n);
        } else {
            const Extent* hit = nullptr;
            uint64_t next_start = tail_start_;
            for (const auto& e : extents_) {
                if (offset >= e.start && offset < e.start + e.size) {
                    hit = &e;
                    break;
                }
                if (e.start > offset && e.start < next_start) next_start = e.start;
            }
            if (hit) {
                uint64_t lim = hit->start + hit->size - offset;
                if (n > lim) n = uint32_t(lim);
                if (write && !hit->writable) {
                    LogOnce("ro-" + hit->name, "disk: guest write to read-only partition '%s' rejected",
                            hit->name.c_str());
                    return false;
                }
                OVERLAPPED ov = {};
                uint64_t fo = offset - hit->start;
                ov.Offset = DWORD(fo);
                ov.OffsetHigh = DWORD(fo >> 32);
                DWORD done = 0;
                BOOL ok = write ? WriteFile(hit->file, buf, n, &done, &ov) : ReadFile(hit->file, buf, n, &done, &ov);
                if (!ok && GetLastError() != ERROR_HANDLE_EOF) return false;
                // The last partial sector of an image reads as zeros.
                if (!write && done < n) std::memset(buf + done, 0, n - done);
            } else {
                // gap between partitions (alignment padding)
                uint64_t lim = next_start - offset;
                if (n > lim) n = uint32_t(lim);
                if (!write) std::memset(buf, 0, n);
            }
        }
        offset += n;
        buf += n;
    }
    return true;
}

bool CompositeDisk::Read(uint64_t offset, void* buf, uint32_t len) {
    return Access(offset, static_cast<uint8_t*>(buf), len, false);
}

bool CompositeDisk::Write(uint64_t offset, const void* buf, uint32_t len) {
    return Access(offset, static_cast<uint8_t*>(const_cast<void*>(buf)), len, true);
}

bool CompositeDisk::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : extents_)
        if (e.writable) FlushFileBuffers(e.file);
    return true;
}

}  // namespace Aetheron
