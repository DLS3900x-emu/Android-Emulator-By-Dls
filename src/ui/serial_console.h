// A window showing the guest's serial console (ttyS0) live, with an input
// line: typed commands are sent to the guest (Android's console shell).
#pragma once
#include <windows.h>

#include <functional>
#include <mutex>
#include <string>

namespace Aetheron {

class SerialConsole {
public:
    bool Create(HINSTANCE inst, std::function<void(const std::string&)> on_input);
    void Show();
    // Thread-safe; text is shown on the next UI refresh (every 50 ms).
    void Append(char c);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM);
    void Flush();
    void Layout();

    HWND hwnd_ = nullptr, output_ = nullptr, input_ = nullptr;
    HFONT font_ = nullptr;
    WNDPROC input_orig_ = nullptr;
    std::function<void(const std::string&)> on_input_;
    std::mutex mutex_;
    std::string pending_;
    int esc_state_ = 0;  // strips ANSI escape sequences
};

}  // namespace Aetheron
