#include "zip_reader.h"

#include <algorithm>
#include <cstring>

namespace Aetheron {

namespace {

constexpr uint32_t kSigEocd = 0x06054b50, kSigEocd64 = 0x06064b50, kSigLocator64 = 0x07064b50,
                   kSigCentral = 0x02014b50, kSigLocal = 0x04034b50;
constexpr size_t kMaxCentralDir = 64u << 20;

uint16_t R16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t R32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t R64(const uint8_t* p) { return uint64_t(R32(p)) | (uint64_t(R32(p + 4)) << 32); }

bool ReadAt(FILE* f, uint64_t off, void* buf, size_t len) {
    return _fseeki64(f, int64_t(off), SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// Streaming DEFLATE decoder (RFC 1951). Reads at most `remaining` bytes from
// the file and writes through a 32 KiB sliding window.
class Inflater {
public:
    Inflater(FILE* f, uint64_t remaining, const ZipReader::Sink& sink) : f_(f), remaining_(remaining), sink_(sink) {}

    bool Run() {
        int last;
        do {
            last = Bits(1);
            const int type = Bits(2);
            if (bad_) return false;
            if (type == 0) Stored();
            else if (type == 1) Fixed();
            else if (type == 2) Dynamic();
            else bad_ = true;
        } while (!last && !bad_);
        if (!bad_ && wpos_ && !sink_(window_, wpos_)) bad_ = true;
        return !bad_;
    }

private:
    static constexpr uint32_t kWindow = 32768;
    struct Huff {
        uint16_t count[16];
        uint16_t symbol[288];
    };

    int NextByte() {
        if (in_pos_ == in_len_) {
            const size_t want = size_t(std::min<uint64_t>(remaining_, sizeof(in_)));
            in_len_ = want ? fread(in_, 1, want, f_) : 0;
            in_pos_ = 0;
            remaining_ -= in_len_;
            if (!in_len_) {
                bad_ = true;
                return 0;
            }
        }
        return in_[in_pos_++];
    }

    int Bits(int n) {
        while (bit_count_ < n) {
            bit_buf_ |= uint32_t(NextByte()) << bit_count_;
            if (bad_) return 0;
            bit_count_ += 8;
        }
        const int v = int(bit_buf_ & ((1u << n) - 1));
        bit_buf_ >>= n;
        bit_count_ -= n;
        return v;
    }

    void Emit(uint8_t b) {
        window_[wpos_++] = b;
        ++total_;
        if (wpos_ == kWindow) {
            if (!sink_(window_, kWindow)) bad_ = true;
            wpos_ = 0;
        }
    }

    void Stored() {
        bit_buf_ = 0;  // skip to the next byte boundary
        bit_count_ = 0;
        uint8_t h[4];
        for (uint8_t& b : h) b = uint8_t(NextByte());
        if (bad_) return;
        const unsigned len = R16(h);
        if (len != (~R16(h + 2) & 0xFFFFu)) {
            bad_ = true;
            return;
        }
        for (unsigned i = 0; i < len && !bad_; ++i) Emit(uint8_t(NextByte()));
    }

    // Builds a canonical Huffman decoding table. Returns 0 for a complete
    // code, negative for an over-subscribed one, positive for an incomplete one.
    static int Build(Huff* h, const uint8_t* length, int n) {
        for (uint16_t& c : h->count) c = 0;
        for (int s = 0; s < n; ++s) h->count[length[s]]++;
        if (h->count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= h->count[len];
            if (left < 0) return left;
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h->count[len]);
        for (int s = 0; s < n; ++s)
            if (length[s]) h->symbol[offs[length[s]]++] = uint16_t(s);
        return left;
    }

    int Decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= Bits(1);
            if (bad_) return -1;
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    void Codes(const Huff& lencode, const Huff& distcode) {
        static const uint16_t lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                           31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        while (!bad_) {
            int sym = Decode(lencode);
            if (sym < 0) {
                bad_ = true;
            } else if (sym < 256) {
                Emit(uint8_t(sym));
            } else if (sym == 256) {
                return;
            } else {
                sym -= 257;
                if (sym >= 29) {
                    bad_ = true;
                    return;
                }
                const int len = lbase[sym] + Bits(lext[sym]);
                const int dsym = Decode(distcode);
                if (dsym < 0 || dsym >= 30) {
                    bad_ = true;
                    return;
                }
                const uint32_t dist = dbase[dsym] + uint32_t(Bits(dext[dsym]));
                if (bad_ || dist > total_) {
                    bad_ = true;
                    return;
                }
                for (int i = 0; i < len && !bad_; ++i) Emit(window_[(wpos_ - dist) & (kWindow - 1)]);
            }
        }
    }

    void Fixed() {
        uint8_t lengths[288];
        int s = 0;
        for (; s < 144; ++s) lengths[s] = 8;
        for (; s < 256; ++s) lengths[s] = 9;
        for (; s < 280; ++s) lengths[s] = 7;
        for (; s < 288; ++s) lengths[s] = 8;
        Huff lencode, distcode;
        Build(&lencode, lengths, 288);
        for (s = 0; s < 30; ++s) lengths[s] = 5;
        Build(&distcode, lengths, 30);
        Codes(lencode, distcode);
    }

    void Dynamic() {
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (bad_ || nlen > 286 || ndist > 30) {
            bad_ = true;
            return;
        }
        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(Bits(3));
        Huff lencode, distcode;
        if (bad_ || Build(&lencode, lengths, 19) != 0) {  // the code-length code must be complete
            bad_ = true;
            return;
        }
        int index = 0;
        while (index < nlen + ndist) {
            int sym = Decode(lencode);
            if (sym < 0) {
                bad_ = true;
                return;
            }
            if (sym < 16) {
                lengths[index++] = uint8_t(sym);
                continue;
            }
            int len = 0, repeat;
            if (sym == 16) {
                if (index == 0) {
                    bad_ = true;
                    return;
                }
                len = lengths[index - 1];
                repeat = 3 + Bits(2);
            } else if (sym == 17) {
                repeat = 3 + Bits(3);
            } else {
                repeat = 11 + Bits(7);
            }
            if (bad_ || index + repeat > nlen + ndist) {
                bad_ = true;
                return;
            }
            while (repeat--) lengths[index++] = uint8_t(len);
        }
        if (lengths[256] == 0) {  // no end-of-block code
            bad_ = true;
            return;
        }
        // Incomplete codes are only allowed when they consist of a single symbol.
        int err = Build(&lencode, lengths, nlen);
        if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) {
            bad_ = true;
            return;
        }
        err = Build(&distcode, lengths + nlen, ndist);
        if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) {
            bad_ = true;
            return;
        }
        Codes(lencode, distcode);
    }

    FILE* f_;
    uint64_t remaining_;
    const ZipReader::Sink& sink_;
    uint8_t in_[65536];
    size_t in_pos_ = 0, in_len_ = 0;
    uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
    uint8_t window_[kWindow];
    uint32_t wpos_ = 0;
    uint64_t total_ = 0;
    bool bad_ = false;
};

}  // namespace

void ZipReader::Close() {
    if (f_) fclose(f_);
    f_ = nullptr;
    entries_.clear();
}

bool ZipReader::Open(const std::wstring& path, std::string* error) {
    Close();
    f_ = _wfopen(path.c_str(), L"rb");
    if (!f_) {
        *error = "cannot open the file";
        return false;
    }
    _fseeki64(f_, 0, SEEK_END);
    const uint64_t file_size = uint64_t(_ftelli64(f_));
    // The end-of-central-directory record sits at the end, before an optional comment.
    const size_t tail_len = size_t(std::min<uint64_t>(file_size, 65536 + 22));
    std::vector<uint8_t> tail(tail_len);
    if (tail_len < 22 || !ReadAt(f_, file_size - tail_len, tail.data(), tail_len)) {
        *error = "not a zip archive";
        return false;
    }
    size_t eocd = tail_len;
    for (size_t i = tail_len - 22 + 1; i-- > 0;)
        if (R32(&tail[i]) == kSigEocd) {
            eocd = i;
            break;
        }
    if (eocd == tail_len) {
        *error = "not a zip archive";
        return false;
    }
    uint64_t count = R16(&tail[eocd + 10]), cd_size = R32(&tail[eocd + 12]), cd_offset = R32(&tail[eocd + 16]);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) {
        uint8_t e64[56];
        if (eocd < 20 || R32(&tail[eocd - 20]) != kSigLocator64 || !ReadAt(f_, R64(&tail[eocd - 20 + 8]), e64, 56) ||
            R32(e64) != kSigEocd64) {
            *error = "damaged zip archive (zip64 records)";
            return false;
        }
        count = R64(e64 + 32);
        cd_size = R64(e64 + 40);
        cd_offset = R64(e64 + 48);
    }
    if (cd_size > kMaxCentralDir || cd_offset > file_size || cd_size > file_size - cd_offset) {
        *error = "damaged zip archive (central directory)";
        return false;
    }
    std::vector<uint8_t> cd(size_t(cd_size) + 1);
    if (cd_size && !ReadAt(f_, cd_offset, cd.data(), size_t(cd_size))) {
        *error = "damaged zip archive (central directory)";
        return false;
    }
    size_t pos = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (pos + 46 > cd_size || R32(&cd[pos]) != kSigCentral) {
            *error = "damaged zip archive (entry list)";
            return false;
        }
        const uint8_t* p = &cd[pos];
        const size_t name_len = R16(p + 28), extra_len = R16(p + 30), comment_len = R16(p + 32);
        if (pos + 46 + name_len + extra_len + comment_len > cd_size) {
            *error = "damaged zip archive (entry list)";
            return false;
        }
        ZipEntry e;
        e.flags = R16(p + 8);
        e.method = R16(p + 10);
        e.crc = R32(p + 16);
        e.comp_size = R32(p + 20);
        e.size = R32(p + 24);
        e.header_offset = R32(p + 42);
        e.name.assign(reinterpret_cast<const char*>(p + 46), name_len);
        // zip64: 8-byte values replace the fields that hold 0xFFFFFFFF, in this order.
        const uint8_t* x = p + 46 + name_len;
        for (size_t xo = 0; xo + 4 <= extra_len;) {
            const uint16_t id = R16(x + xo), len = R16(x + xo + 2);
            if (xo + 4 + len > extra_len) break;
            if (id == 0x0001) {
                size_t fo = xo + 4;
                const size_t end = xo + 4 + len;
                if (e.size == 0xFFFFFFFFu && fo + 8 <= end) e.size = R64(x + fo), fo += 8;
                if (e.comp_size == 0xFFFFFFFFu && fo + 8 <= end) e.comp_size = R64(x + fo), fo += 8;
                if (e.header_offset == 0xFFFFFFFFu && fo + 8 <= end) e.header_offset = R64(x + fo);
            }
            xo += 4 + len;
        }
        entries_.push_back(std::move(e));
        pos += 46 + name_len + extra_len + comment_len;
    }
    return true;
}

bool ZipReader::Extract(const ZipEntry& e, const Sink& sink, std::string* error) {
    if (!f_) {
        *error = "archive is not open";
        return false;
    }
    if (e.flags & 1) {
        *error = "the archive is password protected";
        return false;
    }
    uint8_t lh[30];
    if (!ReadAt(f_, e.header_offset, lh, 30) || R32(lh) != kSigLocal) {
        *error = "damaged zip archive (" + e.name + ")";
        return false;
    }
    const uint64_t data = e.header_offset + 30 + R16(lh + 26) + R16(lh + 28);
    if (_fseeki64(f_, int64_t(data), SEEK_SET) != 0) {
        *error = "damaged zip archive (" + e.name + ")";
        return false;
    }
    uint32_t crc = 0;
    uint64_t written = 0;
    const Sink checked = [&](const uint8_t* d, size_t n) {
        crc = Crc32(crc, d, n);
        written += n;
        return written <= e.size && sink(d, n);
    };
    bool ok;
    if (e.method == 0) {
        std::vector<uint8_t> buf(1 << 16);
        uint64_t left = e.comp_size;
        ok = true;
        while (left && ok) {
            const size_t n = size_t(std::min<uint64_t>(left, buf.size()));
            ok = fread(buf.data(), 1, n, f_) == n && checked(buf.data(), n);
            left -= n;
        }
    } else if (e.method == 8) {
        ok = Inflater(f_, e.comp_size, checked).Run();
    } else {
        *error = "unsupported compression in the archive (" + e.name + ")";
        return false;
    }
    if (!ok || written != e.size || crc != e.crc) {
        *error = "damaged or incomplete data in the archive (" + e.name + ")";
        return false;
    }
    return true;
}

bool ZipReader::ExtractToFile(const ZipEntry& e, const std::wstring& dst, std::string* error) {
    FILE* out = _wfopen(dst.c_str(), L"wb");
    if (!out) {
        *error = "cannot write a temporary file";
        return false;
    }
    const bool ok = Extract(e, [out](const uint8_t* d, size_t n) { return fwrite(d, 1, n, out) == n; }, error);
    const bool closed = fclose(out) == 0;
    if (ok && !closed) *error = "cannot write a temporary file (disk full?)";
    return ok && closed;
}

}  // namespace Aetheron
