#include "dx12_display.h"

#include <cstring>

#include "../common/log.h"

using Microsoft::WRL::ComPtr;

namespace Aetheron {

Dx12Display::~Dx12Display() {
    Stop();
    if (fence_event_) CloseHandle(fence_event_);
}

bool Dx12Display::Initialize(HWND hwnd, uint32_t width, uint32_t height, std::string* error) {
    hwnd_ = hwnd;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr)) {
        *error = "D3D12CreateDevice failed";
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) {
        *error = "CreateCommandQueue failed";
        return false;
    }
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        *error = "CreateDXGIFactory1 failed";
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = 2;
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        *error = "CreateSwapChainForHwnd failed";
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_));
    device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr, IID_PPV_ARGS(&list_));
    list_->Close();
    device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    width_ = height_ = 0;
    if (!ResizeTo(width, height)) {
        *error = "creating swap chain buffers failed";
        return false;
    }
    return true;
}

void Dx12Display::WaitIdle() {
    uint64_t v = ++fence_value_;
    queue_->Signal(fence_.Get(), v);
    if (fence_->GetCompletedValue() < v) {
        fence_->SetEventOnCompletion(v, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

bool Dx12Display::ResizeTo(uint32_t w, uint32_t h) {
    if (w == width_ && h == height_ && upload_) return true;
    WaitIdle();
    for (auto& t : targets_) t.Reset();
    upload_.Reset();
    if (width_ && FAILED(swap_->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) return false;
    for (UINT i = 0; i < 2; ++i)
        if (FAILED(swap_->GetBuffer(i, IID_PPV_ARGS(&targets_[i])))) return false;
    width_ = w;
    height_ = h;
    pitch_ = (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = uint64_t(pitch_) * h;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&upload_))))
        return false;
    void* p = nullptr;
    D3D12_RANGE none = {0, 0};
    if (SUCCEEDED(upload_->Map(0, &none, &p))) {
        std::memset(p, 0, size_t(pitch_) * h);  // black, alpha 0 is ignored by the flip model
        upload_->Unmap(0, nullptr);
    }
    Log("display: swap chain %ux%u", w, h);
    return true;
}

void Dx12Display::Start(ScanoutBuffer* scanout) {
    scanout_ = scanout;
    running_ = true;
    thread_ = std::thread(&Dx12Display::RenderLoop, this);
}

void Dx12Display::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Dx12Display::RenderLoop() {
    while (running_) {
        if (!PresentFrame()) Sleep(16);
    }
}

bool Dx12Display::PresentFrame() {
    {
        std::lock_guard<std::mutex> lock(scanout_->mutex);
        if (scanout_->enabled && scanout_->width && scanout_->height &&
            (scanout_->width != width_ || scanout_->height != height_)) {
            if (!ResizeTo(scanout_->width, scanout_->height)) return false;
            last_frame_ = ~0ULL;
        }
        uint64_t frame = scanout_->frame.load();
        if (frame != last_frame_) {
            void* p = nullptr;
            D3D12_RANGE none = {0, 0};
            if (FAILED(upload_->Map(0, &none, &p))) return false;
            uint8_t* dst = static_cast<uint8_t*>(p);
            if (scanout_->enabled && scanout_->width == width_ && scanout_->height == height_) {
                for (uint32_t y = 0; y < height_; ++y)
                    std::memcpy(dst + size_t(y) * pitch_, scanout_->bgra.data() + size_t(y) * width_, width_ * 4);
            } else {
                std::memset(dst, 0, size_t(pitch_) * height_);
            }
            upload_->Unmap(0, nullptr);
            last_frame_ = frame;
        }
    }

    UINT idx = swap_->GetCurrentBackBufferIndex();
    alloc_->Reset();
    list_->Reset(alloc_.Get(), nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = targets_[idx].Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list_->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = targets_[idx].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = width_;
    src.PlacedFootprint.Footprint.Height = height_;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch_;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list_->ResourceBarrier(1, &b);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    HRESULT hr = swap_->Present(1, 0);  // vsync paces the loop
    WaitIdle();
    return SUCCEEDED(hr);
}

}  // namespace Aetheron
