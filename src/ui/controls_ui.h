// The small "Game controls" card that opens next to the sidebar's keyboard
// button: key mapping on/off, on-screen hints, their opacity, and the way
// into the controls editor. It is its own borderless window so it can float
// over the Android picture; it never takes the keyboard focus.
#pragma once
#include <windows.h>

#include <map>

namespace Aetheron {

class ControlsPopup {
public:
    static constexpr int kW = 256, kH = 178;

    bool Create(HINSTANCE inst, HWND owner);
    void Show(int right, int top);  // screen coordinates of the top-right corner
    void Hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    void SetState(bool enabled, bool hints, float opacity);
    void Paint(HDC dc);

private:
    enum Row { kNone = -1, kRowEnable, kRowHints, kRowSlider, kRowEditor };
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    int RowAt(POINT pt) const;
    void UpdateSlider(int x);
    void Redraw();
    float Animate(int key, bool on);

    HWND hwnd_ = nullptr, owner_ = nullptr;
    bool enabled_ = true, hints_ = true;
    float opacity_ = 0.7f;
    int hover_ = kNone, pressed_ = kNone;
    bool dragging_ = false, tracking_ = false;
    std::map<int, float> anim_;
    bool animating_ = false, timer_on_ = false;
    HFONT font_ = nullptr, font_bold_ = nullptr;
};

}  // namespace Aetheron
