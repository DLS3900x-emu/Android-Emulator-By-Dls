#include "dx12_display.h"

#include <cstring>

#include "../common/log.h"
#include "gdiplus_util.h"

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
    if (notify_hwnd_) PostMessageW(notify_hwnd_, notify_msg_, w, h);
    return true;
}

void Dx12Display::SetSplash(bool show, int percent, const std::wstring& text) {
    std::lock_guard<std::mutex> lock(splash_mutex_);
    if (show == splash_on_ && percent == splash_percent_ && text == splash_text_) return;
    splash_text_ = text;
    splash_percent_ = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    splash_on_ = show;
    ++splash_version_;
}

void Dx12Display::SetOverlay(std::vector<OverlayMarker> markers) {
    std::lock_guard<std::mutex> lock(splash_mutex_);
    overlay_ = std::move(markers);
    ++overlay_version_;
}

// Renders the markers once into two coverage masks (discs and labels), so
// that drawing them on each frame is only a blend inside small rectangles.
void Dx12Display::BuildOverlay() {
    std::vector<OverlayMarker> markers;
    {
        std::lock_guard<std::mutex> lock(splash_mutex_);
        markers = overlay_;
        overlay_built_ = overlay_version_;
    }
    overlay_w_ = width_;
    overlay_h_ = height_;
    overlay_rects_.clear();
    if (markers.empty()) return;
    const int w = int(width_), h = int(height_);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp) {
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    const size_t n = size_t(w) * h;
    auto grab = [&](std::vector<uint8_t>* mask) {
        GdiFlush();
        mask->resize(n);
        const uint8_t* p = static_cast<const uint8_t*>(bits);
        for (size_t i = 0; i < n; ++i) (*mask)[i] = p[i * 4 + 1];  // green channel of white-on-black
    };
    std::memset(bits, 0, n * 4);
    for (const OverlayMarker& m : markers) FillCircle(dc, m.x * w, m.y * h, m.radius * h, RGB(255, 255, 255));
    grab(&overlay_disc_);

    std::memset(bits, 0, n * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT font = CreateFontW(-(h / 30 > 12 ? h / 30 : 12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    for (const OverlayMarker& m : markers) {
        const int cx = int(m.x * w), cy = int(m.y * h), r = int(m.radius * h) + 2;
        RECT box = {cx - r, cy - r, cx + r, cy + r};
        DrawTextW(dc, m.label.c_str(), -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        // The label may be wider than its disc; blend a generous box around it.
        RECT area = {cx - r - w / 16, cy - r, cx + r + w / 16, cy + r};
        area.left = area.left < 0 ? 0 : area.left;
        area.top = area.top < 0 ? 0 : area.top;
        area.right = area.right > w ? w : area.right;
        area.bottom = area.bottom > h ? h : area.bottom;
        if (area.right > area.left && area.bottom > area.top) overlay_rects_.push_back(area);
    }
    grab(&overlay_text_);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void Dx12Display::BlendOverlay(uint8_t* dst) {
    for (const RECT& r : overlay_rects_) {
        for (int y = r.top; y < r.bottom; ++y) {
            uint8_t* row = dst + size_t(y) * pitch_;
            const size_t base = size_t(y) * overlay_w_;
            for (int x = r.left; x < r.right; ++x) {
                const uint32_t disc = overlay_disc_[base + x] * 150u / 255u, text = overlay_text_[base + x];
                if (!disc && !text) continue;
                uint8_t* p = row + size_t(x) * 4;
                for (int c = 0; c < 3; ++c) {
                    uint32_t v = (p[c] * (255u - disc) + 22u * disc) / 255u;  // darken under the disc
                    v = (v * (255u - text) + 255u * text) / 255u;             // white label
                    p[c] = uint8_t(v);
                }
            }
        }
    }
}

// Renders the boot screen with GDI into the upload buffer (BGRA rows).
void Dx12Display::DrawSplash(uint8_t* dst) {
    std::wstring text;
    int percent;
    {
        std::lock_guard<std::mutex> lock(splash_mutex_);
        text = splash_text_;
        percent = splash_percent_;
    }
    const int w = int(width_), h = int(height_);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp) {
        if (dc) DeleteDC(dc);
        std::memset(dst, 0, size_t(pitch_) * height_);
        return;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    auto fill = [&](int x, int y, int cx, int cy, COLORREF c) {
        RECT r = {x, y, x + cx, y + cy};
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
    };
    auto font = [&](int px, int weight) {
        return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    fill(0, 0, w, h, RGB(11, 13, 18));
    SetBkMode(dc, TRANSPARENT);

    HFONT title = font(h / 18, FW_LIGHT);
    HGDIOBJ old_font = SelectObject(dc, title);
    SetTextColor(dc, RGB(240, 243, 250));
    RECT tr = {0, h * 38 / 100, w, h * 50 / 100};
    DrawTextW(dc, L"A E T H E R O N", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const int bw = w * 46 / 100, bh = h / 320 > 3 ? h / 320 : 3, bx = (w - bw) / 2, by = h * 55 / 100;
    fill(bx, by, bw, bh, RGB(38, 42, 52));
    fill(bx, by, bw * percent / 100, bh, RGB(120, 170, 255));

    HFONT small = font(h / 56, FW_NORMAL);
    SelectObject(dc, small);
    SetTextColor(dc, RGB(140, 148, 165));
    RECT sr = {w / 12, by + bh + h / 50, w - w / 12, by + bh + h / 50 + h / 8};
    DrawTextW(dc, text.c_str(), -1, &sr, DT_CENTER | DT_WORDBREAK);

    GdiFlush();
    for (int y = 0; y < h; ++y)
        std::memcpy(dst + size_t(y) * pitch_, static_cast<const uint8_t*>(bits) + size_t(y) * w * 4, size_t(w) * 4);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(title);
    DeleteObject(small);
    DeleteObject(bmp);
    DeleteDC(dc);
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
    bool idle = false;
    {
        std::lock_guard<std::mutex> lock(scanout_->mutex);
        if (scanout_->enabled && scanout_->width && scanout_->height &&
            (scanout_->width != width_ || scanout_->height != height_)) {
            if (!ResizeTo(scanout_->width, scanout_->height)) return false;
            last_frame_ = ~0ULL;
        }
        const bool splash = splash_on_;
        // The top bit keeps boot-screen versions distinct from guest frame numbers.
        // The overlay version is part of the number so that a changed overlay redraws a still picture.
        const uint64_t frame = splash ? (0x8000000000000000ULL | splash_version_.load())
                                      : scanout_->frame.load() + (overlay_version_.load() << 40);
        // Nothing new to show: skip the copy and the present (re-present now
        // and then so a resized window never keeps a stale picture for long).
        idle = frame == last_frame_ && ++idle_frames_ < 120;
        if (!idle) idle_frames_ = 0;
        if (frame != last_frame_) {
            void* p = nullptr;
            D3D12_RANGE none = {0, 0};
            if (FAILED(upload_->Map(0, &none, &p))) return false;
            uint8_t* dst = static_cast<uint8_t*>(p);
            if (splash) {
                DrawSplash(dst);
            } else if (scanout_->enabled && scanout_->width == width_ && scanout_->height == height_) {
                for (uint32_t y = 0; y < height_; ++y)
                    std::memcpy(dst + size_t(y) * pitch_, scanout_->bgra.data() + size_t(y) * width_, width_ * 4);
                if (overlay_built_ != overlay_version_ || overlay_w_ != width_ || overlay_h_ != height_) BuildOverlay();
                if (!overlay_rects_.empty()) BlendOverlay(dst);
            } else {
                std::memset(dst, 0, size_t(pitch_) * height_);
            }
            upload_->Unmap(0, nullptr);
            last_frame_ = frame;
        }
    }

    if (idle) {
        Sleep(4);  // outside the scanout lock
        return true;
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
