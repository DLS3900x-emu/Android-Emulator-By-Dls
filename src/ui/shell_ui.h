// Aetheron's window chrome: top bar (title, navigation, window buttons), the
// tool sidebar and the settings page. Everything is drawn with GDI into the
// main window; clicks are reported to it as WM_COMMAND with a UiCmd id.
#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Aetheron {

enum UiCmd : UINT {
    kUiCollectLogs = 101,
    kUiShowConsole = 102,
    kUiOpenFolder = 103,
    kUiExit = 104,
    kUiWipe = 105,
    kUiBack = 120,
    kUiHome,
    kUiRecents,
    kUiFullscreen,
    kUiScreenshot,
    kUiSettings,
    kUiMenu,
    kUiMinimize,
    kUiMaximize,
    kUiSaveSettings,
    kUiInstallApk,
};

// The values the settings page edits. All of them are read at start-up.
struct UiSettings {
    uint64_t ram_mb = 4096;
    uint32_t cpus = 1;
    uint32_t width = 1280, height = 720, dpi = 160;
    bool network = true, boot_splash = true, first_boot_setup = true;
    bool operator==(const UiSettings& o) const {
        return ram_mb == o.ram_mb && cpus == o.cpus && width == o.width && height == o.height && dpi == o.dpi && network == o.network &&
               boot_splash == o.boot_splash && first_boot_setup == o.first_boot_setup;
    }
};

class ShellUi {
public:
    static constexpr int kTopH = 40, kSideW = 46, kResizeBorder = 6;

    void Attach(HWND hwnd) { hwnd_ = hwnd; }
    void SetBarsVisible(bool visible);
    // Client-area rectangle left for the Android screen (or the settings page).
    RECT ContentRect() const;

    void Paint(HDC window_dc);
    LRESULT HitTest(HWND hwnd, POINT client_pt, bool resizable) const;
    void MouseMove(POINT pt);
    void MouseLeave();
    void MouseDown(POINT pt);
    void MouseUp(POINT pt);

    void SetStatus(const std::wstring& text);

    void OpenSettings(const UiSettings& current);
    void CloseSettings();
    bool settings_open() const { return settings_open_; }
    const UiSettings& settings() const { return edit_; }
    void MarkSaved() { saved_ = edit_; Redraw(); }

private:
    struct Hot {
        RECT r;
        int key;
        std::function<void()> act;
        std::wstring hint;
    };
    void Redraw();
    void Command(UINT id);
    const Hot* HotAt(POINT pt) const;
    void EnsureFonts();
    void Fill(HDC dc, const RECT& r, COLORREF c);
    void Text(HDC dc, const RECT& r, const std::wstring& s, HFONT f, COLORREF c, UINT flags);
    void AddHot(const RECT& r, int key, std::function<void()> act, const std::wstring& hint);
    COLORREF StateColor(int key, COLORREF normal) const;
    void IconButton(HDC dc, const RECT& r, int key, wchar_t glyph, const std::wstring& hint, UINT cmd,
                    bool danger = false);
    void DrawBars(HDC dc, const RECT& client);
    void DrawSettings(HDC dc, const RECT& area);

    HWND hwnd_ = nullptr;
    bool bars_ = true;
    std::vector<Hot> hots_;
    int hover_ = -1, pressed_ = -1;
    std::wstring status_;
    HFONT font_ui_ = nullptr, font_bold_ = nullptr, font_big_ = nullptr, font_icon_ = nullptr, font_small_ = nullptr;

    bool settings_open_ = false;
    int page_ = 0;
    UiSettings edit_, saved_;
};

}  // namespace Aetheron
