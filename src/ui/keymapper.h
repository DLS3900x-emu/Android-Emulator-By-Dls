// Key mapping: keyboard keys press spots on the Android touch screen, the way
// games expect fingers. Each held key is its own finger (multi-touch slot), so
// a joystick and several buttons work at the same time.
//
//   tap  <key> <x> <y>                 hold the key = finger down at that spot
//   dpad on|off <up> <left> <down> <right> <x> <y> <radius>
//                                      four keys drag a finger around a centre
//
// Coordinates are fractions of the screen (0..1); the joystick radius is a
// fraction of the screen height.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Aetheron {

class KeyMapper {
public:
    // down = true: finger down, or moved if the slot is already down.
    using TouchFn = std::function<void(int slot, bool down, float x, float y)>;
    static constexpr int kMouseSlot = 0, kDpadSlot = 1, kFirstTapSlot = 2, kSlots = 10;

    struct Tap {
        uint32_t vk;
        float x, y;
    };
    struct Dpad {
        bool enabled = false;
        uint32_t up = 'W', left = 'A', down = 'S', right = 'D';
        float x = 0.15f, y = 0.72f, radius = 0.10f;
    };

    bool enabled = true;
    bool hints = true;
    float aspect = 16.0f / 9.0f;  // screen width / height, keeps the joystick circle round
    std::vector<Tap> taps;
    Dpad dpad;

    void Parse(const std::string& text);
    std::string Serialize() const;

    // Feeds a key press/release (Windows virtual-key code). Returns true if
    // the key is mapped and must not reach Android as a keyboard key.
    bool OnKey(uint32_t vk, bool down, const TouchFn& touch);
    void ReleaseAll(const TouchFn& touch);
    void SetTap(uint32_t vk, float x, float y);  // adds or moves

    static std::string KeyName(uint32_t vk);
    static uint32_t KeyFromName(const std::string& name);  // 0 if unknown

private:
    void UpdateDpad(uint8_t before, const TouchFn& touch);

    uint8_t dpad_state_ = 0;         // bit 0 up, 1 left, 2 down, 3 right
    std::map<uint32_t, int> held_;   // tap key -> slot
};

}  // namespace Aetheron
