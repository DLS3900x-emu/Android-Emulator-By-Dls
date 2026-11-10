// Developer tool: paints Aetheron's window chrome into .bmp files without
// starting a VM, so the look can be checked (also under Wine):
//   x86_64-w64-mingw32-g++-posix -std=c++17 -DUNICODE -D_UNICODE -DNOMINMAX -I../../src preview.cpp \
//       ../../src/ui/shell_ui.cpp ../../src/ui/controls_ui.cpp -o preview.exe -static -lgdiplus -lgdi32 -luser32 -ldwmapi
#include <windows.h>

#include <cstdio>
#include <functional>

#include "ui/controls_ui.h"
#include "ui/gdiplus_util.h"
#include "ui/shell_ui.h"

using namespace Aetheron;

static void Save(int w, int h, const char* path, const std::function<void(HDC)>& paint) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(dc, bmp);
    paint(dc);
    paint(dc);  // second pass: hints for hovered items are known after the first
    GdiFlush();
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + w * h * 4;
    FILE* f = fopen(path, "wb");
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(bits, 4, size_t(w) * h, f);
    fclose(f);
    DeleteObject(bmp);
    DeleteDC(dc);
}

int main() {
    EnsureGdiplus();
    const int w = 1180, h = 700;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"AetheronPreview";
    RegisterClassW(&wc);
    // message-only: needs no desktop
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, w, h, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!hwnd) return 1;
    ShellUi ui;
    ui.Attach(hwnd);
    ui.SetStatus(L"guest running: Android boot completed");
    auto paint = [&](HDC dc) { ui.Paint(dc); };
    Save(w, h, "chrome.bmp", paint);
    ui.SetEditor(true);
    ui.MouseMove(POINT{w - 48 - 60, 200});  // hover an editor tile
    Save(w, h, "editor.bmp", paint);
    ui.SetEditor(false);
    UiSettings s;
    s.cpus = 4;
    ui.OpenSettings(s);
    Save(w, h, "settings.bmp", paint);

    ControlsPopup popup;  // painted without its window
    popup.SetState(true, false, 0.7f);
    Save(ControlsPopup::kW, ControlsPopup::kH, "popup.bmp", [&](HDC dc) { popup.Paint(dc); });
    return 0;
}
