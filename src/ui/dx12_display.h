// Presents the guest's virtio-gpu scanout in a window with Direct3D 12.
// The swap chain matches the guest resolution; DXGI stretches it to the
// window (the owner keeps that window at the guest's aspect ratio). While the
// boot screen is on, it is drawn instead of the guest scanout.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../devices/virtio_gpu.h"

namespace Aetheron {

// A labelled circle drawn over the Android picture (key-mapping hints).
struct OverlayMarker {
    float x, y;     // centre, fractions of the screen
    float radius;   // fraction of the screen height
    std::wstring label;
};

class Dx12Display {
public:
    ~Dx12Display();
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height, std::string* error);
    void Start(ScanoutBuffer* scanout);
    void Stop();
    // Boot screen with a progress bar; `percent` and `text` come from real boot stages.
    void SetSplash(bool show, int percent, const std::wstring& text);
    // Posts `msg` (wParam = width, lParam = height) to `target` when the guest resolution changes.
    void SetOverlay(std::vector<OverlayMarker> markers, float opacity = 1.0f);
    void SetLayoutNotify(HWND target, UINT msg) {
        notify_hwnd_ = target;
        notify_msg_ = msg;
    }

private:
    void DrawSplash(uint8_t* dst);
    void BuildOverlay();
    void BlendOverlay(uint8_t* dst);
    bool ResizeTo(uint32_t w, uint32_t h);
    void RenderLoop();
    bool PresentFrame();
    void WaitIdle();

    HWND hwnd_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    uint32_t pitch_ = 0;
    ScanoutBuffer* scanout_ = nullptr;
    uint64_t last_frame_ = ~0ULL;
    uint32_t idle_frames_ = 0;
    std::vector<OverlayMarker> overlay_;              // guarded by splash_mutex_
    std::atomic<uint64_t> overlay_version_{0};
    uint64_t overlay_built_ = ~0ULL;
    float overlay_opacity_ = 1.0f;                    // guarded by splash_mutex_
    uint32_t disc_alpha_ = 150, text_alpha_ = 255;
    uint32_t overlay_w_ = 0, overlay_h_ = 0;
    std::vector<uint8_t> overlay_disc_, overlay_text_;  // coverage masks, width*height
    std::vector<RECT> overlay_rects_;                 // where the masks are not empty
    HWND notify_hwnd_ = nullptr;
    UINT notify_msg_ = 0;
    std::mutex splash_mutex_;
    std::wstring splash_text_;
    int splash_percent_ = 0;
    std::atomic<bool> splash_on_{false};
    std::atomic<uint64_t> splash_version_{1};

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> targets_[2];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    uint64_t fence_value_ = 0;

    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace Aetheron
