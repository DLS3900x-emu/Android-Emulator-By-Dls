#include "ramdisk_inspect.h"

#include <cstdio>
#include <cstring>

namespace Aetheron {

namespace {

constexpr uint32_t kLz4LegacyMagic = 0x184C2102;
constexpr size_t kMaxUnpacked = 512u << 20;

uint32_t U32(const std::vector<uint8_t>& v, size_t off) {
    uint32_t x = 0;
    std::memcpy(&x, v.data() + off, 4);
    return x;
}

// Decodes one LZ4 block; returns false on malformed input.
bool Lz4Block(const uint8_t* src, size_t n, std::vector<uint8_t>* out) {
    size_t i = 0;
    while (i < n) {
        const uint8_t token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = src[i++];
                lit += b;
            } while (b == 255);
        }
        if (lit > n - i) return false;
        out->insert(out->end(), src + i, src + i + lit);
        i += lit;
        if (i >= n) break;  // the last sequence has literals only
        if (n - i < 2) return false;
        const size_t offset = src[i] | (size_t(src[i + 1]) << 8);
        i += 2;
        size_t len = token & 15;
        if (len == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = src[i++];
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (offset == 0 || offset > out->size()) return false;
        if (out->size() + len > kMaxUnpacked) return false;
        size_t from = out->size() - offset;
        for (size_t k = 0; k < len; ++k) out->push_back((*out)[from + k]);  // may overlap
    }
    return true;
}

// Unpacks consecutive LZ4-legacy frames (vendor ramdisk fragments are
// compressed separately and concatenated).
bool UnpackLz4Legacy(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
    size_t pos = 0;
    while (pos + 4 <= in.size()) {
        const uint32_t v = U32(in, pos);
        pos += 4;
        if (v == kLz4LegacyMagic) continue;
        if (v == 0) continue;  // padding between fragments
        if (v > in.size() - pos) return !out->empty();
        if (!Lz4Block(in.data() + pos, v, out)) return !out->empty();
        pos += v;
    }
    return true;
}

bool Hex8(const uint8_t* p, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
        const uint8_t c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

void ListCpio(const std::vector<uint8_t>& d, std::vector<std::string>* files) {
    size_t pos = 0;
    while (pos + 110 <= d.size()) {
        if (std::memcmp(&d[pos], "070701", 6) != 0 && std::memcmp(&d[pos], "070702", 6) != 0) {
            ++pos;  // zero padding between concatenated archives
            continue;
        }
        uint32_t filesize, namesize;
        if (!Hex8(&d[pos + 54], &filesize) || !Hex8(&d[pos + 94], &namesize)) return;
        if (namesize == 0 || namesize > d.size() - pos - 110) return;
        std::string name(reinterpret_cast<const char*>(&d[pos + 110]), namesize - 1);
        size_t data = (pos + 110 + namesize + 3) & ~size_t(3);
        if (data > d.size() || filesize > d.size() - data) return;
        if (name != "TRAILER!!!" && name != ".") files->push_back(name);
        pos = (data + filesize + 3) & ~size_t(3);
    }
}

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

std::vector<std::string> ListRamdiskFiles(const std::vector<uint8_t>& ramdisk, std::string* format) {
    std::vector<std::string> files;
    *format = "unknown";
    if (ramdisk.size() < 6) return files;
    if (U32(ramdisk, 0) == kLz4LegacyMagic) {
        *format = "lz4-legacy";
        std::vector<uint8_t> raw;
        if (UnpackLz4Legacy(ramdisk, &raw)) ListCpio(raw, &files);
    } else if (std::memcmp(ramdisk.data(), "0707", 4) == 0) {
        *format = "cpio";
        ListCpio(ramdisk, &files);
    }
    return files;
}

std::string DetectFilesystem(const std::wstring& image_path) {
    FILE* f = _wfopen(image_path.c_str(), L"rb");
    if (!f) return "";
    uint8_t sb[128] = {};
    _fseeki64(f, 1024, SEEK_SET);
    const size_t n = fread(sb, 1, sizeof(sb), f);
    fclose(f);
    if (n < 64) return "";
    uint32_t f2fs_magic;
    std::memcpy(&f2fs_magic, sb, 4);
    if (f2fs_magic == 0xF2F52010) return "f2fs";
    if (sb[56] == 0x53 && sb[57] == 0xEF) return "ext4";
    return "";
}

std::string ChooseFstabSuffix(const std::vector<std::string>& files, const std::string& userdata_fs) {
    const std::string fs = userdata_fs.empty() ? "f2fs" : userdata_fs;
    std::string best;
    int best_score = -1;
    for (const std::string& path : files) {
        const size_t slash = path.find_last_of('/');
        const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
        if (base.rfind("fstab.", 0) != 0 || base.size() <= 6) continue;
        const std::string suffix = base.substr(6);
        // Cuttlefish names them cf.<fs>.<mode>; hctr2 is its default mode on x86_64.
        const int score = (Contains(suffix, fs) ? 2 : 0) + (Contains(suffix, "hctr2") ? 1 : 0);
        if (score > best_score) {
            best_score = score;
            best = suffix;
        }
    }
    return best;
}

}  // namespace Aetheron
