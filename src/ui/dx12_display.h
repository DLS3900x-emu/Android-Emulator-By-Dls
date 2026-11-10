// Presents the guest's virtio-gpu scanout in a window with Direct3D 12.
// The swap chain matches the guest resolution; DXGI stretches it to the
// window. Until the guest sets a scanout, the window is plain black.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>

#include "../devices/virtio_gpu.h"

namespace Aetheron {

class Dx12Display {
public:
    ~Dx12Display();
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height, std::string* error);
    void Start(ScanoutBuffer* scanout);
    void Stop();

private:
    bool ResizeTo(uint32_t w, uint32_t h);
    void RenderLoop();
    bool PresentFrame();
    void WaitIdle();

    HWND hwnd_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    uint32_t pitch_ = 0;
    ScanoutBuffer* scanout_ = nullptr;
    uint64_t last_frame_ = ~0ULL;

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
