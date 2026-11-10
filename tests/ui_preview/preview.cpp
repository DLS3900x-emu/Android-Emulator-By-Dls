// Developer tool: paints Aetheron's window chrome into .bmp files without
// starting a VM, so the look can be checked (also under Wine):
//   x86_64-w64-mingw32-g++-posix -std=c++17 -DUNICODE -D_UNICODE -DNOMINMAX -I../../src preview.cpp \
//       ../../src/ui/shell_ui.cpp -o preview.exe -static -lgdiplus -lgdi32 -luser32
#include <windows.h>

#include <cstdio>
#include <vector>

#include "ui/gdiplus_util.h"
#include "ui/shell_ui.h"

using namespace Aetheron;

static void Save(ShellUi& ui, int w, int h, const char* path) {
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
    ui.Paint(dc);
    ui.Paint(dc);  // second pass: hints for hovered items are known after the first
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
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, w, h, HWND_MESSAGE, nullptr, nullptr, nullptr);  // message-only: needs no desktop
    if (!hwnd) return 1;
    ShellUi ui;
    ui.Attach(hwnd);
    ui.SetStatus(L"guest running: Android boot completed");
    UiControls c;
    c.dpad = true;
    c.taps = {L"F   presses the spot at 85%, 60%", L"SPACE   presses the spot at 90%, 90%"};
    ui.SetControls(c);
    UiSettings s;
    s.cpus = 4;
    Save(ui, w, h, "chrome.bmp");
    for (int page = 0; page < 3; ++page) {
        ui.OpenSettings(s, page);
        ui.MouseMove(POINT{w - 30, 150});  // hover a sidebar button
        char name[32];
        snprintf(name, sizeof(name), "settings%d.bmp", page);
        Save(ui, w, h, name);
    }
    return 0;
}
