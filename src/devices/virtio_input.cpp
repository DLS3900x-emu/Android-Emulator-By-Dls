#include "virtio_input.h"

#include <cstring>

namespace Aetheron {

namespace {
constexpr uint8_t kCfgIdName = 0x01, kCfgIdSerial = 0x02, kCfgIdDevids = 0x03, kCfgPropBits = 0x10,
                  kCfgEvBits = 0x11, kCfgAbsInfo = 0x12;
constexpr uint16_t kEvKey = 0x01, kEvAbs = 0x03;
constexpr uint16_t kBtnTouch = 0x14A;
constexpr uint8_t kPropDirect = 0x01;
constexpr uint8_t kAbsMtSlot = 0x2F, kAbsMtX = 0x35, kAbsMtY = 0x36, kAbsMtTrackingId = 0x39;
constexpr uint32_t kTouchSlots = 10;
}  // namespace

VirtioInput::VirtioInput(GuestMemory* mem, Kind kind, uint32_t abs_w, uint32_t abs_h)
    : VirtioPciDevice(mem, 18, 0x098000, 2, 64, 136), kind_(kind), abs_w_(abs_w), abs_h_(abs_h) {
    name_ = kind == Kind::Touchscreen ? "Aetheron Touchscreen" : "Aetheron Keyboard";
    Refresh();
}

uint32_t VirtioInput::ReadDeviceConfig(uint32_t offset, uint8_t size) {
    uint32_t v = 0;
    if (offset + size <= sizeof(cfg_)) std::memcpy(&v, cfg_ + offset, size);
    return v;
}

void VirtioInput::WriteDeviceConfig(uint32_t offset, uint8_t size, uint32_t value) {
    for (uint8_t i = 0; i < size; ++i) {
        uint32_t o = offset + i;
        uint8_t b = uint8_t(value >> (8 * i));
        if (o == 0) select_ = b;
        if (o == 1) subsel_ = b;
    }
    Refresh();
}

void VirtioInput::Refresh() {
    std::memset(cfg_, 0, sizeof(cfg_));
    cfg_[0] = select_;
    cfg_[1] = subsel_;
    uint8_t* u = cfg_ + 8;
    uint8_t size = 0;
    auto set_bit = [&](int bit) {
        u[bit / 8] |= uint8_t(1 << (bit % 8));
        if (bit / 8 + 1 > size) size = uint8_t(bit / 8 + 1);
    };
    switch (select_) {
        case kCfgIdName:
            size = uint8_t(name_.size());
            std::memcpy(u, name_.data(), size);
            break;
        case kCfgIdSerial: {
            const char* s = kind_ == Kind::Touchscreen ? "aetheron-touch-0" : "aetheron-kbd-0";
            size = uint8_t(std::strlen(s));
            std::memcpy(u, s, size);
            break;
        }
        case kCfgIdDevids: {
            uint16_t ids[4] = {0x0006 /*BUS_VIRTUAL*/, 0x1AF4,
                               uint16_t(kind_ == Kind::Touchscreen ? 0x0001 : 0x0002), 0x0001};
            std::memcpy(u, ids, 8);
            size = 8;
            break;
        }
        case kCfgPropBits:
            if (kind_ == Kind::Touchscreen) {
                u[0] = 1 << kPropDirect;
                size = 1;
            }
            break;
        case kCfgEvBits:
            if (kind_ == Kind::Touchscreen) {
                if (subsel_ == kEvKey) set_bit(kBtnTouch);
                if (subsel_ == kEvAbs) {
                    set_bit(0x00);  // ABS_X
                    set_bit(0x01);  // ABS_Y
                    // Multi-touch protocol B: one slot per finger.
                    set_bit(kAbsMtSlot);
                    set_bit(kAbsMtX);
                    set_bit(kAbsMtY);
                    set_bit(kAbsMtTrackingId);
                }
            } else if (subsel_ == kEvKey) {
                for (int k = 1; k <= 248; ++k) set_bit(k);
                set_bit(580);  // KEY_APPSELECT (Android: recent apps)
            }
            break;
        case kCfgAbsInfo:
            if (kind_ == Kind::Touchscreen && (subsel_ == 0 || subsel_ == 1 || subsel_ == kAbsMtSlot ||
                                               subsel_ == kAbsMtX || subsel_ == kAbsMtY || subsel_ == kAbsMtTrackingId)) {
                const uint32_t max = subsel_ == 0 || subsel_ == kAbsMtX   ? abs_w_ - 1
                                     : subsel_ == 1 || subsel_ == kAbsMtY ? abs_h_ - 1
                                     : subsel_ == kAbsMtSlot              ? kTouchSlots - 1
                                                                          : 65535;
                uint32_t info[5] = {0, max, 0, 0, 0};
                std::memcpy(u, info, sizeof(info));
                size = sizeof(info);
            }
            break;
        default:
            break;
    }
    cfg_[2] = size;
}

void VirtioInput::Send(const InputEvent* events, size_t count) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!driver_ok()) return;
    for (size_t i = 0; i < count; ++i) pending_.push_back(events[i]);
    while (pending_.size() > 4096) pending_.pop_front();
    Pump();
}

void VirtioInput::OnQueueNotify(uint16_t q) {
    if (q == 0) {
        Pump();
        return;
    }
    VirtqChain c;  // statusq: LED state etc. from the guest; acknowledged
    bool any = false;
    while (PopChain(1, &c)) {
        PushUsed(1, c.head, 0);
        any = true;
    }
    if (any) NotifyQueue(1);
}

void VirtioInput::Pump() {
    bool any = false;
    VirtqChain c;
    while (!pending_.empty() && PopChain(0, &c)) {
        uint8_t ev[8];
        InputEvent e = pending_.front();
        pending_.pop_front();
        std::memcpy(ev, &e.type, 2);
        std::memcpy(ev + 2, &e.code, 2);
        std::memcpy(ev + 4, &e.value, 4);
        uint32_t n = ScatterWritable(c, ev, 8);
        PushUsed(0, c.head, n);
        any = true;
    }
    if (any) NotifyQueue(0);
}

}  // namespace Aetheron
