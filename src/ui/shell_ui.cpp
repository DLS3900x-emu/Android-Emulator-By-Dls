#include "shell_ui.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "gdiplus_util.h"

namespace Aetheron {

namespace {

// Neutral gray theme.
constexpr COLORREF kBar = RGB(30, 31, 34), kContent = RGB(15, 15, 17), kPanel = RGB(27, 28, 32);
constexpr COLORREF kHover = RGB(50, 52, 57), kPress = RGB(66, 68, 74), kControl = RGB(40, 42, 47);
constexpr COLORREF kLine = RGB(42, 44, 49), kText = RGB(234, 236, 240), kDim = RGB(148, 152, 160);
constexpr COLORREF kIcon = RGB(196, 199, 205), kCloseHover = RGB(196, 43, 28);
constexpr COLORREF kPrimary = RGB(226, 228, 233), kPrimaryHover = RGB(205, 208, 214), kOnPrimary = RGB(24, 25, 28);
constexpr COLORREF kTrackOff = RGB(58, 60, 66), kKnobOff = RGB(170, 174, 182);

// Glyphs of the Windows icon font "Segoe MDL2 Assets".
constexpr wchar_t kGlyphMenu = 0xE700, kGlyphSettings = 0xE713, kGlyphBack = 0xE72B, kGlyphHome = 0xE80F,
                  kGlyphRecents = 0xE7C4, kGlyphFullscreen = 0xE740, kGlyphCamera = 0xE722, kGlyphConsole = 0xE756,
                  kGlyphFolder = 0xE8B7, kGlyphLogs = 0xE8A5, kGlyphKeyboard = 0xE765, kGlyphMinimize = 0xE921,
                  kGlyphMaximize = 0xE922, kGlyphRestore = 0xE923, kGlyphClose = 0xE8BB, kGlyphPrev = 0xE76B,
                  kGlyphNext = 0xE76C;

const wchar_t* const kPages[] = {L"Performance", L"Display", L"Controls", L"Network", L"Android", L"Tools", L"About"};

struct Resolution {
    uint32_t w, h;
};
const Resolution kResolutions[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {720, 1280}, {1080, 1920}};
const uint32_t kDpis[] = {160, 240, 320};
const uint64_t kRamMb[] = {2048, 3072, 4096, 6144, 8192};
const uint32_t kCpus[] = {1, 2, 4, 6, 8};

template <typename T, size_t N, typename Eq>
size_t IndexOf(const T (&options)[N], Eq eq) {
    for (size_t i = 0; i < N; ++i)
        if (eq(options[i])) return i;
    return 0;
}

bool In(const RECT& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    auto ch = [t](int x, int y) { return int(x + (y - x) * t + 0.5f); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

RECT Inset(const RECT& r, int dx, int dy) { return RECT{r.left + dx, r.top + dy, r.right - dx, r.bottom - dy}; }

}  // namespace

void ShellUi::Redraw() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void ShellUi::Command(UINT id) {
    if (hwnd_) PostMessageW(hwnd_, WM_COMMAND, id, 0);
}

void ShellUi::SetBarsVisible(bool visible) {
    bars_ = visible;
    Redraw();
}

RECT ShellUi::ContentRect() const {
    RECT rc = {0, 0, 0, 0};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    if (bars_) {
        rc.top += kTopH;
        rc.right -= kSideW;
    }
    if (rc.right < rc.left) rc.right = rc.left;
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    return rc;
}

void ShellUi::SetStatus(const std::wstring& text) {
    status_ = text;
    Redraw();
}

void ShellUi::SetControls(const UiControls& c) {
    controls_ = c;
    Redraw();
}

void ShellUi::OpenSettings(const UiSettings& current, int page) {
    edit_ = saved_ = current;
    if (page >= 0 && page < int(std::size(kPages))) page_ = page;
    settings_open_ = true;
    anim_.clear();
    Redraw();
}

void ShellUi::CloseSettings() {
    settings_open_ = false;
    anim_.clear();
    Redraw();
}

const ShellUi::Hot* ShellUi::HotAt(POINT pt) const {
    // Later entries are drawn on top.
    for (auto it = hots_.rbegin(); it != hots_.rend(); ++it)
        if (In(it->r, pt)) return &*it;
    return nullptr;
}

LRESULT ShellUi::HitTest(HWND hwnd, POINT pt, bool resizable) const {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (resizable) {
        const bool l = pt.x < kResizeBorder, r = pt.x >= rc.right - kResizeBorder;
        const bool t = pt.y < kResizeBorder, b = pt.y >= rc.bottom - kResizeBorder;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (b && l) return HTBOTTOMLEFT;
        if (b && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t) return HTTOP;
        if (b) return HTBOTTOM;
    }
    // The empty part of the top bar drags the window like a title bar.
    if (bars_ && pt.y < kTopH && !HotAt(pt)) return HTCAPTION;
    return HTCLIENT;
}

void ShellUi::MouseMove(POINT pt) {
    const Hot* h = HotAt(pt);
    const int key = h ? h->key : -1;
    if (key != hover_) {
        hover_ = key;
        Redraw();
    }
}

void ShellUi::MouseLeave() {
    if (hover_ != -1) {
        hover_ = -1;
        Redraw();
    }
}

void ShellUi::MouseDown(POINT pt) {
    const Hot* h = HotAt(pt);
    pressed_ = h ? h->key : -1;
    Redraw();
}

void ShellUi::MouseUp(POINT pt) {
    const Hot* h = HotAt(pt);
    std::function<void()> act;
    if (h && h->key == pressed_) act = h->act;  // copy: the action may rebuild hots_
    pressed_ = -1;
    Redraw();
    if (act) act();
}

void ShellUi::EnsureFonts() {
    if (font_ui_) return;
    auto make = [](int px, int weight, const wchar_t* face) {
        return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    font_ui_ = make(14, FW_NORMAL, L"Segoe UI");
    font_small_ = make(12, FW_NORMAL, L"Segoe UI");
    font_bold_ = make(14, FW_SEMIBOLD, L"Segoe UI");
    font_big_ = make(22, FW_SEMIBOLD, L"Segoe UI");
    font_icon_ = make(15, FW_NORMAL, L"Segoe MDL2 Assets");
}

void ShellUi::Fill(HDC dc, const RECT& r, COLORREF c) {
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
}

void ShellUi::Text(HDC dc, const RECT& r, const std::wstring& s, HFONT f, COLORREF c, UINT flags) {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    RECT rr = r;
    DrawTextW(dc, s.c_str(), int(s.size()), &rr, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

void ShellUi::AddHot(const RECT& r, int key, std::function<void()> act, const std::wstring& hint) {
    hots_.push_back({r, key, std::move(act), hint});
}

COLORREF ShellUi::StateColor(int key, COLORREF normal, COLORREF hover, COLORREF press) const {
    if (key == pressed_ && key == hover_) return press;
    if (key == hover_) return hover;
    return normal;
}

// Eases a toggle's knob towards its state; keeps the repaint timer running meanwhile.
float ShellUi::Animate(int key, bool on) {
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

void ShellUi::IconButton(HDC dc, const RECT& r, int key, wchar_t glyph, const std::wstring& hint, UINT cmd,
                         bool danger) {
    const bool hot = key == hover_;
    if (hot || key == pressed_)
        FillRound(dc, Inset(r, 5, 4), 8, danger ? kCloseHover : StateColor(key, kBar, kHover, kPress));
    Text(dc, r, std::wstring(1, glyph), font_icon_, hot ? kText : kIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddHot(r, key, [this, cmd] { Command(cmd); }, hint);
}

void ShellUi::DrawBars(HDC dc, const RECT& client) {
    const int cw = client.right, ch = client.bottom;

    // ---- top bar ----
    Fill(dc, RECT{0, 0, cw, kTopH}, kBar);
    const std::wstring name = L"AETHERON";
    SIZE sz = {0, 0};
    HGDIOBJ old = SelectObject(dc, font_bold_);
    GetTextExtentPoint32W(dc, name.c_str(), int(name.size()), &sz);
    SelectObject(dc, old);
    int x = 16;
    Text(dc, RECT{x, 0, x + sz.cx, kTopH}, name, font_bold_, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x += sz.cx + 12;
    const int nav_w = 40;
    IconButton(dc, RECT{x, 0, x + nav_w, kTopH}, 1, kGlyphBack, L"Back", kUiBack);
    x += nav_w;
    IconButton(dc, RECT{x, 0, x + nav_w, kTopH}, 2, kGlyphHome, L"Home", kUiHome);
    x += nav_w;
    IconButton(dc, RECT{x, 0, x + nav_w, kTopH}, 3, kGlyphRecents, L"Recent apps", kUiRecents);
    x += nav_w;

    const int win_w = 46;
    int rx = cw - 4 * win_w;
    const bool zoomed = hwnd_ && IsZoomed(hwnd_);
    IconButton(dc, RECT{rx, 0, rx + win_w, kTopH}, 4, kGlyphMenu, L"Menu", kUiMenu);
    IconButton(dc, RECT{rx + win_w, 0, rx + 2 * win_w, kTopH}, 5, kGlyphMinimize, L"Minimize", kUiMinimize);
    IconButton(dc, RECT{rx + 2 * win_w, 0, rx + 3 * win_w, kTopH}, 6, zoomed ? kGlyphRestore : kGlyphMaximize,
               zoomed ? L"Restore" : L"Maximize", kUiMaximize);
    IconButton(dc, RECT{rx + 3 * win_w, 0, cw, kTopH}, 7, kGlyphClose, L"Shut Android down and close", kUiExit, true);

    // ---- sidebar ----
    Fill(dc, RECT{cw - kSideW, kTopH, cw, ch}, kBar);
    const int bh = 42;
    int y = kTopH + 4;
    auto side_btn = [&](int key, wchar_t glyph, const wchar_t* hint, UINT cmd) {
        IconButton(dc, RECT{cw - kSideW, y, cw, y + bh}, key, glyph, hint, cmd);
        y += bh;
    };
    side_btn(10, kGlyphFullscreen, L"Full screen (F11)", kUiFullscreen);
    side_btn(15, kGlyphKeyboard, L"Key mapping: play with the keyboard", kUiControls);
    side_btn(11, kGlyphCamera, L"Save a screenshot", kUiScreenshot);
    side_btn(12, kGlyphConsole, L"Android console", kUiShowConsole);
    side_btn(13, kGlyphLogs, L"Collect logs into a zip", kUiCollectLogs);
    side_btn(14, kGlyphFolder, L"Open the Aetheron folder", kUiOpenFolder);
    // Bottom group, only if the window is tall enough not to overlap the top group.
    const int bottom_top = ch - 4 - 4 * bh;
    if (bottom_top >= y + 8) {
        y = bottom_top;
        side_btn(20, kGlyphSettings, L"Settings", kUiSettings);
        side_btn(21, kGlyphBack, L"Back", kUiBack);
        side_btn(22, kGlyphHome, L"Home", kUiHome);
        side_btn(23, kGlyphRecents, L"Recent apps", kUiRecents);
    }

    // ---- status / hint text in the free part of the top bar ----
    std::wstring text = status_;
    for (const Hot& h : hots_)
        if (h.key == hover_ && !h.hint.empty()) text = h.hint;
    RECT sr = {x + 14, 0, rx - 12, kTopH};
    if (sr.right > sr.left)
        Text(dc, sr, text, font_small_, kDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void ShellUi::DrawSettings(HDC dc, const RECT& area) {
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    const int cw = std::min(880, std::max(aw - 40, 320)), chh = std::min(620, std::max(ah - 40, 240));
    const RECT card = {area.left + (aw - cw) / 2, area.top + (ah - chh) / 2, area.left + (aw - cw) / 2 + cw,
                       area.top + (ah - chh) / 2 + chh};
    FillRound(dc, card, 14, kPanel);
    const int pad = 28, px = card.left + pad, py = card.top + 22;

    Text(dc, RECT{px, py, card.right - 80, py + 36}, L"Settings", font_big_, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    {
        const RECT r = {card.right - 54, card.top + 18, card.right - 18, card.top + 54};
        if (hover_ == 100) FillRound(dc, r, 10, StateColor(100, kPanel, kHover, kPress));
        Text(dc, r, std::wstring(1, kGlyphClose), font_icon_, hover_ == 100 ? kText : kIcon,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(r, 100, [this] { Command(kUiSettings); }, L"Close settings");
    }

    // Page list.
    const int list_w = 172, top = py + 58;
    for (int i = 0; i < int(std::size(kPages)); ++i) {
        const RECT r = {px - 10, top + i * 40, px + list_w - 22, top + i * 40 + 34};
        const bool sel = i == page_;
        if (sel) FillRound(dc, r, 9, kControl);
        else if (hover_ == 110 + i) FillRound(dc, r, 9, RGB(34, 35, 40));
        Text(dc, RECT{r.left + 12, r.top, r.right, r.bottom}, kPages[i], sel ? font_bold_ : font_ui_, sel ? kText : kDim,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        AddHot(r, 110 + i, [this, i] {
            page_ = i;
            anim_.clear();
            Redraw();
        }, L"");
    }

    // Rows of the selected page.
    const int rx = px + list_w + 8, rw = card.right - pad - rx, ctl_w = 204, ctl_h = 34, row_h = 54;
    const int bottom = card.bottom - 22;
    int y = top - 6;
    int key = 200;
    auto label = [&](const std::wstring& s, int h) {
        Text(dc, RECT{rx, y, rx + rw - ctl_w - 16, y + h}, s, font_ui_, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    auto end_row = [&](int h) {
        y += h;
        Fill(dc, RECT{rx, y - 1, rx + rw, y}, kLine);
    };
    auto chevron = [&](const RECT& r, wchar_t glyph, std::function<void()> act) {
        if (hover_ == key) FillCircle(dc, (r.left + r.right) / 2.0f, (r.top + r.bottom) / 2.0f, 13, StateColor(key, kControl, kHover, kPress));
        Text(dc, r, std::wstring(1, glyph), font_icon_, hover_ == key ? kText : kIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(r, key++, [this, act] { act(); Redraw(); }, L"");
    };
    auto choice = [&](const std::wstring& name, const std::wstring& value, std::function<void()> prev,
                      std::function<void()> next) {
        label(name, row_h);
        const RECT box = {rx + rw - ctl_w, y + (row_h - ctl_h) / 2, rx + rw, y + (row_h - ctl_h) / 2 + ctl_h};
        FillRound(dc, box, 9, kControl);
        chevron(RECT{box.left + 2, box.top, box.left + 2 + ctl_h, box.bottom}, kGlyphPrev, prev);
        chevron(RECT{box.right - 2 - ctl_h, box.top, box.right - 2, box.bottom}, kGlyphNext, next);
        Text(dc, RECT{box.left + ctl_h, box.top, box.right - ctl_h, box.bottom}, value, font_ui_, kText,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        end_row(row_h);
    };
    auto toggle = [&](const std::wstring& name, bool value, std::function<void()> act) {
        label(name, row_h);
        const int tw = 44, th = 24;
        const RECT t = {rx + rw - tw, y + (row_h - th) / 2, rx + rw, y + (row_h - th) / 2 + th};
        const float a = Animate(key, value);
        COLORREF track = Mix(kTrackOff, kPrimary, a);
        if (hover_ == key) track = Mix(track, value ? kPrimaryHover : kPress, 0.6f);
        FillRound(dc, t, th / 2, track);
        FillCircle(dc, t.left + 12.0f + a * (tw - 24), t.top + th / 2.0f, 8.5f, Mix(kKnobOff, kOnPrimary, a));
        AddHot(RECT{t.left - 10, t.top - 10, t.right + 4, t.bottom + 10}, key++, [this, act] { act(); Redraw(); }, L"");
        end_row(row_h);
    };
    auto info = [&](const std::wstring& name, const std::wstring& text) {
        Text(dc, RECT{rx, y + 10, rx + rw, y + 30}, name, font_ui_, kText, DT_LEFT | DT_SINGLELINE);
        Text(dc, RECT{rx, y + 32, rx + rw, y + 68}, text, font_small_, kDim, DT_LEFT | DT_WORDBREAK);
        end_row(76);
    };
    auto button = [&](const std::wstring& name, const std::wstring& caption, UINT cmd, int h, int w) {
        label(name, h);
        const int bh = std::min(ctl_h, h - 10);
        const RECT b = {rx + rw - w, y + (h - bh) / 2, rx + rw, y + (h - bh) / 2 + bh};
        FillRound(dc, b, 9, StateColor(key, kControl, kHover, kPress));
        Text(dc, b, caption, font_ui_, kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(b, key++, [this, cmd] { Command(cmd); }, L"");
        end_row(h);
    };
    // Steps through a fixed list of options without wrapping around.
    auto step = [](size_t index, size_t count, int delta) {
        const int64_t n = int64_t(index) + delta;
        return size_t(n < 0 ? 0 : n >= int64_t(count) ? count - 1 : n);
    };

    bool saved_page = true;  // pages whose values are written by "Save changes"
    switch (page_) {
        case 0: {
            const size_t i = IndexOf(kRamMb, [&](uint64_t v) { return v == edit_.ram_mb; });
            choice(L"Memory for Android", std::to_wstring(edit_.ram_mb / 1024) + L" GB",
                   [this, i, step] { edit_.ram_mb = kRamMb[step(i, std::size(kRamMb), -1)]; },
                   [this, i, step] { edit_.ram_mb = kRamMb[step(i, std::size(kRamMb), 1)]; });
            const size_t c = IndexOf(kCpus, [&](uint32_t v) { return v == edit_.cpus; });
            choice(L"CPU cores (more than 1 is experimental)", std::to_wstring(edit_.cpus),
                   [this, c, step] { edit_.cpus = kCpus[step(c, std::size(kCpus), -1)]; },
                   [this, c, step] { edit_.cpus = kCpus[step(c, std::size(kCpus), 1)]; });
            info(L"Graphics: software", L"Android draws everything on the CPU. GPU acceleration is not implemented yet.");
            break;
        }
        case 1: {
            const size_t i = IndexOf(kResolutions, [&](const Resolution& r) { return r.w == edit_.width && r.h == edit_.height; });
            auto set_res = [this](size_t k) {
                edit_.width = kResolutions[k].w;
                edit_.height = kResolutions[k].h;
            };
            choice(L"Resolution", std::to_wstring(edit_.width) + L" x " + std::to_wstring(edit_.height),
                   [i, step, set_res] { set_res(step(i, std::size(kResolutions), -1)); },
                   [i, step, set_res] { set_res(step(i, std::size(kResolutions), 1)); });
            const size_t d = IndexOf(kDpis, [&](uint32_t v) { return v == edit_.dpi; });
            choice(L"Pixel density", std::to_wstring(edit_.dpi) + L" DPI",
                   [this, d, step] { edit_.dpi = kDpis[step(d, std::size(kDpis), -1)]; },
                   [this, d, step] { edit_.dpi = kDpis[step(d, std::size(kDpis), 1)]; });
            toggle(L"Aetheron boot screen", edit_.boot_splash, [this] { edit_.boot_splash = !edit_.boot_splash; });
            info(L"Tip", L"A lower resolution is faster, because Android draws every pixel on the CPU.");
            break;
        }
        case kPageControls: {
            saved_page = false;  // these apply at once
            toggle(L"Key mapping (keys press spots on the screen)", controls_.enabled, [this] { Command(kUiKmEnable); });
            toggle(L"Show the keys on the screen", controls_.hints, [this] { Command(kUiKmHints); });
            toggle(L"W A S D joystick", controls_.dpad, [this] { Command(kUiKmDpad); });
            button(L"Joystick position", L"Place on screen", kUiKmPlaceDpad, 46, 150);
            button(L"Map a key to a spot on the screen", L"Add key", kUiKmAdd, 46, 150);
            const int row = 38;
            const int fit = std::max(0, (bottom - y) / row);
            const int shown = std::min<int>(int(controls_.taps.size()), fit);
            for (int i = 0; i < shown; ++i) {
                // The last visible row turns into a note when more keys exist than fit.
                if (i == shown - 1 && shown < int(controls_.taps.size())) {
                    Text(dc, RECT{rx, y, rx + rw, y + row},
                         L"+ " + std::to_wstring(controls_.taps.size() - size_t(i)) +
                             L" more keys - see config\\aetheron_keymap.txt",
                         font_small_, kDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    break;
                }
                button(controls_.taps[size_t(i)], L"Remove", kUiKmRemove + UINT(i), row, 96);
            }
            break;
        }
        case 3:
            toggle(L"Internet access (built-in NAT)", edit_.network, [this] { edit_.network = !edit_.network; });
            break;
        case 4:
            toggle(L"Run the clean-up script after boot", edit_.first_boot_setup,
                   [this] { edit_.first_boot_setup = !edit_.first_boot_setup; });
            info(L"Clean-up script", L"config\\aetheron_setup.sh hides unwanted apps and applies the dark theme. "
                                     L"It runs once, and again whenever the file changes.");
            button(L"Erase all Android data", L"Factory reset...", kUiWipe, row_h, ctl_w);
            break;
        case 5:
            saved_page = false;
            button(L"Android console (serial)", L"Open", kUiShowConsole, row_h, ctl_w);
            button(L"Collect logs into a zip", L"Collect", kUiCollectLogs, row_h, ctl_w);
            button(L"Aetheron folder", L"Open", kUiOpenFolder, row_h, ctl_w);
            break;
        default:
            saved_page = false;
            info(L"Aetheron", L"Runs real Android in a virtual machine on the Windows Hypervisor Platform.");
            break;
    }

    // Footer: these settings are read at start-up, so say so next to the save button.
    if (saved_page) {
        y += 18;
        const bool dirty = !(edit_ == saved_);
        Text(dc, RECT{rx, y, rx + rw - 170, y + 38}, L"Changes apply the next time Aetheron starts.", font_small_, kDim,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const RECT b = {rx + rw - 150, y, rx + rw, y + 38};
        FillRound(dc, b, 10, dirty ? StateColor(190, kPrimary, kPrimaryHover, kDim) : kControl);
        Text(dc, b, L"Save changes", font_bold_, dirty ? kOnPrimary : kDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (dirty) AddHot(b, 190, [this] { Command(kUiSaveSettings); }, L"");
    }
}

void ShellUi::Paint(HDC window_dc) {
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    EnsureFonts();
    HDC dc = CreateCompatibleDC(window_dc);
    HBITMAP bmp = CreateCompatibleBitmap(window_dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    hots_.clear();
    animating_ = false;
    Fill(dc, rc, kContent);
    if (settings_open_) DrawSettings(dc, ContentRect());
    if (bars_) DrawBars(dc, rc);
    BitBlt(window_dc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    // Repaint at ~60 Hz only while a toggle is still sliding.
    if (animating_ && !timer_on_) {
        SetTimer(hwnd_, kAnimTimer, 15, nullptr);
        timer_on_ = true;
    } else if (!animating_ && timer_on_) {
        KillTimer(hwnd_, kAnimTimer);
        timer_on_ = false;
    }
}

}  // namespace Aetheron
