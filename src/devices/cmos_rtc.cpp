#include "cmos_rtc.h"

#include <windows.h>

#include <ctime>

#include "../common/log.h"

namespace Aetheron {

namespace {
int64_t HostUnixSeconds() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100ns since 1601
    return int64_t(t / 10000000ULL) - 11644473600LL;
}
}  // namespace

CmosRtc::CmosRtc(IrqSink* irq) : irq_(irq) {
    ram_[0x0A] = 0x26;  // 32.768 kHz time base, 1024 Hz rate select
    ram_[0x0B] = 0x02;  // 24-hour mode, BCD
    ram_[0x0D] = 0x80;  // valid RAM and time
}

void CmosRtc::SetMemorySize(uint64_t low_bytes, uint64_t high_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 0x34/0x35: memory above 16 MiB (below 4 GiB) in 64 KiB units
    uint64_t above16 = low_bytes > (16ULL << 20) ? (low_bytes - (16ULL << 20)) >> 16 : 0;
    if (above16 > 0xFFFF) above16 = 0xFFFF;
    ram_[0x34] = uint8_t(above16);
    ram_[0x35] = uint8_t(above16 >> 8);
    // 0x5B-0x5D: memory above 4 GiB in 64 KiB units
    uint64_t above4g = high_bytes >> 16;
    ram_[0x5B] = uint8_t(above4g);
    ram_[0x5C] = uint8_t(above4g >> 8);
    ram_[0x5D] = uint8_t(above4g >> 16);
}

int64_t CmosRtc::GuestEpochSeconds() const { return HostUnixSeconds() + offset_seconds_; }

uint8_t CmosRtc::ToReg(int v) const {
    if (ram_[0x0B] & 0x04) return uint8_t(v);  // binary mode
    return uint8_t(((v / 10) << 4) | (v % 10));
}

int CmosRtc::FromReg(uint8_t v) const {
    if (ram_[0x0B] & 0x04) return v;
    return (v >> 4) * 10 + (v & 0x0F);
}

uint8_t CmosRtc::ReadTimeReg(uint8_t index) {
    __time64_t t = GuestEpochSeconds();
    struct tm tmv;
    _gmtime64_s(&tmv, &t);
    switch (index) {
        case 0x00:
            return ToReg(tmv.tm_sec);
        case 0x02:
            return ToReg(tmv.tm_min);
        case 0x04: {
            int h = tmv.tm_hour;
            if (ram_[0x0B] & 0x02) return ToReg(h);
            bool pm = h >= 12;
            h %= 12;
            if (h == 0) h = 12;
            return uint8_t(ToReg(h) | (pm ? 0x80 : 0));
        }
        case 0x06:
            return ToReg(tmv.tm_wday + 1);
        case 0x07:
            return ToReg(tmv.tm_mday);
        case 0x08:
            return ToReg(tmv.tm_mon + 1);
        case 0x09:
            return ToReg(tmv.tm_year % 100);
        case 0x32:
            return ToReg((tmv.tm_year + 1900) / 100);
        default:
            return ram_[index];
    }
}

void CmosRtc::WriteTimeReg(uint8_t index, uint8_t v) {
    __time64_t t = GuestEpochSeconds();
    struct tm tmv;
    _gmtime64_s(&tmv, &t);
    switch (index) {
        case 0x00:
            tmv.tm_sec = FromReg(v);
            break;
        case 0x02:
            tmv.tm_min = FromReg(v);
            break;
        case 0x04: {
            if (ram_[0x0B] & 0x02) {
                tmv.tm_hour = FromReg(v);
            } else {
                int h = FromReg(v & 0x7F) % 12;
                tmv.tm_hour = h + ((v & 0x80) ? 12 : 0);
            }
            break;
        }
        case 0x07:
            tmv.tm_mday = FromReg(v);
            break;
        case 0x08:
            tmv.tm_mon = FromReg(v) - 1;
            break;
        case 0x09:
            tmv.tm_year = ((tmv.tm_year + 1900) / 100) * 100 + FromReg(v) - 1900;
            break;
        case 0x32:
            tmv.tm_year = FromReg(v) * 100 + (tmv.tm_year % 100) - 1900;
            break;
        default:
            return;
    }
    __time64_t nt = _mkgmtime64(&tmv);
    if (nt != -1) offset_seconds_ = int64_t(nt) - HostUnixSeconds();
}

uint32_t CmosRtc::PortRead(uint16_t port, uint8_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (port == 0x70) return 0xFF;  // index register is write-only
    uint8_t idx = index_;
    switch (idx) {
        case 0x00: case 0x02: case 0x04: case 0x06: case 0x07: case 0x08: case 0x09: case 0x32:
            return ReadTimeReg(idx);
        case 0x0A: {
            // Update-in-progress is set for the last ~244us before each second.
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            bool uip = (t % 10000000ULL) >= (10000000ULL - 2440);
            return uint8_t((ram_[0x0A] & 0x7F) | (uip ? 0x80 : 0));
        }
        case 0x0C: {
            uint8_t v = ram_[0x0C];
            ram_[0x0C] = 0;  // reading clears interrupt flags
            return v;
        }
        default:
            return ram_[idx & 0x7F];
    }
}

void CmosRtc::PortWrite(uint16_t port, uint8_t size, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t v = uint8_t(value);
    if (port == 0x70) {
        index_ = v & 0x7F;  // bit 7 is the NMI mask
        return;
    }
    switch (index_) {
        case 0x00: case 0x02: case 0x04: case 0x07: case 0x08: case 0x09: case 0x32:
            WriteTimeReg(index_, v);
            return;
        case 0x0B:
            ram_[0x0B] = v;
            if (v & 0x70)
                LogOnce("rtc-irq", "RTC: guest enabled periodic/alarm/update interrupts (reg B=0x%02x) - "
                                   "UNIMPLEMENTED, no IRQ8 will be generated", v);
            return;
        case 0x0C:
        case 0x0D:
            return;  // read-only
        default:
            ram_[index_] = v;
    }
}

}  // namespace Aetheron
