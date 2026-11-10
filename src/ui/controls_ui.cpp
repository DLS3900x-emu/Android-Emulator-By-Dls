#include "controls_ui.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <cmath>
#include <string>

#include "gdiplus_util.h"
#include "shell_ui.h"

namespace Aetheron {

namespace {
constexpr COLORREF kCard = RGB(38, 39, 44), kLine = RGB(58, 60, 66), kText = RGB(234, 236, 240);
constexpr COLORREF kDim = RGB(158, 162, 170), kHover = RGB(52, 54, 60), kPrimary = RGB(226, 228, 233);
constexpr COLORREF kOnPrimary = RGB(24, 25, 28), kTrackOff = RGB(70, 72, 79), kKnobOff = RGB(180, 184, 192);
constexpr UINT_PTR kTimer = 1;
constexpr int kPad = 16, kRowTop[] = {0, 50, 94, 134}, kRowBottom[] = {50, 94, 134, 178};
constexpr int kTrackLeft = 96, kTrackRight = ControlsPopup::kW - 22, kSliderY = 114;
constexpr float kMinOpacity = 0.2f;

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    auto ch = [t](int x, int y) { return int(x + (y - x) * t + 0.5f); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}
}  // namespace

bool ControlsPopup::Create(HINSTANCE inst, HWND owner) {
    owner_ = owner;
    WNDCLASSW wc = {};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AetheronControlsPopup";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, kW, kH, owner,
                            nullptr, inst, this);
    if (!hwnd_) return false;
    // Rounded corners where the system offers them (Windows 11); harmless elsewhere.
    const int round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    return true;
}

void ControlsPopup::Show(int right, int top) {
    if (!hwnd_) return;
    anim_.clear();
    hover_ = pressed_ = kNone;
    SetWindowPos(hwnd_, HWND_TOP, right - kW, top, kW, kH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Redraw();
}

void ControlsPopup::Hide() {
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return;
    if (dragging_) {
        dragging_ = false;
        ReleaseCapture();
    }
    ShowWindow(hwnd_, SW_HIDE);
}

void ControlsPopup::SetState(bool enabled, bool hints, float opacity) {
    enabled_ = enabled;
    hints_ = hints;
    if (!dragging_) opacity_ = opacity;  // while dragging, the knob follows the mouse
    Redraw();
}

void ControlsPopup::Redraw() {
    if (hwnd_ && IsWindowVisible(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

float ControlsPopup::Animate(int key, bool on) {
    const float target = on ? 1.0f : 0.0f;
    auto it = anim_.find(key);
    if (it == anim_.end()) {
        anim_[key] = target;
        return target;
    }
    const float d = target - it->second;
    if (std::fabs(d) < 0.02f) {
        it->second = target;
    } else {
        it->second += d * 0.34f;
        animating_ = true;
    }
    return it->second;
}

int ControlsPopup::RowAt(POINT pt) const {
    if (pt.x < 0 || pt.x >= kW) return kNone;
    for (int i = 0; i < 4; ++i)
        if (pt.y >= kRowTop[i] && pt.y < kRowBottom[i]) return i;
    return kNone;
}

void ControlsPopup::UpdateSlider(int x) {
    float v = float(x - kTrackLeft) / float(kTrackRight - kTrackLeft);
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    opacity_ = kMinOpacity + (1 - kMinOpacity) * v;
    PostMessageW(owner_, WM_COMMAND, kUiKmOpacity, LPARAM(int(opacity_ * 100 + 0.5f)));
    Redraw();
}

void ControlsPopup::Paint(HDC dc) {
    if (!font_) {
        auto make = [](int weight) {
            return CreateFontW(-14, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        font_ = make(FW_NORMAL);
        font_bold_ = make(FW_SEMIBOLD);
    }
    animating_ = false;
    SetBkMode(dc, TRANSPARENT);
    auto fill = [&](const RECT& r, COLORREF c) {
        SetBkColor(dc, c);
        ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
    };
    auto text = [&](int row, const wchar_t* s, HFONT f, COLORREF c) {
        HGDIOBJ old = SelectObject(dc, f);
        SetTextColor(dc, c);
        RECT r = {kPad, kRowTop[row], kW - kPad, kRowBottom[row]};
        DrawTextW(dc, s, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    };
    auto toggle = [&](int row, bool value) {
        const int tw = 44, th = 24, mid = (kRowTop[row] + kRowBottom[row]) / 2;
        const RECT t = {kW - kPad - tw, mid - th / 2, kW - kPad, mid + th / 2};
        const float a = Animate(row, value);
        FillRound(dc, t, th / 2, Mix(kTrackOff, kPrimary, a));
        FillCircle(dc, t.left + 12.0f + a * (tw - 24), float(mid), 8.5f, Mix(kKnobOff, kOnPrimary, a));
    };

    fill(RECT{0, 0, kW, kH}, kCard);
    if (hover_ == kRowEditor) fill(RECT{0, kRowTop[kRowEditor], kW, kRowBottom[kRowEditor]}, kHover);
    fill(RECT{0, kRowBottom[kRowEnable] - 1, kW, kRowBottom[kRowEnable]}, kLine);
    fill(RECT{0, kRowTop[kRowEditor], kW, kRowTop[kRowEditor] + 1}, kLine);

    text(kRowEnable, L"Game controls", font_bold_, kText);
    toggle(kRowEnable, enabled_);
    text(kRowHints, L"Show keys on screen", font_, kText);
    toggle(kRowHints, hints_);

    text(kRowSlider, L"Opacity", font_, kText);
    const float v = (opacity_ - kMinOpacity) / (1 - kMinOpacity);
    const int knob = kTrackLeft + int(v * (kTrackRight - kTrackLeft) + 0.5f);
    FillRound(dc, RECT{kTrackLeft, kSliderY - 2, kTrackRight, kSliderY + 2}, 2, kTrackOff);
    FillRound(dc, RECT{kTrackLeft, kSliderY - 2, knob, kSliderY + 2}, 2, kPrimary);
    FillCircle(dc, float(knob), float(kSliderY), dragging_ || hover_ == kRowSlider ? 8.0f : 7.0f, kPrimary);

    text(kRowEditor, L"Controls editor", font_, kText);
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GdipColor(kDim), 1.5f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        const float cx = kW - kPad - 6.0f, cy = (kRowTop[kRowEditor] + kRowBottom[kRowEditor]) / 2.0f;
        g.DrawLine(&pen, cx - 3, cy - 5, cx + 2, cy);
        g.DrawLine(&pen, cx + 2, cy, cx - 3, cy + 5);
    }

    if (!hwnd_) return;
    if (animating_ && !timer_on_) {
        SetTimer(hwnd_, kTimer, 15, nullptr);
        timer_on_ = true;
    } else if (!animating_ && timer_on_) {
        KillTimer(hwnd_, kTimer);
        timer_on_ = false;
    }
}

LRESULT CALLBACK ControlsPopup::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<ControlsPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ControlsPopup::Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // the main window keeps the keyboard
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC wdc = BeginPaint(hwnd, &ps);
            HDC dc = CreateCompatibleDC(wdc);
            HBITMAP bmp = CreateCompatibleBitmap(wdc, kW, kH);
            HGDIOBJ old = SelectObject(dc, bmp);
            Paint(dc);
            BitBlt(wdc, 0, 0, kW, kH, dc, 0, 0, SRCCOPY);
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimer) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                tracking_ = TrackMouseEvent(&tme) != 0;
            }
            if (dragging_) {
                UpdateSlider(pt.x);
                return 0;
            }
            const int row = RowAt(pt);
            if (row != hover_) {
                hover_ = row;
                Redraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hover_ != kNone) {
                hover_ = kNone;
                Redraw();
            }
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = RowAt(pt);
            if (pressed_ == kRowSlider && pt.x >= kTrackLeft - 12) {
                dragging_ = true;
                SetCapture(hwnd);
                UpdateSlider(pt.x);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
                Redraw();
                return 0;
            }
            const int row = RowAt(pt);
            if (row != pressed_) return 0;
            if (row == kRowEnable) PostMessageW(owner_, WM_COMMAND, kUiKmEnable, 0);
            else if (row == kRowHints) PostMessageW(owner_, WM_COMMAND, kUiKmHints, 0);
            else if (row == kRowEditor) PostMessageW(owner_, WM_COMMAND, kUiKmEditor, 0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace Aetheron
