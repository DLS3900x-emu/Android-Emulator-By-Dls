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

#include "../devices/virtio_gpu.h"

namespace Aetheron {

class Dx12Display {
public:
    ~Dx12Display();
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height, std::string* error);
    void Start(ScanoutBuffer* scanout);
    void Stop();
    // Boot screen with a progress bar; `percent` and `text` come from real boot stages.
    void SetSplash(bool show, int percent, const std::wstring& text);
    // Posts `msg` (wParam = width, lParam = height) to `target` when the guest resolution changes.
    void SetLayoutNotify(HWND target, UINT msg) {
        notify_hwnd_ = target;
        notify_msg_ = msg;
    }

private:
    void DrawSplash(uint8_t* dst);
    bool ResizeTo(uint32_t w, uint32_t h);
    void RenderLoop();
    bool PresentFrame();
    void WaitIdle();

    HWND hwnd_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    uint32_t pitch_ = 0;
    ScanoutBuffer* scanout_ = nullptr;
    uint64_t last_frame_ = ~0ULL;
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
