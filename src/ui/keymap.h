// Windows virtual-key codes -> Linux evdev key codes (input-event-codes.h).
#pragma once
#include <cstdint>

namespace Aetheron {

inline uint16_t VkToLinuxKey(uint32_t vk, bool extended) {
    if (vk >= 'A' && vk <= 'Z') {
        static const uint16_t letters[26] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                                             49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
        return letters[vk - 'A'];
    }
    if (vk >= '1' && vk <= '9') return uint16_t(2 + (vk - '1'));
    if (vk == '0') return 11;
    if (vk >= 0x70 && vk <= 0x79) return uint16_t(59 + (vk - 0x70));  // F1-F10
    switch (vk) {
        case 0x7A: return 87;   // F11
        case 0x7B: return 88;   // F12
        case 0x1B: return 1;    // Esc
        case 0x08: return 14;   // Backspace
        case 0x09: return 15;   // Tab
        case 0x0D: return extended ? 96 : 28;  // KP Enter / Enter
        case 0x20: return 57;   // Space
        case 0x10: return 42;   // Shift
        case 0xA0: return 42;
        case 0xA1: return 54;
        case 0x11: return extended ? 97 : 29;  // Ctrl
        case 0x12: return extended ? 100 : 56; // Alt
        case 0x14: return 58;   // Caps Lock
        case 0x25: return 105;  // Left
        case 0x26: return 103;  // Up
        case 0x27: return 106;  // Right
        case 0x28: return 108;  // Down
        case 0x24: return 102;  // Home
        case 0x23: return 107;  // End
        case 0x21: return 104;  // Page Up
        case 0x22: return 109;  // Page Down
        case 0x2D: return 110;  // Insert
        case 0x2E: return 111;  // Delete
        case 0xBA: return 39;   // ;
        case 0xBB: return 13;   // =
        case 0xBC: return 51;   // ,
        case 0xBD: return 12;   // -
        case 0xBE: return 52;   // .
        case 0xBF: return 53;   // /
        case 0xC0: return 41;   // `
        case 0xDB: return 26;   // [
        case 0xDC: return 43;   // backslash
        case 0xDD: return 27;   // ]
        case 0xDE: return 40;   // '
        case 0x5B: return 125;  // Left Windows -> Meta
        case 0x5C: return 126;
        case 0x5D: return 127;  // Menu (Compose)
        default: return 0;
    }
}

}  // namespace Aetheron
