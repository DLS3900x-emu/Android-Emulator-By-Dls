#include "android_boot.h"

#include <cstring>
#include <fstream>

#include "../common/log.h"

namespace Aetheron {

namespace {

bool ReadAll(const std::wstring& path, std::vector<uint8_t>* out) {
    // std::ifstream with wide paths is an MSVC extension; use Win32 directly.
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out->resize(size_t(n));
    bool ok = n == 0 || fread(out->data(), 1, size_t(n), f) == size_t(n);
    fclose(f);
    return ok;
}

uint32_t U32(const std::vector<uint8_t>& v, size_t off) {
    uint32_t x = 0;
    if (off + 4 <= v.size()) std::memcpy(&x, v.data() + off, 4);
    return x;
}

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

std::string CString(const std::vector<uint8_t>& v, size_t off, size_t max) {
    std::string s;
    for (size_t i = 0; i < max && off + i < v.size() && v[off + i]; ++i) s.push_back(char(v[off + i]));
    return s;
}

bool Slice(const std::vector<uint8_t>& v, uint64_t off, uint64_t len, std::vector<uint8_t>* out) {
    if (off > v.size() || len > v.size() - off) return false;
    out->assign(v.begin() + off, v.begin() + off + len);
    return true;
}

// boot.img / init_boot.img, header version 3 or 4 (fixed 4096-byte pages).
//   0  magic "ANDROID!"   8 kernel_size  12 ramdisk_size  16 os_version
//   20 header_size        24 reserved[4] 40 header_version 44 cmdline[1536]
//   1580 signature_size (v4)
bool ParseBootImage(const std::vector<uint8_t>& img, std::vector<uint8_t>* kernel, std::vector<uint8_t>* ramdisk,
                    std::string* cmdline, uint32_t* version, std::string* error) {
    if (img.size() < 4096 || std::memcmp(img.data(), "ANDROID!", 8) != 0) {
        *error = "not an Android boot image (missing ANDROID! magic)";
        return false;
    }
    uint32_t hv = U32(img, 40);
    if (hv < 3) {
        *error = "boot image header v" + std::to_string(hv) + " not supported (need v3/v4)";
        return false;
    }
    *version = hv;
    const uint64_t page = 4096;
    uint32_t ksz = U32(img, 8), rsz = U32(img, 12);
    uint64_t koff = page;
    uint64_t roff = koff + AlignUp(ksz, page);
    if (kernel && !Slice(img, koff, ksz, kernel)) {
        *error = "kernel section out of bounds";
        return false;
    }
    if (ramdisk && !Slice(img, roff, rsz, ramdisk)) {
        *error = "ramdisk section out of bounds";
        return false;
    }
    if (cmdline) *cmdline = CString(img, 44, 1536);
    return true;
}

// vendor_boot.img v3/v4:
//   0 magic "VNDRBOOT" 8 header_version 12 page_size 16 kernel_addr 20 ramdisk_addr
//   24 vendor_ramdisk_size 28 cmdline[2048] 2076 tags_addr 2080 name[16]
//   2096 header_size 2100 dtb_size 2104 dtb_addr(u64)
//   v4: 2112 vendor_ramdisk_table_size 2116 entry_num 2120 entry_size 2124 bootconfig_size
// Sections (each page aligned): header, vendor ramdisks, dtb, ramdisk table, bootconfig.
// Table entry: size u32, offset u32, type u32, name[32], board_id[16 x u32].
bool ParseVendorBoot(const std::vector<uint8_t>& img, AndroidBootFiles* out, std::string* error) {
    if (img.size() < 4096 || std::memcmp(img.data(), "VNDRBOOT", 8) != 0) {
        *error = "not a vendor_boot image (missing VNDRBOOT magic)";
        return false;
    }
    uint32_t hv = U32(img, 8), page = U32(img, 12);
    if (hv < 3 || page == 0) {
        *error = "unsupported vendor_boot header";
        return false;
    }
    out->vendor_header_version = hv;
    uint32_t rd_size = U32(img, 24);
    out->vendor_cmdline = CString(img, 28, 2048);
    uint32_t header_size = U32(img, 2096), dtb_size = U32(img, 2100);
    uint64_t rd_off = AlignUp(header_size, page);
    uint64_t dtb_off = rd_off + AlignUp(rd_size, page);
    std::vector<uint8_t> all_rd;
    if (!Slice(img, rd_off, rd_size, &all_rd)) {
        *error = "vendor ramdisk out of bounds";
        return false;
    }
    if (hv < 4) {
        out->vendor_ramdisk = std::move(all_rd);
        return true;
    }
    uint32_t table_size = U32(img, 2112), entries = U32(img, 2116), entry_size = U32(img, 2120),
             bc_size = U32(img, 2124);
    uint64_t table_off = dtb_off + AlignUp(dtb_size, page);
    uint64_t bc_off = table_off + AlignUp(table_size, page);
    if (bc_size) {
        std::vector<uint8_t> bc;
        if (!Slice(img, bc_off, bc_size, &bc)) {
            *error = "vendor bootconfig out of bounds";
            return false;
        }
        out->vendor_bootconfig.assign(bc.begin(), bc.end());
        while (!out->vendor_bootconfig.empty() && out->vendor_bootconfig.back() == 0)
            out->vendor_bootconfig.pop_back();
    }
    if (entries == 0) {
        out->vendor_ramdisk = std::move(all_rd);
        return true;
    }
    for (uint32_t i = 0; i < entries; ++i) {
        uint64_t e = table_off + uint64_t(i) * entry_size;
        uint32_t sz = U32(img, e), off = U32(img, e + 4), type = U32(img, e + 8);
        std::string name = CString(img, e + 12, 32);
        // VENDOR_RAMDISK_TYPE_RECOVERY (2) is only loaded for recovery boot.
        bool load = type != 2;
        Log("vendor_boot: ramdisk[%u] '%s' type %u size %u %s", i, name.c_str(), type, sz,
            load ? "(loaded)" : "(skipped: recovery)");
        if (!load) continue;
        if (uint64_t(off) + sz > all_rd.size()) {
            *error = "vendor ramdisk table entry out of bounds";
            return false;
        }
        out->vendor_ramdisk.insert(out->vendor_ramdisk.end(), all_rd.begin() + off, all_rd.begin() + off + sz);
    }
    return true;
}

}  // namespace

bool LoadAndroidBootImages(const std::wstring& boot_img, const std::wstring& init_boot_img,
                           const std::wstring& vendor_boot_img, AndroidBootFiles* out, std::string* error) {
    std::vector<uint8_t> boot, init_boot, vendor;
    if (!ReadAll(boot_img, &boot)) {
        *error = "cannot read boot.img";
        return false;
    }
    std::vector<uint8_t> boot_ramdisk;
    if (!ParseBootImage(boot, &out->kernel, &boot_ramdisk, &out->boot_cmdline, &out->boot_header_version, error)) {
        *error = "boot.img: " + *error;
        return false;
    }
    if (ReadAll(init_boot_img, &init_boot)) {
        uint32_t v;
        if (!ParseBootImage(init_boot, nullptr, &out->generic_ramdisk, nullptr, &v, error)) {
            *error = "init_boot.img: " + *error;
            return false;
        }
    } else {
        out->generic_ramdisk = std::move(boot_ramdisk);  // pre-Android-13 layout
    }
    if (!ReadAll(vendor_boot_img, &vendor)) {
        *error = "cannot read vendor_boot.img";
        return false;
    }
    if (!ParseVendorBoot(vendor, out, error)) {
        *error = "vendor_boot.img: " + *error;
        return false;
    }
    if (out->kernel.empty() || out->generic_ramdisk.empty() || out->vendor_ramdisk.empty()) {
        *error = "a required section (kernel / generic ramdisk / vendor ramdisk) is empty";
        return false;
    }
    Log("boot: kernel %zu KiB (boot hdr v%u), generic ramdisk %zu KiB, vendor ramdisk %zu KiB (vendor hdr v%u)",
        out->kernel.size() >> 10, out->boot_header_version, out->generic_ramdisk.size() >> 10,
        out->vendor_ramdisk.size() >> 10, out->vendor_header_version);
    Log("boot: boot.img cmdline: '%s'", out->boot_cmdline.c_str());
    Log("boot: vendor cmdline: '%s'", out->vendor_cmdline.c_str());
    return true;
}

std::vector<uint8_t> BuildInitrdWithBootconfig(const AndroidBootFiles& f, const std::string& params) {
    std::vector<uint8_t> rd;
    rd.reserve(f.vendor_ramdisk.size() + f.generic_ramdisk.size() + params.size() + 64);
    rd.insert(rd.end(), f.vendor_ramdisk.begin(), f.vendor_ramdisk.end());
    while (rd.size() % 4) rd.push_back(0);
    rd.insert(rd.end(), f.generic_ramdisk.begin(), f.generic_ramdisk.end());
    while (rd.size() % 4) rd.push_back(0);
    if (params.empty()) return rd;

    std::string p = params;
    if (p.back() != '\n') p.push_back('\n');
    // Pad with NULs so the size field is 4-byte aligned (as AOSP libbootconfig does).
    while (p.size() % 4) p.push_back('\0');
    uint32_t size = uint32_t(p.size());
    uint32_t csum = 0;
    for (unsigned char c : p) csum += c;
    rd.insert(rd.end(), p.begin(), p.end());
    for (int i = 0; i < 4; ++i) rd.push_back(uint8_t(size >> (8 * i)));
    for (int i = 0; i < 4; ++i) rd.push_back(uint8_t(csum >> (8 * i)));
    const char* magic = "#BOOTCONFIG\n";
    rd.insert(rd.end(), magic, magic + 12);
    return rd;
}

}  // namespace Aetheron
