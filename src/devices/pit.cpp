#include "pit.h"

#include "../common/host_clock.h"
#include "../common/log.h"

namespace Aetheron {

uint64_t Pit::Ticks(const Channel& c, uint64_t now_ns) const {
    if (now_ns <= c.start_ns) return 0;
    uint64_t d = now_ns - c.start_ns;
    // d * 1193182 / 1e9 without overflow for ~years of uptime
    return (d / 1000000000ULL) * kFreqHz + (d % 1000000000ULL) * kFreqHz / 1000000000ULL;
}

uint16_t Pit::Count(const Channel& c, uint64_t now_ns) const {
    if (!c.loaded) return static_cast<uint16_t>(c.reload);
    bool gated = (c.mode == 0 || c.mode == 2 || c.mode == 3 || c.mode == 4) && !c.gate;
    if (gated && (c.mode == 0 || c.mode == 4)) return static_cast<uint16_t>(c.frozen);
    if (gated) return static_cast<uint16_t>(c.reload);
    uint64_t t = Ticks(c, now_ns);
    switch (c.mode) {
        case 2:
            return static_cast<uint16_t>(c.reload - (t % c.reload));
        case 3: {
            // Square wave: counts down by 2, twice per period.
            uint64_t half = (c.reload + 1) / 2;
            uint64_t ph = t % c.reload;
            uint64_t within = ph < half ? ph : ph - half;
            uint64_t v = c.reload - (within * 2) % c.reload;
            return static_cast<uint16_t>(v & ~1ULL);
        }
        default:  // 0, 1, 4, 5: count down once, then wrap freely
            return static_cast<uint16_t>((uint64_t(c.reload) - t) & 0xFFFF);
    }
}

bool Pit::Out(const Channel& c, uint64_t now_ns) const {
    if (!c.loaded) return c.mode != 0;
    uint64_t t = Ticks(c, now_ns);
    switch (c.mode) {
        case 0:
            if (!c.gate) return false;
            return t >= c.reload;
        case 1:
            return t >= c.reload;
        case 2:
            return (t % c.reload) != c.reload - 1;
        case 3:
            return (t % c.reload) < (c.reload + 1) / 2;
        case 4:
        case 5:
            return t != c.reload;
        default:
            return true;
    }
}

void Pit::Load(int ch, uint32_t value, uint64_t now_ns) {
    Channel& c = ch_[ch];
    c.reload = value ? value : 0x10000;
    if (c.mode == 3 && c.reload == 1) c.reload = 2;
    c.loaded = true;
    c.start_ns = now_ns;
    c.frozen = c.reload;
    c.fired_index = 0;
}

void Pit::SetGate(int ch, bool gate, uint64_t now_ns) {
    Channel& c = ch_[ch];
    if (c.gate == gate) return;
    if (!gate) {
        if (c.mode == 0 || c.mode == 4) c.frozen = Count(c, now_ns);
        c.gate = false;
        return;
    }
    c.gate = true;
    if (c.mode == 1 || c.mode == 5 || c.mode == 2 || c.mode == 3) {
        c.start_ns = now_ns;  // rising gate retriggers / restarts
    } else if (c.mode == 0 || c.mode == 4) {
        // resume from the frozen value
        uint64_t elapsed_ticks = (uint64_t(c.reload) - c.frozen) & 0xFFFF;
        uint64_t elapsed_ns = elapsed_ticks * 1000000000ULL / kFreqHz;
        c.start_ns = now_ns - elapsed_ns;
    }
}

uint32_t Pit::PortRead(uint16_t port, uint8_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = HostNowNs();
    if (port == 0x61) {
        uint8_t v = port61_ & 0x0F;
        if ((now / 15085) & 1) v |= 0x10;  // DRAM refresh toggle (~15 us)
        if (Out(ch_[2], now)) v |= 0x20;
        return v;
    }
    if (port < 0x40 || port > 0x42) return 0xFF;  // 0x43 is write-only
    Channel& c = ch_[port - 0x40];
    if (c.status_latched) {
        c.status_latched = false;
        return c.status;
    }
    uint16_t v = c.latched ? c.latch_value : Count(c, now);
    uint8_t out;
    switch (c.access) {
        case 1:
            out = v & 0xFF;
            c.latched = false;
            break;
        case 2:
            out = v >> 8;
            c.latched = false;
            break;
        default:
            if (c.read_lsb_next) {
                out = v & 0xFF;
                c.read_lsb_next = false;
            } else {
                out = v >> 8;
                c.read_lsb_next = true;
                c.latched = false;
            }
    }
    return out;
}

void Pit::PortWrite(uint16_t port, uint8_t size, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = HostNowNs();
    uint8_t v = static_cast<uint8_t>(value);
    if (port == 0x61) {
        port61_ = v & 0x0F;
        SetGate(2, (v & 1) != 0, now);
        return;
    }
    if (port == 0x43) {
        int sel = v >> 6;
        if (sel == 3) {  // read-back command
            for (int i = 0; i < 3; ++i) {
                if (!(v & (2 << i))) continue;
                Channel& c = ch_[i];
                if (!(v & 0x20) && !c.latched) {
                    c.latched = true;
                    c.latch_value = Count(c, now);
                    c.read_lsb_next = true;
                }
                if (!(v & 0x10) && !c.status_latched) {
                    c.status_latched = true;
                    c.status = uint8_t((Out(c, now) ? 0x80 : 0) | (c.loaded ? 0 : 0x40) | (c.access << 4) |
                                       (c.mode << 1) | (c.bcd ? 1 : 0));
                }
            }
            return;
        }
        Channel& c = ch_[sel];
        uint8_t access = (v >> 4) & 3;
        if (access == 0) {  // counter latch command
            if (!c.latched) {
                c.latched = true;
                c.latch_value = Count(c, now);
                c.read_lsb_next = true;
            }
            return;
        }
        c.access = access;
        c.mode = (v >> 1) & 7;
        if (c.mode > 5) c.mode -= 4;  // 6,7 alias 2,3
        c.bcd = v & 1;
        if (c.bcd) LogOnce("pit-bcd", "PIT: BCD counting requested (unimplemented, using binary)");
        c.loaded = false;
        c.write_lsb_next = true;
        c.read_lsb_next = true;
        c.latched = false;
        return;
    }
    if (port < 0x40 || port > 0x42) return;
    int ch = port - 0x40;
    Channel& c = ch_[ch];
    switch (c.access) {
        case 1:
            Load(ch, v, now);
            break;
        case 2:
            Load(ch, uint32_t(v) << 8, now);
            break;
        default:
            if (c.write_lsb_next) {
                c.pending_lsb = v;
                c.write_lsb_next = false;
                if (c.mode == 0) c.loaded = false;  // mode 0 stops counting on LSB write
            } else {
                c.write_lsb_next = true;
                Load(ch, c.pending_lsb | (uint32_t(v) << 8), now);
            }
    }
}

uint64_t Pit::NextDeadlineNs() {
    std::lock_guard<std::mutex> lock(mutex_);
    const Channel& c = ch_[0];
    if (!c.loaded) return UINT64_MAX;
    uint64_t period_ns = uint64_t(c.reload) * 1000000000ULL / kFreqHz;
    if (c.mode == 2 || c.mode == 3) return c.start_ns + (c.fired_index + 1) * period_ns;
    if ((c.mode == 0 || c.mode == 4) && c.fired_index == 0) return c.start_ns + period_ns;
    return UINT64_MAX;
}

void Pit::Poll(uint64_t now_ns) {
    bool fire = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Channel& c = ch_[0];
        if (!c.loaded) return;
        uint64_t t = Ticks(c, now_ns);
        if (c.mode == 2 || c.mode == 3) {
            uint64_t idx = t / c.reload;
            if (idx > c.fired_index) {
                // Deliver one interrupt; if the host was late, skip missed
                // periods (the guest's clocksource keeps true time).
                c.fired_index = idx;
                fire = true;
            }
        } else if (c.mode == 0 || c.mode == 4) {
            if (c.fired_index == 0 && t >= c.reload) {
                c.fired_index = 1;
                fire = true;
            }
        }
    }
    if (fire && irq_) irq_->PulseIrq(2);  // ISA IRQ0 is wired to IOAPIC pin 2
}

}  // namespace Aetheron
