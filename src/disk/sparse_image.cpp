#include "sparse_image.h"

#include <windows.h>
#include <winioctl.h>

#include <vector>

#include "../common/log.h"

namespace Aetheron {

namespace {
constexpr uint32_t kMagic = 0xED26FF3A;
constexpr uint16_t kChunkRaw = 0xCAC1, kChunkFill = 0xCAC2, kChunkDontCare = 0xCAC3, kChunkCrc = 0xCAC4;

#pragma pack(push, 1)
struct SparseHeader {
    uint32_t magic;
    uint16_t major, minor;
    uint16_t file_hdr_sz, chunk_hdr_sz;
    uint32_t blk_sz, total_blks, total_chunks, image_checksum;
};
struct ChunkHeader {
    uint16_t type, reserved;
    uint32_t chunk_sz, total_sz;
};
#pragma pack(pop)

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

bool ReadExact(HANDLE h, void* buf, uint32_t len) {
    DWORD got = 0;
    return ReadFile(h, buf, len, &got, nullptr) && got == len;
}

bool ReadHeader(HANDLE h, SparseHeader* hdr) {
    if (!ReadExact(h, hdr, sizeof(*hdr))) return false;
    if (hdr->magic != kMagic || hdr->major != 1 || hdr->file_hdr_sz < sizeof(SparseHeader) ||
        hdr->chunk_hdr_sz < sizeof(ChunkHeader) || hdr->blk_sz == 0 || hdr->blk_sz % 4)
        return false;
    LARGE_INTEGER skip;
    skip.QuadPart = hdr->file_hdr_sz - sizeof(SparseHeader);
    return SetFilePointerEx(h, skip, nullptr, FILE_CURRENT) != 0;
}
}  // namespace

bool IsAndroidSparse(const std::wstring& path) {
    Handle f;
    f.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) return false;
    uint32_t magic = 0;
    return ReadExact(f.h, &magic, 4) && magic == kMagic;
}

uint64_t SparseExpandedSize(const std::wstring& path) {
    Handle f;
    f.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    SparseHeader hdr;
    if (f.h == INVALID_HANDLE_VALUE || !ReadHeader(f.h, &hdr)) return 0;
    return uint64_t(hdr.total_blks) * hdr.blk_sz;
}

bool UnsparseImage(const std::wstring& src, const std::wstring& dst) {
    Handle in, out;
    in.h = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                       nullptr);
    if (in.h == INVALID_HANDLE_VALUE) return false;
    SparseHeader hdr;
    if (!ReadHeader(in.h, &hdr)) {
        Log("sparse: bad header in %ls", src.c_str());
        return false;
    }
    std::wstring tmp = dst + L".partial";
    out.h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (out.h == INVALID_HANDLE_VALUE) {
        Log("sparse: cannot create %ls (error %lu)", tmp.c_str(), GetLastError());
        return false;
    }
    DWORD ret;
    DeviceIoControl(out.h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);

    std::vector<uint8_t> buf(8u << 20);
    uint64_t out_pos = 0;
    for (uint32_t i = 0; i < hdr.total_chunks; ++i) {
        ChunkHeader ch;
        if (!ReadExact(in.h, &ch, sizeof(ch))) return false;
        LARGE_INTEGER skip;
        skip.QuadPart = hdr.chunk_hdr_sz - sizeof(ChunkHeader);
        if (skip.QuadPart) SetFilePointerEx(in.h, skip, nullptr, FILE_CURRENT);
        uint64_t bytes = uint64_t(ch.chunk_sz) * hdr.blk_sz;
        uint64_t payload = ch.total_sz - hdr.chunk_hdr_sz;
        LARGE_INTEGER pos;
        pos.QuadPart = int64_t(out_pos);
        SetFilePointerEx(out.h, pos, nullptr, FILE_BEGIN);
        switch (ch.type) {
            case kChunkRaw: {
                if (payload != bytes) return false;
                uint64_t left = bytes;
                while (left) {
                    uint32_t n = uint32_t(left < buf.size() ? left : buf.size());
                    DWORD w;
                    if (!ReadExact(in.h, buf.data(), n) || !WriteFile(out.h, buf.data(), n, &w, nullptr) || w != n)
                        return false;
                    left -= n;
                }
                break;
            }
            case kChunkFill: {
                uint32_t pattern;
                if (payload != 4 || !ReadExact(in.h, &pattern, 4)) return false;
                if (pattern == 0) break;  // leave as a hole (reads as zero)
                for (size_t k = 0; k < buf.size() / 4; ++k) reinterpret_cast<uint32_t*>(buf.data())[k] = pattern;
                uint64_t left = bytes;
                while (left) {
                    uint32_t n = uint32_t(left < buf.size() ? left : buf.size());
                    DWORD w;
                    if (!WriteFile(out.h, buf.data(), n, &w, nullptr) || w != n) return false;
                    left -= n;
                }
                break;
            }
            case kChunkDontCare:
                break;
            case kChunkCrc: {
                skip.QuadPart = int64_t(payload);
                SetFilePointerEx(in.h, skip, nullptr, FILE_CURRENT);
                bytes = 0;
                break;
            }
            default:
                Log("sparse: unknown chunk type 0x%x in %ls", ch.type, src.c_str());
                return false;
        }
        out_pos += bytes;
    }
    LARGE_INTEGER end;
    end.QuadPart = int64_t(uint64_t(hdr.total_blks) * hdr.blk_sz);
    SetFilePointerEx(out.h, end, nullptr, FILE_BEGIN);
    SetEndOfFile(out.h);
    CloseHandle(out.h);
    out.h = INVALID_HANDLE_VALUE;
    DeleteFileW(dst.c_str());
    return MoveFileW(tmp.c_str(), dst.c_str()) != 0;
}

}  // namespace Aetheron
