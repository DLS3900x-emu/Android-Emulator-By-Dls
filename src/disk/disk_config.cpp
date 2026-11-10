#include "disk_config.h"

#include <windows.h>
#include <winioctl.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "../common/log.h"
#include "sparse_image.h"
#include "../common/file_util.h"

namespace Aetheron {

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

bool ParseSize(const std::string& s, uint64_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    unsigned long long v = strtoull(s.c_str(), &end, 10);
    uint64_t mul = 1;
    if (*end == 'K' || *end == 'k') mul = 1ULL << 10;
    else if (*end == 'M' || *end == 'm') mul = 1ULL << 20;
    else if (*end == 'G' || *end == 'g') mul = 1ULL << 30;
    else if (*end != 0) return false;
    *out = v * mul;
    return *out > 0;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

uint64_t FileMtime(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

// Creates (or grows) an NTFS sparse file of at least `size` bytes.
bool EnsureFileSize(const std::wstring& path, uint64_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD ret;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
    LARGE_INTEGER cur;
    GetFileSizeEx(h, &cur);
    bool ok = true;
    if (uint64_t(cur.QuadPart) < size) {
        LARGE_INTEGER s;
        s.QuadPart = int64_t(size);
        ok = SetFilePointerEx(h, s, nullptr, FILE_BEGIN) && SetEndOfFile(h);
    }
    CloseHandle(h);
    return ok;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out->resize(n > 0 ? size_t(n) : 0);
    const bool ok = out->empty() || fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}

// Writes a copy of a vbmeta image with the AVB "verification disabled" and
// "hashtree disabled" flags set, as `fastboot --disable-verity
// --disable-verification flash vbmeta` does. Needed when the chained
// vbmeta_* images of the build are not on the disk.
bool WriteVbmetaWithVerificationDisabled(const std::wstring& src, const std::wstring& dst, std::string* error) {
    constexpr size_t kFlagsOffset = 120;  // AvbVBMetaImageHeader.flags, big endian
    std::vector<uint8_t> img;
    if (!ReadWholeFile(src, &img)) {
        *error = "cannot read the vbmeta image";
        return false;
    }
    if (img.size() < 256 || std::memcmp(img.data(), "AVB0", 4) != 0) {
        *error = "not an AVB vbmeta image (missing AVB0 magic)";
        return false;
    }
    const uint32_t before = (uint32_t(img[kFlagsOffset]) << 24) | (uint32_t(img[kFlagsOffset + 1]) << 16) |
                            (uint32_t(img[kFlagsOffset + 2]) << 8) | img[kFlagsOffset + 3];
    const uint32_t after = before | 3;
    img[kFlagsOffset] = uint8_t(after >> 24);
    img[kFlagsOffset + 1] = uint8_t(after >> 16);
    img[kFlagsOffset + 2] = uint8_t(after >> 8);
    img[kFlagsOffset + 3] = uint8_t(after);
    FILE* f = _wfopen(dst.c_str(), L"wb");
    if (!f) {
        *error = "cannot write the vbmeta copy into the cache";
        return false;
    }
    const bool ok = fwrite(img.data(), 1, img.size(), f) == img.size();
    fclose(f);
    if (!ok) {
        *error = "cannot write the vbmeta copy into the cache";
        return false;
    }
    Log("disk: vbmeta flags 0x%x -> 0x%x (verification disabled) in %ls", before, after, dst.c_str());
    return true;
}

}  // namespace

bool PrepareDiskPartitions(const std::wstring& config_path, const std::wstring& images_dir,
                           const std::wstring& cache_dir, std::vector<PartitionSpec>* out, std::string* error) {
    std::string text;
    if (!ReadTextFile(config_path, &text)) {
        *error = "cannot open disk config";
        return false;
    }
    std::istringstream f(text);
    CreateDirectoryW(cache_dir.c_str(), nullptr);
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ss(line);
        std::string name, source, opt;
        if (!(ss >> name)) continue;
        if (!(ss >> source)) {
            *error = "line " + std::to_string(lineno) + ": missing source";
            return false;
        }
        bool rw = false, disable_verification = false;
        uint64_t min_size = 0;
        std::string ifmissing;
        while (ss >> opt) {
            if (opt == "rw") rw = true;
            else if (opt == "disable-verification") disable_verification = true;
            else if (opt.rfind("min=", 0) == 0) ParseSize(opt.substr(4), &min_size);
            else if (opt.rfind("ifmissing=", 0) == 0) ifmissing = opt.substr(10);
            else {
                *error = "line " + std::to_string(lineno) + ": unknown option " + opt;
                return false;
            }
        }

        std::wstring cache_file = cache_dir + L"\\" + Widen(name) + L".raw";
        PartitionSpec spec;
        spec.name = name;

        if (source.rfind("blank:", 0) != 0) {
            std::wstring src = images_dir + L"\\" + Widen(source);
            if (!FileExists(src)) {
                if (ifmissing.empty()) {
                    *error = "partition '" + name + "': image " + source + " not found in the images folder";
                    return false;
                }
                Log("disk: %s not found, partition '%s' will be %s", source.c_str(), name.c_str(), ifmissing.c_str());
                source = ifmissing;
            } else if (IsAndroidSparse(src)) {
                uint64_t expanded = SparseExpandedSize(src);
                if (!FileExists(cache_file) || FileSize(cache_file) < expanded || FileMtime(cache_file) < FileMtime(src)) {
                    Log("disk: expanding sparse %s (%llu MiB) into cache, first run only...", source.c_str(),
                        (unsigned long long)(expanded >> 20));
                    if (!UnsparseImage(src, cache_file)) {
                        *error = "failed to expand sparse image " + source;
                        return false;
                    }
                }
                if (min_size && !EnsureFileSize(cache_file, min_size)) {
                    *error = "cannot grow " + name;
                    return false;
                }
                spec.file = cache_file;
                spec.writable = true;
                out->push_back(spec);
                continue;
            } else if (disable_verification) {
                std::string why;
                if (!WriteVbmetaWithVerificationDisabled(src, cache_file, &why)) {
                    *error = "partition '" + name + "': " + why;
                    return false;
                }
                spec.file = cache_file;
                spec.writable = rw;
                out->push_back(spec);
                continue;
            } else {
                spec.file = src;
                spec.writable = rw;
                out->push_back(spec);
                continue;
            }
        }
        uint64_t size = 0;
        if (!ParseSize(source.substr(6), &size)) {
            *error = "partition '" + name + "': bad blank size";
            return false;
        }
        if (min_size > size) size = min_size;
        if (!EnsureFileSize(cache_file, size)) {
            *error = "cannot create blank partition file for " + name;
            return false;
        }
        spec.file = cache_file;
        spec.writable = true;
        out->push_back(spec);
    }
    if (out->empty()) {
        *error = "disk config lists no partitions";
        return false;
    }
    return true;
}

}  // namespace Aetheron
