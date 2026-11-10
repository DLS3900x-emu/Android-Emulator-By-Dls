#include "serial_console.h"

#include <commctrl.h>

namespace Aetheron {

namespace {
constexpr UINT_PTR kTimer = 1;
constexpr int kMaxChars = 1 << 20;  // keep the last ~1 MB of output
SerialConsole* g_console = nullptr;
}  // namespace

bool SerialConsole::Create(HINSTANCE inst, std::function<void(const std::string&)> on_input) {
    g_console = this;
    on_input_ = std::move(on_input);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = HBRUSH(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AetheronSerialConsole";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Aetheron - serial console (ttyS0)", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 900, 520, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH,
                        L"Consolas");
    output_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0, 0,
                              0, 0, hwnd_, nullptr, inst, nullptr);
    input_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                             hwnd_, nullptr, inst, nullptr);
    SendMessageW(output_, WM_SETFONT, WPARAM(font_), TRUE);
    SendMessageW(input_, WM_SETFONT, WPARAM(font_), TRUE);
    SendMessageW(output_, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(input_, EM_SETCUEBANNER, TRUE, LPARAM(L"Type a command for the Android console and press Enter"));
    input_orig_ = WNDPROC(SetWindowLongPtrW(input_, GWLP_WNDPROC, LONG_PTR(InputProc)));
    Layout();
    SetTimer(hwnd_, kTimer, 50, nullptr);
    return true;
}

void SerialConsole::Show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void SerialConsole::Append(char c) {
    // Drop ANSI escape sequences (ESC [ ... letter) and other control bytes.
    if (esc_state_ == 1) {
        esc_state_ = c == '[' ? 2 : 0;
        return;
    }
    if (esc_state_ == 2) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) esc_state_ = 0;
        return;
    }
    if (c == 0x1B) {
        esc_state_ = 1;
        return;
    }
    if (c == '\r' || (c != '\n' && c != '\t' && uint8_t(c) < 0x20)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (c == '\n') pending_ += '\r';
    pending_ += c;
}

void SerialConsole::Flush() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        text.swap(pending_);
    }
    if (text.empty() || !output_) return;
    int len = GetWindowTextLengthW(output_);
    if (len > kMaxChars) {
        SendMessageW(output_, EM_SETSEL, 0, len / 2);
        SendMessageW(output_, EM_REPLACESEL, FALSE, LPARAM(L""));
        len = GetWindowTextLengthW(output_);
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), &w[0], n);
    SendMessageW(output_, EM_SETSEL, len, len);
    SendMessageW(output_, EM_REPLACESEL, FALSE, LPARAM(w.c_str()));
}

void SerialConsole::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int ih = 26;
    MoveWindow(output_, 0, 0, rc.right, rc.bottom - ih, TRUE);
    MoveWindow(input_, 0, rc.bottom - ih, rc.right, ih, TRUE);
}

LRESULT CALLBACK SerialConsole::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    SerialConsole* self = g_console;
    switch (m) {
        case WM_SIZE:
            if (self && self->output_) self->Layout();
            return 0;
        case WM_TIMER:
            if (self) self->Flush();
            return 0;
        case WM_CLOSE:
            ShowWindow(h, SW_HIDE);  // keep collecting output; reopen from the menu
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK SerialConsole::InputProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    SerialConsole* self = g_console;
    if (m == WM_CHAR && w == '\r') {
        int n = GetWindowTextLengthW(h);
        std::wstring text(n, 0);
        if (n) GetWindowTextW(h, &text[0], n + 1);
        SetWindowTextW(h, L"");
        int u = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), n, nullptr, 0, nullptr, nullptr);
        std::string s(u, 0);
        if (u) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), n, &s[0], u, nullptr, nullptr);
        if (self && self->on_input_) self->on_input_(s + "\n");
        return 0;
    }
    return CallWindowProcW(self->input_orig_, h, m, w, l);
}

}  // namespace Aetheron
