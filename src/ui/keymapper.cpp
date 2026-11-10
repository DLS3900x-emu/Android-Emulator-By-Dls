#include "keymapper.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace Aetheron {

namespace {
struct Named {
    const char* name;
    uint32_t vk;
};
// Windows virtual-key codes (kept numeric so this file builds without windows.h).
const Named kNamed[] = {{"SPACE", 0x20}, {"SHIFT", 0x10}, {"CTRL", 0x11},  {"ALT", 0x12},  {"TAB", 0x09},
                        {"ENTER", 0x0D}, {"LEFT", 0x25},  {"UP", 0x26},    {"RIGHT", 0x27}, {"DOWN", 0x28},
                        {"CAPS", 0x14},  {"BACKSPACE", 0x08}};

float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
}  // namespace

std::string KeyMapper::KeyName(uint32_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    if (vk >= 0x70 && vk <= 0x7B) return "F" + std::to_string(vk - 0x70 + 1);
    for (const Named& n : kNamed)
        if (n.vk == vk) return n.name;
    return "#" + std::to_string(vk);
}

uint32_t KeyMapper::KeyFromName(const std::string& raw) {
    std::string s;
    for (char c : raw) s.push_back(c >= 'a' && c <= 'z' ? char(c - 32) : c);
    if (s.size() == 1 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return uint32_t(s[0]);
    if (s.size() >= 2 && s[0] == 'F' && s[1] >= '1' && s[1] <= '9') {
        const int n = std::atoi(s.c_str() + 1);
        if (n >= 1 && n <= 12) return uint32_t(0x70 + n - 1);
    }
    for (const Named& n : kNamed)
        if (s == n.name) return n.vk;
    if (s.size() > 1 && s[0] == '#') {
        const int n = std::atoi(s.c_str() + 1);
        if (n > 0 && n < 256) return uint32_t(n);
    }
    return 0;
}

bool KeyMapper::IsDpadKey(uint32_t vk) const {
    return dpad.enabled && (vk == dpad.up || vk == dpad.left || vk == dpad.down || vk == dpad.right);
}

int KeyMapper::AddItem(Type type, float x, float y) {
    Item it;
    it.type = type;
    it.x = Clamp01(x);
    it.y = Clamp01(y);
    // A swipe starts out pointing right (or left when there is no room).
    it.x2 = it.x <= 0.8f ? it.x + 0.15f : it.x - 0.15f;
    it.y2 = it.y;
    items.push_back(it);
    return int(items.size()) - 1;
}

int KeyMapper::AssignKey(int index, uint32_t vk) {
    if (index < 0 || index >= int(items.size()) || !vk) return -1;
    if (IsDpadKey(vk)) return -1;
    for (int i = int(items.size()) - 1; i >= 0; --i) {
        if (i == index || items[size_t(i)].vk != vk) continue;
        items.erase(items.begin() + i);
        if (i < index) --index;
    }
    items[size_t(index)].vk = vk;
    return index;
}

void KeyMapper::Parse(const std::string& text) {
    items.clear();
    dpad = Dpad();
    enabled = true;
    hints = true;
    opacity = 0.7f;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        const size_t hash = line.find('#');
        // '#' starts a comment unless it is a "#<number>" key name
        if (hash != std::string::npos && (hash + 1 >= line.size() || line[hash + 1] < '0' || line[hash + 1] > '9'))
            line.resize(hash);
        std::istringstream ls(line);
        std::string word;
        if (!(ls >> word)) continue;
        if (word == "enabled" || word == "hints") {
            std::string v;
            ls >> v;
            (word == "enabled" ? enabled : hints) = v == "on";
        } else if (word == "opacity") {
            float v = 0;
            if (ls >> v) opacity = v < 0.2f ? 0.2f : v > 1 ? 1 : v;
        } else if (word == "tap" || word == "repeat" || word == "swipe") {
            std::string key;
            float x = 0, y = 0;
            if (!(ls >> key >> x >> y)) continue;
            const uint32_t vk = KeyFromName(key);
            if (!vk) continue;
            const Type type = word == "tap" ? kTap : word == "repeat" ? kRepeat : kSwipe;
            float x2 = 0, y2 = 0;
            if (type == kSwipe && !(ls >> x2 >> y2)) continue;
            const int i = AssignKey(AddItem(type, x, y), vk);
            if (i < 0) {
                items.pop_back();  // the key is one of the joystick's
                continue;
            }
            if (type == kSwipe) {
                items[size_t(i)].x2 = Clamp01(x2);
                items[size_t(i)].y2 = Clamp01(y2);
            }
        } else if (word == "dpad") {
            std::string on, k[4];
            float x = 0, y = 0, r = 0;
            if (ls >> on >> k[0] >> k[1] >> k[2] >> k[3] >> x >> y >> r) {
                const uint32_t v[4] = {KeyFromName(k[0]), KeyFromName(k[1]), KeyFromName(k[2]), KeyFromName(k[3])};
                if (v[0] && v[1] && v[2] && v[3] && r > 0) {
                    dpad.enabled = on == "on";
                    dpad.up = v[0];
                    dpad.left = v[1];
                    dpad.down = v[2];
                    dpad.right = v[3];
                    dpad.x = Clamp01(x);
                    dpad.y = Clamp01(y);
                    dpad.radius = r > 0.4f ? 0.4f : r;
                }
            }
        }
    }
    // A key cannot be both a joystick direction and a control.
    for (int i = int(items.size()) - 1; i >= 0; --i)
        if (IsDpadKey(items[size_t(i)].vk)) items.erase(items.begin() + i);
}

std::string KeyMapper::Serialize() const {
    char b[200];
    std::string out = "# Aetheron key mapping. Edit here or in the controls editor (keyboard button in the sidebar).\n"
                      "# tap|repeat <key> <x> <y>    swipe <key> <x> <y> <x2> <y2>\n"
                      "# dpad on|off <up> <left> <down> <right> <x> <y> <radius>\n";
    out += std::string("enabled ") + (enabled ? "on" : "off") + "\n";
    out += std::string("hints ") + (hints ? "on" : "off") + "\n";
    snprintf(b, sizeof(b), "opacity %.2f\n", opacity);
    out += b;
    snprintf(b, sizeof(b), "dpad %s %s %s %s %s %.3f %.3f %.3f\n", dpad.enabled ? "on" : "off", KeyName(dpad.up).c_str(),
             KeyName(dpad.left).c_str(), KeyName(dpad.down).c_str(), KeyName(dpad.right).c_str(), dpad.x, dpad.y,
             dpad.radius);
    out += b;
    for (const Item& t : items) {
        if (!t.vk) continue;
        if (t.type == kSwipe)
            snprintf(b, sizeof(b), "swipe %s %.3f %.3f %.3f %.3f\n", KeyName(t.vk).c_str(), t.x, t.y, t.x2, t.y2);
        else
            snprintf(b, sizeof(b), "%s %s %.3f %.3f\n", t.type == kTap ? "tap" : "repeat", KeyName(t.vk).c_str(), t.x, t.y);
        out += b;
    }
    return out;
}

void KeyMapper::UpdateDpad(uint8_t before, const TouchFn& touch) {
    if (dpad_state_ == 0) {
        if (before) touch(kDpadSlot, false, dpad.x, dpad.y);
        return;
    }
    // A real thumb lands on the stick's centre and then slides.
    if (!before) touch(kDpadSlot, true, dpad.x, dpad.y);
    float dx = float((dpad_state_ >> 3) & 1) - float((dpad_state_ >> 1) & 1);
    float dy = float((dpad_state_ >> 2) & 1) - float(dpad_state_ & 1);
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 0) {
        dx /= len;
        dy /= len;
    }
    touch(kDpadSlot, true, Clamp01(dpad.x + dx * dpad.radius / aspect), Clamp01(dpad.y + dy * dpad.radius));
}

bool KeyMapper::OnKey(uint32_t vk, bool down, uint64_t now_ms, const TouchFn& touch) {
    if (!enabled || !vk) return false;
    if (dpad.enabled) {
        const int bit = vk == dpad.up ? 0 : vk == dpad.left ? 1 : vk == dpad.down ? 2 : vk == dpad.right ? 3 : -1;
        if (bit >= 0) {
            const uint8_t before = dpad_state_;
            if (down) dpad_state_ |= uint8_t(1 << bit);
            else dpad_state_ &= uint8_t(~(1 << bit));
            if (dpad_state_ != before) UpdateDpad(before, touch);  // key repeat changes nothing
            return true;
        }
    }
    for (const Item& t : items) {
        if (t.vk != vk) continue;
        auto it = held_.find(vk);
        if (down) {
            if (it != held_.end()) return true;  // key repeat
            bool used[kSlots] = {};
            for (auto& h : held_)
                if (h.second.down || h.second.item.type == kRepeat) used[h.second.slot] = true;
            for (int slot = kFirstSlot; slot < kSlots; ++slot) {
                if (used[slot]) continue;
                held_[vk] = Held{slot, now_ms, true, t};
                touch(slot, true, t.x, t.y);
                break;
            }
        } else if (it != held_.end()) {
            if (it->second.down) touch(it->second.slot, false, t.x, t.y);
            held_.erase(it);
        }
        return true;
    }
    return false;
}

void KeyMapper::Tick(uint64_t now_ms, const TouchFn& touch) {
    for (auto& e : held_) {
        Held& h = e.second;
        const uint64_t dt = now_ms - h.t0;
        if (h.item.type == kRepeat) {
            const bool want = (dt / kRepeatHalfMs) % 2 == 0;
            if (want != h.down) {
                h.down = want;
                touch(h.slot, want, h.item.x, h.item.y);
            }
        } else if (h.item.type == kSwipe && h.down) {
            const float p = float(dt) / float(kSwipeMs);
            if (p >= 1) {
                touch(h.slot, true, h.item.x2, h.item.y2);
                touch(h.slot, false, h.item.x2, h.item.y2);
                h.down = false;  // one swipe per key press
            } else {
                touch(h.slot, true, h.item.x + (h.item.x2 - h.item.x) * p, h.item.y + (h.item.y2 - h.item.y) * p);
            }
        }
    }
}

bool KeyMapper::needs_tick() const {
    for (auto& e : held_)
        if (e.second.item.type == kRepeat || (e.second.item.type == kSwipe && e.second.down)) return true;
    return false;
}

void KeyMapper::ReleaseAll(const TouchFn& touch) {
    if (dpad_state_) touch(kDpadSlot, false, dpad.x, dpad.y);
    dpad_state_ = 0;
    for (auto& h : held_)
        if (h.second.down) touch(h.second.slot, false, h.second.item.x, h.second.item.y);
    held_.clear();
}

}  // namespace Aetheron
