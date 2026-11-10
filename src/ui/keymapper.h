// Key mapping: keyboard keys act as fingers on the Android touch screen. Each
// held key is its own finger (multi-touch slot), so a joystick and several
// buttons work at the same time.
//
//   tap    <key> <x> <y>             finger down while the key is held
//   repeat <key> <x> <y>             taps again and again while the key is held
//   swipe  <key> <x> <y> <x2> <y2>   one swipe from the first to the second spot
//   dpad on|off <up> <left> <down> <right> <x> <y> <radius>
//                                    four keys drag a finger around a centre
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
    static constexpr int kMouseSlot = 0, kDpadSlot = 1, kFirstSlot = 2, kSlots = 10;
    static constexpr uint64_t kRepeatHalfMs = 45, kSwipeMs = 160;

    enum Type { kTap, kRepeat, kSwipe };
    struct Item {
        Type type = kTap;
        uint32_t vk = 0;  // 0: no key assigned yet
        float x = 0, y = 0;
        float x2 = 0, y2 = 0;  // swipe end
    };
    struct Dpad {
        bool enabled = false;
        uint32_t up = 'W', left = 'A', down = 'S', right = 'D';
        float x = 0.15f, y = 0.72f, radius = 0.10f;
    };

    bool enabled = true;
    bool hints = true;
    float opacity = 0.7f;         // of the on-screen hints
    float aspect = 16.0f / 9.0f;  // screen width / height, keeps the joystick circle round
    std::vector<Item> items;
    Dpad dpad;

    void Parse(const std::string& text);
    std::string Serialize() const;

    // Feeds a key press/release (Windows virtual-key code). Returns true if
    // the key is mapped and must not reach Android as a keyboard key.
    bool OnKey(uint32_t vk, bool down, uint64_t now_ms, const TouchFn& touch);
    // Advances repeated taps and swipes; call every ~15 ms while needs_tick().
    void Tick(uint64_t now_ms, const TouchFn& touch);
    bool needs_tick() const;
    void ReleaseAll(const TouchFn& touch);

    // Editing (call ReleaseAll first). AddItem returns the new index.
    int AddItem(Type type, float x, float y);
    // Gives the item a key, taking it away from any other item. Returns the
    // item's index afterwards, or -1 if the key belongs to the joystick.
    int AssignKey(int index, uint32_t vk);

    static std::string KeyName(uint32_t vk);
    static uint32_t KeyFromName(const std::string& name);  // 0 if unknown

private:
    struct Held {
        int slot;
        uint64_t t0;
        bool down;
        Item item;
    };
    void UpdateDpad(uint8_t before, const TouchFn& touch);
    bool IsDpadKey(uint32_t vk) const;

    uint8_t dpad_state_ = 0;          // bit 0 up, 1 left, 2 down, 3 right
    std::map<uint32_t, Held> held_;   // by key
};

}  // namespace Aetheron
