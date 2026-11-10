// virtio-input (virtio spec 5.8). Two flavours: an absolute-coordinate
// touchscreen (INPUT_PROP_DIRECT, so Android treats it as a touch screen) and
// a keyboard. Events use Linux evdev codes.
#pragma once
#include <deque>
#include <string>

#include "virtio_pci.h"

namespace Aetheron {

struct InputEvent {
    uint16_t type, code;
    int32_t value;
};

class VirtioInput : public VirtioPciDevice {
public:
    enum class Kind { Touchscreen, Keyboard };
    VirtioInput(GuestMemory* mem, Kind kind, uint32_t abs_width, uint32_t abs_height);

    // Thread-safe. Queue a group of events (caller includes EV_SYN).
    void Send(const InputEvent* events, size_t count);

protected:
    void OnQueueNotify(uint16_t q) override;
    uint32_t ReadDeviceConfig(uint32_t offset, uint8_t size) override;
    void WriteDeviceConfig(uint32_t offset, uint8_t size, uint32_t value) override;
    void OnReset() override { pending_.clear(); }

private:
    void Refresh();
    void Pump();  // mutex held

    Kind kind_;
    uint32_t abs_w_, abs_h_;
    std::string name_;
    uint8_t select_ = 0, subsel_ = 0;
    uint8_t cfg_[136] = {};
    std::deque<InputEvent> pending_;
};

}  // namespace Aetheron
